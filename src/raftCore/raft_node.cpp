#include "raft_node.h"
#include "../rpc/tcp_client.h"
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <ctime>
#include <thread>
#include <chrono>

std::string nodeStateToString(NodeState state) {
    switch (state) {
        case NodeState::FOLLOWER: return "FOLLOWER";
        case NodeState::CANDIDATE: return "CANDIDATE";
        case NodeState::LEADER: return "LEADER";
        default: return "UNKNOWN";
    }
}

RaftNode::RaftNode(int nodeId)
    : id_(nodeId),
      selfPort_(0),
      currentTerm_(0),
      votedFor_(-1),
      commitIndex_(0),
      lastApplied_(0),
      state_(NodeState::FOLLOWER),
      leaderId_(-1),
      running_(false),
      electionTimeoutMs_(1500 + std::rand() % 1500),
      lastIncludedIndex_(0),
      lastIncludedTerm_(0),
      snapshotThreshold_(5),
    votingMember_(true),
    removed_(false),
    leaving_(false),
    storage_("raft_" + std::to_string(nodeId)),
      configState_(ConfigState::STABLE){
    lastHeartbeatTime_ = std::chrono::steady_clock::now();
    kvStore_.open("./db/leveldb_node_" + std::to_string(nodeId));
    
    loadPersistentState();
    
}

void RaftNode::setPeers(const std::vector<PeerInfo>& peers) {
    peers_ = peers;
    currentConfig_.nodes.clear();
    currentConfig_.nodes.push_back(PeerInfo(id_, selfIp_, selfPort_));
    for (const auto& p : peers_) {
        currentConfig_.nodes.push_back(p);
    }
}

void RaftNode::setSelfAddress(const std::string& ip, int port) {
    selfIp_ = ip;
    selfPort_ = port;
}

int RaftNode::getId() const {
    return id_;
}

int RaftNode::getCurrentTerm() const {
    std::lock_guard<std::mutex> lock(mu_);
    return currentTerm_;
}

void RaftNode::startElection() {
    RequestVoteArgs args;
    int termSnapshot = 0;
    std::vector<int> grantedNodeIds;

    {
        std::lock_guard<std::mutex> lock(mu_);

        // 被移除、正在移除、非投票成员都不能发起选举
        if (removed_ || leaving_ || !votingMember_) {
            return;
        }

        // 自己不在当前配置里，不能参与选举
        if (!isNodeInCurrentVotingConfig(id_)) {
            votingMember_ = false;
            return;
        }

        // 切换为 Candidate：
        // currentTerm_++，votedFor_=id_
        becomeCandidate();

        termSnapshot = currentTerm_;

        args.term = currentTerm_;
        args.candidateId = id_;
        args.lastLogIndex = getLastLogIndex();
        args.lastLogTerm = getLastLogTerm();

        // 自己给自己投票
        grantedNodeIds.push_back(id_);
    }

    // 向所有 peers 请求投票
    for (const auto& peer : peers_) {
        // 字符串协议版本
        std::string req = buildRequestVoteRpc(args);
        std::string resp;

        bool ok = TcpClient::sendMessage(peer.ip, peer.port, req, resp);
        if (!ok) {
            continue;
        }

        RequestVoteReply reply;
        if (!parseRequestVoteReply(resp, reply)) {
            continue;
        }

        std::lock_guard<std::mutex> lock(mu_);

        if (state_ != NodeState::CANDIDATE ||
            currentTerm_ != termSnapshot) {
            return;
        }

        if (reply.term > currentTerm_) {
            becomeFollower(reply.term);
            return;
        }

        if (reply.voteGranted) {
            grantedNodeIds.push_back(peer.id);

            std::cout << "[Node " << id_
                      << "] receives vote from Node "
                      << peer.id
                      << " in term "
                      << currentTerm_ << std::endl;

            if (hasElectionMajority(grantedNodeIds)) {
                becomeLeader();
                return;
            }
        }
    }
}

void RaftNode::start() {
    running_ = true;
    resetElectionTimer();
    electionThread_ = std::thread(&RaftNode::electionLoop, this);
    heartbeatThread_ = std::thread(&RaftNode::heartbeatLoop, this);
}

void RaftNode::stop() {
    running_ = false;
    if (electionThread_.joinable()) electionThread_.join();
    if (heartbeatThread_.joinable()) heartbeatThread_.join();
}

void RaftNode::becomeFollower(int newTerm) {
    state_ = NodeState::FOLLOWER;
    currentTerm_ = newTerm;
    votedFor_ = -1;
    leaderId_ = -1;
    resetElectionTimer();

    persist();

    std::cout << "[Node " << id_ << "] becomes FOLLOWER, term=" << currentTerm_ << std::endl;
}

void RaftNode::becomeCandidate() {
    state_ = NodeState::CANDIDATE;
    currentTerm_++;
    votedFor_ = id_;
    leaderId_ = -1;
    resetElectionTimer();

    persist();
    std::cout << "[Node " << id_ << "] becomes CANDIDATE, term=" << currentTerm_ << std::endl;
}

void RaftNode::becomeLeader() {
    state_ = NodeState::LEADER;
    leaderId_ = id_;

    initLeaderState();
    std::cout << "[Node " << id_ << "] becomes LEADER, term=" << currentTerm_ << std::endl;
}

bool RaftNode::isCandidateLogUpToDate(int candidateLastLogIndex, int candidateLastLogTerm) const {
    int myLastTerm = getLastLogTerm();
    int myLastIndex = getLastLogIndex();

    if (candidateLastLogTerm != myLastTerm) {
        return candidateLastLogTerm > myLastTerm;
    }

    return candidateLastLogIndex >= myLastIndex;
}

void RaftNode::resetElectionTimer() {
    lastHeartbeatTime_ = std::chrono::steady_clock::now();
    electionTimeoutMs_ = 1500 + std::rand() % 1500;
}

bool RaftNode::isElectionTimeout() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastHeartbeatTime_).count();
    return elapsed >= electionTimeoutMs_;
}

void RaftNode::electionLoop() {
    while (running_) {
        bool shouldStartElection = false;

        {
            std::lock_guard<std::mutex> lock(mu_);

            // 已被移除的节点不能选举
            if (removed_) {
                goto sleep_label;
            }

            // 正在被移除的节点不能选举
            if (leaving_) {
                goto sleep_label;
            }

            // learner / 非投票成员不能选举
            if (!votingMember_) {
                goto sleep_label;
            }

            // 自己不在当前投票配置里，也不能选举
            if (!isNodeInCurrentVotingConfig(id_)) {
                votingMember_ = false;
                goto sleep_label;
            }

            if (state_ != NodeState::LEADER && isElectionTimeout()) {
                std::cout << "[Node " << id_ << "] election timeout" << std::endl;
                shouldStartElection = true;
            }
        }

        if (shouldStartElection) {
            startElection();
        }

sleep_label:
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void RaftNode::heartbeatLoop() {
    while (running_) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (state_ == NodeState::LEADER) {
                // do nothing here
            } else {
                goto hb_sleep;
            }
        }

        sendHeartbeats();

hb_sleep:
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void RaftNode::sendHeartbeats() {
    for (size_t i = 0; i < peers_.size(); i++) {
        replicateToPeer(i);
    }

    // 每轮复制后，Leader 再尝试推进 commitIndex
    updateCommitIndex();
}

RequestVoteReply RaftNode::handleRequestVote(const RequestVoteArgs& args) {
    std::lock_guard<std::mutex> lock(mu_);

    RequestVoteReply reply;
    reply.term = currentTerm_;
    reply.voteGranted = false;

    if (!votingMember_) {
        return reply;
    }

    if (!isNodeInCurrentVotingConfig(args.candidateId)) {
        static int rejectCount = 0;
        rejectCount++;

        if (rejectCount % 20 == 1) {
            std::cout << "[Node " << id_ << "] reject vote for Node "
                  << args.candidateId
                  << ": candidate not in config"
                  << std::endl;
        }

        return reply;
    }

    if (args.term < currentTerm_) {
        return reply;
    }

    if (args.term > currentTerm_) {
        becomeFollower(args.term);
    }

    bool canVote = (votedFor_ == -1 || votedFor_ == args.candidateId);
    bool logOk = isCandidateLogUpToDate(args.lastLogIndex, args.lastLogTerm);

    if (canVote && logOk) {
        votedFor_ = args.candidateId;
        reply.voteGranted = true;
        reply.term = currentTerm_;
        resetElectionTimer();

        persist();

        std::cout << "[Node " << id_ << "] votes for Node "
                  << args.candidateId << " in term " << currentTerm_ << std::endl;
    }

    return reply;
}

std::string RaftNode::buildRequestVoteRpc(const RequestVoteArgs& args) const {
    std::ostringstream oss;
    oss << "RV "
        << args.term << " "
        << args.candidateId << " "
        << args.lastLogIndex << " "
        << args.lastLogTerm;
    return oss.str();
}

std::string RaftNode::buildAppendEntriesRpc(const AppendEntriesArgs& args) const {
    std::ostringstream oss;

    oss << "AE "
        << args.term << " "
        << args.leaderId << " "
        << args.prevLogIndex << " "
        << args.prevLogTerm << " "
        << args.leaderCommit << " "
        << args.entries.size() << "\n";

    for (const auto& e : args.entries) {
        oss << e.index << " "
            << e.term << " "
            << static_cast<int>(e.op) << " "
            << e.key << " "
            << e.value << " "
            << encodeConfig(e.oldConfig) << " "
            << encodeConfig(e.newConfig) << "\n";
    }

    return oss.str();
}

bool RaftNode::parseRequestVoteRpc(const std::string& req, RequestVoteArgs& args) const {
    std::istringstream iss(req);
    std::string type;
    iss >> type;
    if (type != "RV") return false;
    iss >> args.term >> args.candidateId >> args.lastLogIndex >> args.lastLogTerm;
    return !iss.fail();
}

bool RaftNode::parseAppendEntriesRpc(const std::string& req, AppendEntriesArgs& args) const {
    std::istringstream iss(req);

    std::string type;
    size_t entryCount = 0;

    iss >> type;
    if (type != "AE") return false;

    iss >> args.term
        >> args.leaderId
        >> args.prevLogIndex
        >> args.prevLogTerm
        >> args.leaderCommit
        >> entryCount;

    if (iss.fail()) return false;

    args.entries.clear();

    for (size_t i = 0; i < entryCount; i++) {
        LogEntry e;
        int opInt = 0;

        iss >> e.index >> e.term >> opInt >> e.key >> e.value;
        if (iss.fail()) return false;

        e.op = static_cast<CommandType>(opInt);
        e.oldConfig = decodeConfig(iss);
        e.newConfig = decodeConfig(iss);
        args.entries.push_back(e);
    }

    return true;
}

std::string RaftNode::buildRequestVoteReply(const RequestVoteReply& reply) const {
    std::ostringstream oss;
    oss << "RVR " << reply.term << " " << (reply.voteGranted ? 1 : 0);
    return oss.str();
}

std::string RaftNode::buildAppendEntriesReply(const AppendEntriesReply& reply) const {
    std::ostringstream oss;
    oss << "AER " << reply.term << " " << (reply.success ? 1 : 0);
    return oss.str();
}

bool RaftNode::parseRequestVoteReply(const std::string& resp, RequestVoteReply& reply) const {
    std::istringstream iss(resp);
    std::string type;
    int granted = 0;
    iss >> type >> reply.term >> granted;
    if (type != "RVR") return false;
    reply.voteGranted = (granted == 1);
    return !iss.fail();
}

bool RaftNode::parseAppendEntriesReply(const std::string& resp, AppendEntriesReply& reply) const {
    std::istringstream iss(resp);
    std::string type;
    int success = 0;
    iss >> type >> reply.term >> success;
    if (type != "AER") return false;
    reply.success = (success == 1);
    return !iss.fail();
}

std::string RaftNode::handleRpcText(const std::string& req) {
    {
        std::istringstream iss(req);
        std::string type, key, value;
        iss >> type;
        if (type == "CPUT") {
            iss >> key >> value;
            if (!iss.fail()) {
                return handleClientPut(key, value);
            }
            return "ERR";
        }
    }

    {
        std::istringstream iss(req);
        std::string type, key;
        iss >> type;
        if (type == "CGET") {
            iss >> key;
            if (!iss.fail()) {
                return handleClientGet(key);
            }
            return "ERR";
        }
    }

    {
        std::istringstream iss(req);
        std::string type;
        iss >> type;

        if (type == "CADD") {
            int nid = 0;
            int port = 0;
            std::string ip;

            iss >> nid >> ip >> port;

            if (iss.fail()) {
                return "ERR";
            }

            {
                std::lock_guard<std::mutex> lock(mu_);
                if (state_ != NodeState::LEADER) {
                    return "NOT_LEADER";
                }
            }

            bool ok = addNode(PeerInfo(nid, ip, port));
            return ok ? "OK" : "FAIL";
        }

        if (type == "CREMOVE") {
            int nid = 0;
            iss >> nid;

            if (iss.fail()) {
                return "ERR";
            }

            {
                std::lock_guard<std::mutex> lock(mu_);
                if (state_ != NodeState::LEADER) {
                    return "NOT_LEADER";
                }
            }

            bool ok = removeNode(nid);
            return ok ? "OK" : "FAIL";
        }
    }

    {
        std::istringstream iss(req);
        std::string type;
        iss >> type;

        if (type == "CDEL") {
            std::string key;
            iss >> key;

            if (iss.fail()) {
                return "ERR";
            }

            return handleClientDelete(key);
        }
    }

    RequestVoteArgs rvArgs;
    if (parseRequestVoteRpc(req, rvArgs)) {
        RequestVoteReply reply = handleRequestVote(rvArgs);
        return buildRequestVoteReply(reply);
    }

    AppendEntriesArgs aeArgs;
    if (parseAppendEntriesRpc(req, aeArgs)) {
        AppendEntriesReply reply = handleAppendEntries(aeArgs);
        return buildAppendEntriesReply(reply);
    }

    InstallSnapshotArgs isArgs;
    if (parseInstallSnapshotRpc(req, isArgs)) {
        InstallSnapshotReply reply = handleInstallSnapshot(isArgs);
        return buildInstallSnapshotReply(reply);
    }

    return "ERR";
}

void RaftNode::printStatus() const {
    std::lock_guard<std::mutex> lock(mu_);

    std::cout << "Node " << id_
              << " | state=" << nodeStateToString(state_)
              << " | term=" << currentTerm_
              << " | votedFor=" << votedFor_
              << " | leaderId=" << leaderId_
              << " | commitIndex=" << commitIndex_
              << " | lastApplied=" << lastApplied_
              << " | lastIncludedIndex=" << lastIncludedIndex_
              << " | lastIncludedTerm=" << lastIncludedTerm_
              << " | logs=" << logs_.size()
              << std::endl;

    if (state_ == NodeState::LEADER) {
        std::cout << "  nextIndex: ";
        for (int x : nextIndex_) {
            std::cout << x << " ";
        }
        std::cout << std::endl;

        std::cout << "  matchIndex: ";
        for (int x : matchIndex_) {
            std::cout << x << " ";
        }
        std::cout << std::endl;
    }
}

AppendEntriesReply RaftNode::handleAppendEntries(const AppendEntriesArgs& args) {
    std::lock_guard<std::mutex> lock(mu_);

    AppendEntriesReply reply;
    reply.term = currentTerm_;
    reply.success = false;

    if (!isNodeInCurrentVotingConfig(args.leaderId)) {
        std::cout << "[Node " << id_ << "] reject AppendEntries from Node "
              << args.leaderId
              << ": leader not in config" << std::endl;
        return reply;
    }

    if (args.term < currentTerm_) {
        return reply;
    }

    if (args.term > currentTerm_ || state_ != NodeState::FOLLOWER) {
        becomeFollower(args.term);
    }

    leaderId_ = args.leaderId;
    resetElectionTimer();

    // leader 的 prevLogIndex 已经被本地 snapshot 覆盖
    // 这种情况下，不能用本地 logs_ 校验该位置
    if (args.prevLogIndex < lastIncludedIndex_) {
        // 简化处理：认为这个请求太旧，返回成功但不追加这些过旧日志
        reply.term = currentTerm_;
        reply.success = true;
        return reply;
    }

    // 检查 prevLogIndex 是否存在
    if (!hasLogAt(args.prevLogIndex)) {
        return reply;
    }

    // 检查 prevLogTerm
    int localPrevTerm = getLogTermAt(args.prevLogIndex);
    if (localPrevTerm != args.prevLogTerm) {
        return reply;
    }

    bool logChanged = false;

    for (const auto& newEntry : args.entries) {
        // 如果 leader 发来的日志已经在 snapshot 里，跳过
        if (newEntry.index <= lastIncludedIndex_) {
            continue;
        }

        int localPos = logIndexToPos(newEntry.index);

        if (localPos < static_cast<int>(logs_.size())) {
            if (logs_[localPos].term != newEntry.term) {
                // 冲突：删除冲突点以及之后的日志
                logs_.resize(localPos);
                logs_.push_back(newEntry);
                logChanged = true;
            }
        } else {
            logs_.push_back(newEntry);
            logChanged = true;
        }
    }

    if (logChanged) {
        persist();
    }

    if (args.leaderCommit > commitIndex_) {
        commitIndex_ = std::min(args.leaderCommit, getLastLogIndex());
        applyCommittedLogs();
    }

    reply.term = currentTerm_;
    reply.success = true;
    return reply;
}

void RaftNode::applyCommittedLogs() {
    while (lastApplied_ < commitIndex_) {
        int nextApplyIndex = lastApplied_ + 1;

        if (nextApplyIndex <= lastIncludedIndex_) {
            lastApplied_ = nextApplyIndex;
            continue;
        }

        int pos = logIndexToPos(nextApplyIndex);
        if (pos < 0 || pos >= static_cast<int>(logs_.size())) {
            break;
        }

        const LogEntry& entry = logs_[pos];

        if (entry.op == CommandType::CONFIG_JOINT ||
            entry.op == CommandType::CONFIG_NEW) {
            applyConfigEntry(entry);
        } else {
            kvStore_.apply(entry);
        }

        lastApplied_ = nextApplyIndex;
    }

    maybeCreateSnapshot();
}

bool RaftNode::leaderPut(const std::string& key, const std::string& value) {
    {
        std::lock_guard<std::mutex> lock(mu_);

        if (state_ != NodeState::LEADER) {
            std::cout << "[Node " << id_ << "] not leader, reject put\n";
            return false;
        }

        int newIndex = getLastLogIndex() + 1;
        LogEntry newEntry(currentTerm_, newIndex, CommandType::PUT, key, value);
        logs_.push_back(newEntry);

        // 日志变化先持久化
        persist();

        std::cout << "[Node " << id_ << "] append local log: index=" << newEntry.index
                  << ", term=" << newEntry.term
                  << ", key=" << newEntry.key
                  << ", value=" << newEntry.value << std::endl;
    }

    // 立刻尝试同步到所有 follower
    for (size_t i = 0; i < peers_.size(); i++) {
        replicateToPeer(i);
    }

    // 再根据 matchIndex 推进提交
    updateCommitIndex();

    {
        std::lock_guard<std::mutex> lock(mu_);

        // 只有当前这条日志已经提交，才认为 put 成功
        return commitIndex_ >= getLastLogIndex();
    }
}

bool RaftNode::replicateLogEntry(const LogEntry& entry) {
    int successCount = 1; // Leader 自己算一个
    int termSnapshot = 0;

    {
        std::lock_guard<std::mutex> lock(mu_);
        termSnapshot = currentTerm_;
    }

    for (const auto& peer : peers_) {
        AppendEntriesArgs args;

        {
            std::lock_guard<std::mutex> lock(mu_);

            // 如果中途不再是 Leader 了，直接失败
            if (state_ != NodeState::LEADER) {
                return false;
            }

            args.term = currentTerm_;
            args.leaderId = id_;
            args.leaderCommit = commitIndex_;

            // 这里只复制“最后一条新日志”
            args.entries.clear();
            args.entries.push_back(entry);

            // prevLogIndex / prevLogTerm 指的是新日志前一条
            args.prevLogIndex = entry.index - 1;
            if (args.prevLogIndex == 0) {
                args.prevLogTerm = 0;
            } else {
                args.prevLogTerm = logs_[args.prevLogIndex - 1].term;
            }
        }

        std::string req = buildAppendEntriesRpc(args);
        std::string resp;

        bool ok = TcpClient::sendMessage(peer.ip, peer.port, req, resp);
        if (!ok) {
            continue;
        }

        AppendEntriesReply reply;
        if (!parseAppendEntriesReply(resp, reply)) {
            continue;
        }

        std::lock_guard<std::mutex> lock(mu_);

        if (state_ != NodeState::LEADER || currentTerm_ != termSnapshot) {
            return false;
        }

        // 对方 term 更大，说明自己过期
        if (reply.term > currentTerm_) {
            becomeFollower(reply.term);
            return false;
        }

        if (reply.success) {
            successCount++;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mu_);

        // 多数派复制成功，推进 commitIndex
        int totalNodes = static_cast<int>(peers_.size()) + 1;
        if (successCount > totalNodes / 2) {
            commitIndex_ = entry.index;
            applyCommittedLogs();

            std::cout << "[Node " << id_ << "] commit log index="
                      << entry.index << std::endl;

            return true;
        }
    }

    return false;
}

void RaftNode::printLogs() const {
    std::lock_guard<std::mutex> lock(mu_);

    std::cout << "---- Node " << id_ << " logs ----" << std::endl;
    std::cout << "snapshot: lastIncludedIndex=" << lastIncludedIndex_
              << ", lastIncludedTerm=" << lastIncludedTerm_ << std::endl;

    if (logs_.empty()) {
        std::cout << "(empty)" << std::endl;
        return;
    }

    for (const auto& e : logs_) {
        std::cout << "[index=" << e.index
                  << ", term=" << e.term
                  << ", op=" << static_cast<int>(e.op)
                  << ", key=" << e.key
                  << ", value=" << e.value
                  << "]" << std::endl;
    }
}

std::string RaftNode::handleClientPut(const std::string& key, const std::string& value) {
    {
        std::lock_guard<std::mutex> lock(mu_);

        // 只有 Leader 能处理写请求
        // 如果当前节点不是 Leader，直接返回 NOT_LEADER
        if (state_ != NodeState::LEADER) {
            return "NOT_LEADER";
        }
    }

    // 真正的写入流程走 leaderPut：
    // 1. 本地追加日志
    // 2. 复制到 follower
    // 3. 多数派成功后提交并 apply
    bool ok = leaderPut(key, value);

    if (ok) {
        return "OK";
    }

    return "FAIL";
}

std::string RaftNode::handleClientGet(const std::string& key) {
    std::string value;

    ReadResult result = linearizableGet(key, value);

    switch (result) {
        case ReadResult::OK:
            return "VALUE " + value;

        case ReadResult::NO_KEY:
            return "NO_KEY";

        case ReadResult::NOT_LEADER:
            return "NOT_LEADER";

        case ReadResult::READ_FAIL:
        default:
            return "READ_FAIL";
    }
}

void RaftNode::persist() {
    bool ok = storage_.save(currentTerm_,
                            votedFor_,
                            lastIncludedIndex_,
                            lastIncludedTerm_,
                            logs_);
    if (!ok) {
        std::cerr << "[Node " << id_ << "] persist failed\n";
    }
}

void RaftNode::initLeaderState() {
    int lastIndex = getLastLogIndex();

    nextIndex_.assign(peers_.size(), lastIndex + 1);
    matchIndex_.assign(peers_.size(), 0);

    // 对 Leader 自己来说，已经匹配到最后一条日志
    // 注意：这里 matchIndex_ 只存 follower，不存自己
}

bool RaftNode::replicateToPeer(size_t peerIdx) {
    bool needSnapshot = false;

    {
        std::lock_guard<std::mutex> lock(mu_);

        if (state_ != NodeState::LEADER || peerIdx >= peers_.size()) {
            return false;
        }

        if (nextIndex_[peerIdx] <= lastIncludedIndex_) {
            needSnapshot = true;
        }
    }

    if (needSnapshot) {
        return sendSnapshotToPeer(peerIdx);
    }

    PeerInfo peer;
    AppendEntriesArgs args;
    int termSnapshot = 0;

    {
        std::lock_guard<std::mutex> lock(mu_);

        if (state_ != NodeState::LEADER) {
            return false;
        }

        peer = peers_[peerIdx];
        termSnapshot = currentTerm_;

        args.term = currentTerm_;
        args.leaderId = id_;
        args.leaderCommit = commitIndex_;

        int nextIdx = nextIndex_[peerIdx];

        args.prevLogIndex = nextIdx - 1;
        args.prevLogTerm = getLogTermAt(args.prevLogIndex);

        args.entries.clear();

        for (int index = nextIdx; index <= getLastLogIndex(); index++) {
            int pos = logIndexToPos(index);
            if (pos >= 0 && pos < static_cast<int>(logs_.size())) {
                args.entries.push_back(logs_[pos]);
            }
        }
    }

    // 字符串协议版本
    std::string req = buildAppendEntriesRpc(args);
    std::string resp;

    bool ok = TcpClient::sendMessage(peer.ip, peer.port, req, resp);
    if (!ok) {
        return false;
    }

    AppendEntriesReply reply;
    if (!parseAppendEntriesReply(resp, reply)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mu_);

        if (state_ != NodeState::LEADER || currentTerm_ != termSnapshot) {
            return false;
        }

        if (reply.term > currentTerm_) {
            becomeFollower(reply.term);
            return false;
        }

        if (reply.success) {
            int lastSentIndex =
                args.prevLogIndex + static_cast<int>(args.entries.size());

            matchIndex_[peerIdx] = std::max(matchIndex_[peerIdx], lastSentIndex);
            nextIndex_[peerIdx] = matchIndex_[peerIdx] + 1;

            return true;
        } else {
            if (nextIndex_[peerIdx] > lastIncludedIndex_ + 1) {
                nextIndex_[peerIdx]--;
            } else {
                nextIndex_[peerIdx] = lastIncludedIndex_;
            }

            return false;
        }
    }
}

void RaftNode::updateCommitIndex() {
    std::lock_guard<std::mutex> lock(mu_);

    if (state_ != NodeState::LEADER) {
        return;
    }

    int lastIndex = getLastLogIndex();

    for (int N = lastIndex; N > commitIndex_; N--) {
        if (getLogTermAt(N) != currentTerm_) {
            continue;
        }

        int pos = logIndexToPos(N);
        if (pos < 0 || pos >= static_cast<int>(logs_.size())) {
            continue;
        }

        const LogEntry& entry = logs_[pos];

        if (isMajorityMatchedForEntry(entry)) {
            commitIndex_ = N;
            applyCommittedLogs();

            std::cout << "[Node " << id_ << "] update commitIndex to "
                      << commitIndex_ << std::endl;
            break;
        }
    }
}

int RaftNode::logIndexToPos(int index) const {
    return index - lastIncludedIndex_ - 1;
}

bool RaftNode::hasLogAt(int index) const {
    if (index == lastIncludedIndex_) {
        return true;
    }

    int pos = logIndexToPos(index);
    return pos >= 0 && pos < static_cast<int>(logs_.size());
}

int RaftNode::getLogTermAt(int index) const {
    if (index == 0) {
        return 0;
    }

    if (index == lastIncludedIndex_) {
        return lastIncludedTerm_;
    }

    int pos = logIndexToPos(index);
    if (pos < 0 || pos >= static_cast<int>(logs_.size())) {
        return -1;
    }

    return logs_[pos].term;
}

int RaftNode::getLastLogIndex() const {
    if (logs_.empty()) {
        return lastIncludedIndex_;
    }
    return logs_.back().index;
}

int RaftNode::getLastLogTerm() const {
    if (logs_.empty()) {
        return lastIncludedTerm_;
    }
    return logs_.back().term;
}

void RaftNode::maybeCreateSnapshot() {
    // 关键：联合配置阶段不要做 snapshot
    // 否则 snapshot 如果没有保存配置状态，会导致恢复后配置错乱
    if (configState_ == ConfigState::JOINT) {
        return;
    }

    if (lastApplied_ - lastIncludedIndex_ >= snapshotThreshold_) {
        createSnapshot(lastApplied_);
    }
}

void RaftNode::createSnapshot(int uptoIndex) {
    if (uptoIndex <= lastIncludedIndex_) {
        return;
    }

    if (uptoIndex > lastApplied_) {
        return;
    }

    if (configState_ == ConfigState::JOINT) {
        return;
    }

    int term = getLogTermAt(uptoIndex);
    if (term < 0) {
        std::cerr << "[Node " << id_
                  << "] create snapshot failed, term not found\n";
        return;
    }

    std::vector<LogEntry> newLogs;
    for (const auto& e : logs_) {
        if (e.index > uptoIndex) {
            newLogs.push_back(e);
        }
    }

    logs_.swap(newLogs);

    lastIncludedIndex_ = uptoIndex;
    lastIncludedTerm_ = term;

    persist();

    std::cout << "[Node " << id_
              << "] create snapshot metadata success"
              << ", lastIncludedIndex=" << lastIncludedIndex_
              << ", lastIncludedTerm=" << lastIncludedTerm_
              << ", remainLogs=" << logs_.size()
              << std::endl;
}

void RaftNode::loadPersistentState() {
    int savedTerm = 0;
    int savedVotedFor = -1;
    int savedLastIncludedIndex = 0;
    int savedLastIncludedTerm = 0;
    std::vector<LogEntry> savedLogs;

    bool ok = storage_.load(savedTerm,
                            savedVotedFor,
                            savedLastIncludedIndex,
                            savedLastIncludedTerm,
                            savedLogs);

    if (!ok) {
        std::cout << "[Node " << id_ << "] no previous persistent state\n";
        return;
    }

    currentTerm_ = savedTerm;
    votedFor_ = savedVotedFor;
    lastIncludedIndex_ = savedLastIncludedIndex;
    lastIncludedTerm_ = savedLastIncludedTerm;

    logs_.clear();
    for (const auto& e : savedLogs) {
        if (e.index > lastIncludedIndex_) {
            logs_.push_back(e);
        }
    }

    commitIndex_ = lastIncludedIndex_;
    lastApplied_ = lastIncludedIndex_;

    std::cout << "[Node " << id_ << "] load persistent state success"
              << ", term=" << currentTerm_
              << ", votedFor=" << votedFor_
              << ", lastIncludedIndex=" << lastIncludedIndex_
              << ", lastIncludedTerm=" << lastIncludedTerm_
              << ", logs=" << logs_.size()
              << std::endl;
}

bool RaftNode::sendSnapshotToPeer(size_t peerIdx) {
    PeerInfo peer;
    InstallSnapshotArgs args;
    int termSnapshot = 0;

    {
        std::lock_guard<std::mutex> lock(mu_);

        if (state_ != NodeState::LEADER || peerIdx >= peers_.size()) {
            return false;
        }

        peer = peers_[peerIdx];
        termSnapshot = currentTerm_;

        args.term = currentTerm_;
        args.leaderId = id_;
        args.lastIncludedIndex = lastIncludedIndex_;
        args.lastIncludedTerm = lastIncludedTerm_;
        args.kvData = kvStore_.dump();
    }

    std::string req = buildInstallSnapshotRpc(args);
    std::string resp;

    bool ok = TcpClient::sendMessage(peer.ip, peer.port, req, resp);
    if (!ok) {
        return false;
    }

    InstallSnapshotReply reply;
    if (!parseInstallSnapshotReply(resp, reply)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mu_);

        if (state_ != NodeState::LEADER || currentTerm_ != termSnapshot) {
            return false;
        }

        if (reply.term > currentTerm_) {
            becomeFollower(reply.term);
            return false;
        }

        // follower 已安装到 lastIncludedIndex_
        matchIndex_[peerIdx] = std::max(matchIndex_[peerIdx], lastIncludedIndex_);
        nextIndex_[peerIdx] = lastIncludedIndex_ + 1;

        std::cout << "[Node " << id_ << "] send snapshot to peer "
                  << peer.id << " success, lastIncludedIndex="
                  << lastIncludedIndex_ << std::endl;

        return true;
    }
}

InstallSnapshotReply RaftNode::handleInstallSnapshot(const InstallSnapshotArgs& args) {
    std::lock_guard<std::mutex> lock(mu_);

    InstallSnapshotReply reply;
    reply.term = currentTerm_;

    // 1. leader 任期比自己小，拒绝安装快照
    if (args.term < currentTerm_) {
        return reply;
    }

    // 2. 如果 leader 任期更新，或者当前节点不是 follower，则转为 follower
    if (args.term > currentTerm_ || state_ != NodeState::FOLLOWER) {
        becomeFollower(args.term);
    }

    leaderId_ = args.leaderId;
    resetElectionTimer();

    // 3. 如果收到的快照不比本地新，则忽略
    if (args.lastIncludedIndex <= lastIncludedIndex_) {
        reply.term = currentTerm_;
        return reply;
    }

    // 4. 安装状态机快照
    // 使用 LevelDB 时，快照数据直接写入 LevelDB
    kvStore_.loadFromSnapshot(args.kvData);

    // 5. 删除已经被快照覆盖的日志
    std::vector<LogEntry> remainLogs;
    for (const auto& e : logs_) {
        if (e.index > args.lastIncludedIndex) {
            remainLogs.push_back(e);
        }
    }
    logs_.swap(remainLogs);

    // 6. 更新快照元数据
    lastIncludedIndex_ = args.lastIncludedIndex;
    lastIncludedTerm_ = args.lastIncludedTerm;

    // 7. 更新提交位置和应用位置
    commitIndex_ = std::max(commitIndex_, lastIncludedIndex_);
    lastApplied_ = std::max(lastApplied_, lastIncludedIndex_);

    // 8. 持久化 Raft 元数据
    // 注意：这里不再写 raft_x_snapshot.txt
    // LevelDB 保存 KV 状态机数据，raft_x_state.txt 保存 lastIncludedIndex/Term 和剩余日志
    persist();

    reply.term = currentTerm_;

    std::cout << "[Node " << id_ << "] install snapshot success, lastIncludedIndex="
              << lastIncludedIndex_
              << ", lastIncludedTerm=" << lastIncludedTerm_
              << ", remainLogs=" << logs_.size()
              << std::endl;

    return reply;
}

std::string RaftNode::buildInstallSnapshotRpc(const InstallSnapshotArgs& args) const {
    std::ostringstream oss;

    oss << "IS "
        << args.term << " "
        << args.leaderId << " "
        << args.lastIncludedIndex << " "
        << args.lastIncludedTerm << " "
        << args.kvData.size() << "\n";

    for (const auto& p : args.kvData) {
        oss << p.first << " " << p.second << "\n";
    }

    return oss.str();
}

bool RaftNode::parseInstallSnapshotRpc(const std::string& req, InstallSnapshotArgs& args) const {
    std::istringstream iss(req);

    std::string type;
    size_t kvCount = 0;

    iss >> type;
    if (type != "IS") return false;

    iss >> args.term
        >> args.leaderId
        >> args.lastIncludedIndex
        >> args.lastIncludedTerm
        >> kvCount;

    if (iss.fail()) return false;

    args.kvData.clear();

    for (size_t i = 0; i < kvCount; i++) {
        std::string key, value;
        iss >> key >> value;
        if (iss.fail()) return false;
        args.kvData[key] = value;
    }

    return true;
}

std::string RaftNode::buildInstallSnapshotReply(const InstallSnapshotReply& reply) const {
    std::ostringstream oss;
    oss << "ISR " << reply.term;
    return oss.str();
}

bool RaftNode::parseInstallSnapshotReply(const std::string& resp, InstallSnapshotReply& reply) const {
    std::istringstream iss(resp);

    std::string type;
    iss >> type >> reply.term;

    if (type != "ISR") return false;
    return !iss.fail();
}

bool RaftNode::ensureCurrentTermCommitted() {
    {
        std::lock_guard<std::mutex> lock(mu_);

        // 只有 leader 才需要做这件事
        if (state_ != NodeState::LEADER) {
            return false;
        }

        // 如果当前 commitIndex 对应的日志就是当前任期，
        // 说明当前任期已经有日志被提交，可以直接读。
        if (commitIndex_ > lastIncludedIndex_ &&
            getLogTermAt(commitIndex_) == currentTerm_) {
            return true;
        }
    }

    LogEntry noopEntry;

    {
        std::lock_guard<std::mutex> lock(mu_);

        if (state_ != NodeState::LEADER) {
            return false;
        }

        // 构造一条 no-op 日志
        // 它不会修改 KVStore，只用于证明当前 leader 的任期已经提交过日志
        int newIndex = getLastLogIndex() + 1;
        noopEntry = LogEntry(currentTerm_, newIndex, CommandType::NONE,
                             "__noop__", "__noop__");

        logs_.push_back(noopEntry);

        // 日志变更必须持久化
        persist();

        std::cout << "[Node " << id_ << "] append noop log for ReadIndex, index="
                  << noopEntry.index << ", term=" << noopEntry.term << std::endl;
    }

    // 尝试复制 no-op 日志到 follower
    for (size_t i = 0; i < peers_.size(); i++) {
        replicateToPeer(i);
    }

    // 根据 matchIndex 推进 commitIndex
    updateCommitIndex();

    {
        std::lock_guard<std::mutex> lock(mu_);

        // 再次确认这条当前任期日志是否已经提交
        if (commitIndex_ >= noopEntry.index &&
            getLogTermAt(commitIndex_) == currentTerm_) {
            return true;
        }
    }

    return false;
}

bool RaftNode::sendReadIndexHeartbeats(int termSnapshot) {
    int successCount = 1;  // leader 自己算一个成功
    int totalNodes = static_cast<int>(peers_.size()) + 1;

    for (const auto& peer : peers_) {
        AppendEntriesArgs args;

        {
            std::lock_guard<std::mutex> lock(mu_);

            // 如果身份或任期发生变化，说明这次读已经不安全
            if (state_ != NodeState::LEADER || currentTerm_ != termSnapshot) {
                return false;
            }

            // 构造一条空 AppendEntries
            // 这里不携带日志，只用于确认 leader 身份
            args.term = currentTerm_;
            args.leaderId = id_;
            args.leaderCommit = commitIndex_;
            args.prevLogIndex = getLastLogIndex();
            args.prevLogTerm = getLastLogTerm();
            args.entries.clear();
        }

        std::string req = buildAppendEntriesRpc(args);
        std::string resp;

        bool ok = TcpClient::sendMessage(peer.ip, peer.port, req, resp);
        if (!ok) {
            continue;
        }

        AppendEntriesReply reply;
        if (!parseAppendEntriesReply(resp, reply)) {
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(mu_);

            // 如果发现更大的 term，说明自己已经过期，退回 follower
            if (reply.term > currentTerm_) {
                becomeFollower(reply.term);
                return false;
            }

            if (state_ != NodeState::LEADER || currentTerm_ != termSnapshot) {
                return false;
            }

            if (reply.success) {
                successCount++;
            }
        }

        // 已经达到多数派，可以提前返回
        if (successCount > totalNodes / 2) {
            return true;
        }
    }

    return successCount > totalNodes / 2;
}

bool RaftNode::waitAppliedTo(int readIndex, int termSnapshot) {
    const int maxWaitMs = 1000;
    const int sleepMs = 10;
    int waitedMs = 0;

    while (waitedMs < maxWaitMs) {
        {
            std::lock_guard<std::mutex> lock(mu_);

            // 如果中途不再是 leader，读失败
            if (state_ != NodeState::LEADER || currentTerm_ != termSnapshot) {
                return false;
            }

            // 状态机已经执行到 readIndex，可以安全读取
            if (lastApplied_ >= readIndex) {
                return true;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
        waitedMs += sleepMs;
    }

    return false;
}

ReadResult RaftNode::linearizableGet(const std::string& key, std::string& value) {
    int termSnapshot = 0;
    int readIndex = 0;

    {
        std::lock_guard<std::mutex> lock(mu_);

        if (state_ != NodeState::LEADER) {
            return ReadResult::NOT_LEADER;
        }

        termSnapshot = currentTerm_;
    }

    if (!ensureCurrentTermCommitted()) {
        return ReadResult::READ_FAIL;
    }

    {
        std::lock_guard<std::mutex> lock(mu_);

        if (state_ != NodeState::LEADER || currentTerm_ != termSnapshot) {
            return ReadResult::NOT_LEADER;
        }

        readIndex = commitIndex_;
    }

    if (!sendReadIndexHeartbeats(termSnapshot)) {
        return ReadResult::READ_FAIL;
    }

    if (!waitAppliedTo(readIndex, termSnapshot)) {
        return ReadResult::READ_FAIL;
    }

    {
        std::lock_guard<std::mutex> lock(mu_);

        if (state_ != NodeState::LEADER || currentTerm_ != termSnapshot) {
            return ReadResult::NOT_LEADER;
        }

        if (kvStore_.get(key, value)) {
            return ReadResult::OK;
        }

        return ReadResult::NO_KEY;
    }
}

bool RaftNode::isMajorityInConfig(const ClusterConfig& config, int logIndex) const {
    int count = 0;

    for (const auto& node : config.nodes) {
        if (node.id == id_) {
            // leader 自己默认已经有该日志
            if (getLastLogIndex() >= logIndex) {
                count++;
            }
        } else {
            int peerIdx = findPeerIndexById(node.id);
            if (peerIdx >= 0 &&
                peerIdx < static_cast<int>(matchIndex_.size()) &&
                matchIndex_[peerIdx] >= logIndex) {
                count++;
            }
        }
    }

    return count > config.size() / 2;
}

int RaftNode::findPeerIndexById(int nodeId) const {
    for (size_t i = 0; i < peers_.size(); i++) {
        if (peers_[i].id == nodeId) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool RaftNode::isMajorityMatched(int logIndex) const {
    if (configState_ == ConfigState::STABLE) {
        return isMajorityInConfig(currentConfig_, logIndex);
    }

    // 联合配置阶段，必须同时满足 old 和 new 的多数派
    return isMajorityInConfig(oldConfig_, logIndex) &&
           isMajorityInConfig(newConfig_, logIndex);
}

void RaftNode::applyConfigEntry(const LogEntry& entry) {
    if (entry.op == CommandType::CONFIG_JOINT) {
        configState_ = ConfigState::JOINT;

        oldConfig_.nodes = entry.oldConfig;
        newConfig_.nodes = entry.newConfig;

        std::cout << "[Node " << id_ << "] apply CONFIG_JOINT oldConfig={";
        for (const auto& n : oldConfig_.nodes) {
            std::cout << n.id << "(" << n.ip << ":" << n.port << ") ";
        }
        std::cout << "} newConfig={";
        for (const auto& n : newConfig_.nodes) {
            std::cout << n.id << "(" << n.ip << ":" << n.port << ") ";
        }
        std::cout << "}" << std::endl;

        rebuildPeersFromJointConfig();

        // 关键：如果自己在 oldConfig 中，但不在 newConfig 中，
        // 说明自己正在被移除，不能再主动发起选举。
        if (oldConfig_.contains(id_) && !newConfig_.contains(id_)) {
            leaving_ = true;
            votingMember_ = false;   // 禁止主动选举
            state_ = NodeState::FOLLOWER;

            std::cout << "[Node " << id_
                      << "] entering leaving state, disable election"
                      << std::endl;
        }

        return;
    }

    if (entry.op == CommandType::CONFIG_NEW) {
        configState_ = ConfigState::STABLE;

        currentConfig_.nodes = entry.newConfig;
        oldConfig_.nodes.clear();
        newConfig_.nodes.clear();

        std::cout << "[Node " << id_ << "] apply CONFIG_NEW currentConfig={";
        for (const auto& n : currentConfig_.nodes) {
            std::cout << n.id << "(" << n.ip << ":" << n.port << ") ";
        }
        std::cout << "}" << std::endl;

        rebuildPeersFromCurrentConfig();

        if (currentConfig_.contains(id_)) {
            votingMember_ = true;
            leaving_ = false;
            removed_ = false;

            std::cout << "[Node " << id_
                      << "] becomes voting member"
                      << std::endl;
        } else {
            votingMember_ = false;
            leaving_ = false;
            removed_ = true;
            state_ = NodeState::FOLLOWER;
            leaderId_ = -1;

            std::cout << "[Node " << id_
                      << "] removed from cluster, stop election"
                      << std::endl;
        }

        return;
    }
}

void RaftNode::rebuildPeersFromCurrentConfig() {
    peers_.clear();

    for (const auto& n : currentConfig_.nodes) {
        if (n.id != id_) {
            peers_.push_back(n);
        }
    }

    if (state_ == NodeState::LEADER) {
        initLeaderState();
    }
}

void RaftNode::rebuildPeersFromJointConfig() {
    peers_.clear();

    auto addUnique = [this](const PeerInfo& node) {
        if (node.id == id_) return;

        for (const auto& p : peers_) {
            if (p.id == node.id) return;
        }

        peers_.push_back(node);
    };

    for (const auto& n : oldConfig_.nodes) {
        addUnique(n);
    }

    for (const auto& n : newConfig_.nodes) {
        addUnique(n);
    }

    std::cout << "[Node " << id_ << "] joint peers={";
    for (const auto& p : peers_) {
        std::cout << p.id << " ";
    }
    std::cout << "}" << std::endl;

    if (state_ == NodeState::LEADER) {
        initLeaderState();
    }
}

bool RaftNode::addNode(const PeerInfo& newNode) {
    ClusterConfig oldCfg;
    ClusterConfig newCfg;

    {
        std::lock_guard<std::mutex> lock(mu_);

        if (state_ != NodeState::LEADER) {
            return false;
        }

        if (configState_ != ConfigState::STABLE) {
            std::cout << "[Node " << id_ << "] already in joint config\n";
            return false;
        }

        if (currentConfig_.contains(newNode.id)) {
            std::cout << "[Node " << id_ << "] node already exists\n";
            return false;
        }

        oldCfg = currentConfig_;
        newCfg = currentConfig_;
        newCfg.nodes.push_back(newNode);
    }

    // 1. 追加联合配置日志
    if (!appendConfigJoint(oldCfg, newCfg)) {
        return false;
    }

    // 2. 等待联合配置 apply
    if (!waitConfigState(ConfigState::JOINT, 3000)) {
        return false;
    }

    // 3. 追加新配置日志
    if (!appendConfigNew(newCfg)) {
        return false;
    }

    // 4. 等待新配置 apply
    if (!waitConfigState(ConfigState::STABLE, 3000)) {
        return false;
    }

    return true;
}

bool RaftNode::appendConfigJoint(const ClusterConfig& oldCfg,
                                 const ClusterConfig& newCfg) {
    LogEntry entry;

    {
        std::lock_guard<std::mutex> lock(mu_);

        if (state_ != NodeState::LEADER) {
            return false;
        }

        int newIndex = getLastLogIndex() + 1;

        entry.term = currentTerm_;
        entry.index = newIndex;
        entry.op = CommandType::CONFIG_JOINT;

        // 必须设置，否则 AppendEntries 解析会错位
        entry.key = "__config_joint__";
        entry.value = "__config_joint__";

        entry.oldConfig = oldCfg.nodes;
        entry.newConfig = newCfg.nodes;

        logs_.push_back(entry);
        persist();

        std::cout << "[Node " << id_ << "] append CONFIG_JOINT log index="
                  << newIndex << std::endl;
    }

    for (size_t i = 0; i < peers_.size(); i++) {
        replicateToPeer(i);
    }

    updateCommitIndex();

    return true;
}


bool RaftNode::appendConfigNew(const ClusterConfig& newCfg) {
    LogEntry entry;

    {
        std::lock_guard<std::mutex> lock(mu_);

        if (state_ != NodeState::LEADER) {
            return false;
        }

        int newIndex = getLastLogIndex() + 1;

        entry.term = currentTerm_;
        entry.index = newIndex;
        entry.op = CommandType::CONFIG_NEW;

        // 必须设置，否则解析会错位
        entry.key = "__config_new__";
        entry.value = "__config_new__";

        entry.newConfig = newCfg.nodes;

        logs_.push_back(entry);
        persist();

        std::cout << "[Node " << id_ << "] append CONFIG_NEW log index="
                  << newIndex << std::endl;
    }

    for (size_t i = 0; i < peers_.size(); i++) {
        replicateToPeer(i);
    }

    updateCommitIndex();

    return true;
}

bool RaftNode::waitConfigState(ConfigState target, int timeoutMs) {
    int waited = 0;
    const int step = 20;

    while (waited < timeoutMs) {
        {
            std::lock_guard<std::mutex> lock(mu_);

            if (state_ != NodeState::LEADER) {
                return false;
            }

            if (configState_ == target) {
                return true;
            }
        }

        // 继续推动复制与提交
        for (size_t i = 0; i < peers_.size(); i++) {
            replicateToPeer(i);
        }

        updateCommitIndex();

        std::this_thread::sleep_for(std::chrono::milliseconds(step));
        waited += step;
    }

    return false;
}

bool RaftNode::removeNode(int removeNodeId) {
    ClusterConfig oldCfg;
    ClusterConfig newCfg;

    {
        std::lock_guard<std::mutex> lock(mu_);

        if (state_ != NodeState::LEADER) {
            return false;
        }

        if (configState_ != ConfigState::STABLE) {
            return false;
        }

        if (!currentConfig_.contains(removeNodeId)) {
            return false;
        }

        oldCfg = currentConfig_;

        for (const auto& n : currentConfig_.nodes) {
            if (n.id != removeNodeId) {
                newCfg.nodes.push_back(n);
            }
        }

        if (newCfg.nodes.empty()) {
            return false;
        }
    }

    if (!appendConfigJoint(oldCfg, newCfg)) {
        return false;
    }

    if (!waitConfigState(ConfigState::JOINT, 3000)) {
        return false;
    }

    if (!appendConfigNew(newCfg)) {
        return false;
    }

    if (!waitConfigState(ConfigState::STABLE, 3000)) {
        return false;
    }

    // 如果移除的是自己
    if (removeNodeId == id_) {
        std::cout << "[Node " << id_ << "] removed from cluster\n";
        stop();
    }

    return true;
}

std::string encodeConfig(const std::vector<PeerInfo>& nodes) {
    std::ostringstream oss;

    oss << nodes.size();

    for (const auto& n : nodes) {
        oss << " " << n.id << " " << n.ip << " " << n.port;
    }

    return oss.str();
}

std::vector<PeerInfo> decodeConfig(std::istream& iss) {
    size_t count = 0;
    iss >> count;

    std::vector<PeerInfo> nodes;

    for (size_t i = 0; i < count; i++) {
        PeerInfo p;
        iss >> p.id >> p.ip >> p.port;
        nodes.push_back(p);
    }

    return nodes;
}

bool RaftNode::hasMajorityInConfigByIds(
    const ClusterConfig& cfg,
    const std::vector<int>& grantedNodeIds) const {

    int count = 0;

    for (const auto& n : cfg.nodes) {
        if (containsNodeId(grantedNodeIds, n.id)) {
            count++;
        }
    }

    return count > cfg.size() / 2;
}

bool RaftNode::hasElectionMajority(
    const std::vector<int>& grantedNodeIds) const {

    if (configState_ == ConfigState::STABLE) {
        return hasMajorityInConfigByIds(currentConfig_, grantedNodeIds);
    }

    return hasMajorityInConfigByIds(oldConfig_, grantedNodeIds) &&
           hasMajorityInConfigByIds(newConfig_, grantedNodeIds);
}

void RaftNode::setVotingMember(bool voting) {
    std::lock_guard<std::mutex> lock(mu_);
    votingMember_ = voting;

    std::cout << "[Node " << id_ << "] votingMember="
              << (votingMember_ ? "true" : "false") << std::endl;
}

bool RaftNode::isNodeInCurrentVotingConfig(int nodeId) const {
    if (configState_ == ConfigState::STABLE) {
        return currentConfig_.contains(nodeId);
    }

    // 联合配置阶段，old/new 中任一配置包含它，才认为是合法成员
    return oldConfig_.contains(nodeId) || newConfig_.contains(nodeId);
}

bool RaftNode::isMajorityMatchedForEntry(const LogEntry& entry) const {
    if (entry.op == CommandType::CONFIG_JOINT) {
        ClusterConfig oldCfg;
        ClusterConfig newCfg;

        oldCfg.nodes = entry.oldConfig;
        newCfg.nodes = entry.newConfig;

        return isMajorityInConfig(oldCfg, entry.index) &&
               isMajorityInConfig(newCfg, entry.index);
    }

    if (configState_ == ConfigState::JOINT) {
        return isMajorityInConfig(oldConfig_, entry.index) &&
               isMajorityInConfig(newConfig_, entry.index);
    }

    return isMajorityInConfig(currentConfig_, entry.index);
}

bool RaftNode::leaderDelete(const std::string& key) {
    int deleteIndex = 0;

    {
        std::lock_guard<std::mutex> lock(mu_);

        if (state_ != NodeState::LEADER) {
            std::cout << "[Node " << id_ << "] not leader, reject delete\n";
            return false;
        }

        int newIndex = getLastLogIndex() + 1;

        LogEntry entry(currentTerm_,
                       newIndex,
                       CommandType::DEL,
                       key,
                       "__del__");

        logs_.push_back(entry);
        deleteIndex = newIndex;

        persist();

        std::cout << "[Node " << id_
                  << "] append DEL log: index=" << entry.index
                  << ", term=" << entry.term
                  << ", key=" << entry.key
                  << std::endl;
    }

    for (size_t i = 0; i < peers_.size(); i++) {
        replicateToPeer(i);
    }

    updateCommitIndex();

    {
        std::lock_guard<std::mutex> lock(mu_);
        return commitIndex_ >= deleteIndex;
    }
}

std::string RaftNode::handleClientDelete(const std::string& key) {
    {
        std::lock_guard<std::mutex> lock(mu_);

        if (state_ != NodeState::LEADER) {
            return "NOT_LEADER";
        }
    }

    bool ok = leaderDelete(key);
    return ok ? "OK" : "FAIL";
}


