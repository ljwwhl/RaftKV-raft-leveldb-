#ifndef RAFT_NODE_H
#define RAFT_NODE_H

#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <iostream>
#include "../common/types.h"
#include "../stateMachine/kv_store.h"
#include "../persistence/storage.h"

class RaftNode {
public:
    explicit RaftNode(int nodeId);

    void setPeers(const std::vector<PeerInfo>& peers);
    void setSelfAddress(const std::string& ip, int port);

    int getId() const;
    int getCurrentTerm() const;
    NodeState getState() const;

    void start();
    void stop();

    RequestVoteReply handleRequestVote(const RequestVoteArgs& args);
    AppendEntriesReply handleAppendEntries(const AppendEntriesArgs& args);

    std::string handleRpcText(const std::string& req);

    void becomeFollower(int newTerm);
    void becomeCandidate();
    void becomeLeader();

    int getLastLogIndex() const;
    int getLastLogTerm() const;
    bool isCandidateLogUpToDate(int candidateLastLogIndex, int candidateLastLogTerm) const;

    void printStatus() const;
    void printLogs() const;

    // Day 4：Leader 接收客户端 PUT 请求
    bool leaderPut(const std::string& key, const std::string& value);

    // Day 4：提交后的日志应用到状态机
    void applyCommittedLogs();

    // Day 5：处理客户端 PUT 请求
    std::string handleClientPut(const std::string& key, const std::string& value);

     // Day 5：处理客户端 GET 请求
    std::string handleClientGet(const std::string& key);

    // Day 6：从磁盘恢复持久状态
    void loadPersistentState();

    // Day 6：把当前持久状态写盘
    void persist();

    // Read Index：处理线性一致读
    ReadResult linearizableGet(const std::string& key, std::string& value);

    void setVotingMember(bool voting);

    bool leaderDelete(const std::string& key);
    std::string handleClientDelete(const std::string& key);

    

private:
    void electionLoop();
    void heartbeatLoop();

    void startElection();
    void sendHeartbeats();

    bool isElectionTimeout() const;
    void resetElectionTimer();

    std::string buildRequestVoteRpc(const RequestVoteArgs& args) const;
    std::string buildAppendEntriesRpc(const AppendEntriesArgs& args) const;

    bool parseRequestVoteRpc(const std::string& req, RequestVoteArgs& args) const;
    bool parseAppendEntriesRpc(const std::string& req, AppendEntriesArgs& args) const;

    std::string buildRequestVoteReply(const RequestVoteReply& reply) const;
    std::string buildAppendEntriesReply(const AppendEntriesReply& reply) const;

    bool parseRequestVoteReply(const std::string& resp, RequestVoteReply& reply) const;
    bool parseAppendEntriesReply(const std::string& resp, AppendEntriesReply& reply) const;

    // Day 4：向所有 follower 复制某条新日志
    bool replicateLogEntry(const LogEntry& entry);

    void initLeaderState();
    bool replicateToPeer(size_t peerIdx);
    void updateCommitIndex();

    // Snapshot：index 转 vector 下标
    int logIndexToPos(int index) const;

    // Snapshot：判断某个日志 index 是否还在 logs_ 中
    bool hasLogAt(int index) const;

    // Snapshot：获取某个日志 index 的 term
    int getLogTermAt(int index) const;

    // Snapshot：创建本地快照
    void maybeCreateSnapshot();
    void createSnapshot(int uptoIndex);

    // Snapshot：加载本地快照
    // void loadSnapshot();

    // Snapshot：处理 InstallSnapshot RPC
    InstallSnapshotReply handleInstallSnapshot(const InstallSnapshotArgs& args);

    // Snapshot：发送 InstallSnapshot 给某个 follower
    bool sendSnapshotToPeer(size_t peerIdx);

    // Snapshot 文本协议构造与解析
    std::string buildInstallSnapshotRpc(const InstallSnapshotArgs& args) const;
    bool parseInstallSnapshotRpc(const std::string& req, InstallSnapshotArgs& args) const;

    std::string buildInstallSnapshotReply(const InstallSnapshotReply& reply) const;
    bool parseInstallSnapshotReply(const std::string& resp, InstallSnapshotReply& reply) const;

    // Read Index：确保当前任期已经提交过日志
    bool ensureCurrentTermCommitted();

    // Read Index：发送一轮空 AppendEntries，确认 leader 仍被多数派认可
    bool sendReadIndexHeartbeats(int termSnapshot);

    // Read Index：等待状态机执行到指定 readIndex
    bool waitAppliedTo(int readIndex, int termSnapshot);

    //判断某个配置是否多数派复制
    bool isMajorityInConfig(const ClusterConfig& config, int logIndex) const;

    int findPeerIndexById(int nodeId) const;

    bool isMajorityMatched(int logIndex) const;//判断当前配置下某条日志是否可提交

    void applyConfigEntry(const LogEntry& entry);

    void rebuildPeersFromCurrentConfig();

    void rebuildPeersFromJointConfig();

    bool addNode(const PeerInfo& newNode);

    bool appendConfigJoint(const ClusterConfig& oldCfg, const ClusterConfig& newCfg);

    bool appendConfigNew(const ClusterConfig& newCfg);

    bool waitConfigState(ConfigState target, int timeoutMs);

    bool removeNode(int removeNodeId);

    bool hasElectionMajority(const std::vector<int>& grantedNodeIds) const;
    bool hasMajorityInConfigByIds(const ClusterConfig& cfg,
                              const std::vector<int>& grantedNodeIds) const;
    

    bool isNodeInCurrentVotingConfig(int nodeId) const;

    bool isMajorityMatchedForEntry(const LogEntry& entry) const;

private:
    int id_;
    std::string selfIp_;
    int selfPort_;

    mutable std::mutex mu_;

    int currentTerm_;
    int votedFor_;
    std::vector<LogEntry> logs_;

    int commitIndex_;
    int lastApplied_;

    NodeState state_;
    int leaderId_;

    KVStore kvStore_;

    std::vector<PeerInfo> peers_;

    std::atomic<bool> running_;
    std::thread electionThread_;
    std::thread heartbeatThread_;

    std::chrono::steady_clock::time_point lastHeartbeatTime_;
    int electionTimeoutMs_;

    PersistentStorage storage_;
    std::vector<int> nextIndex_;
    std::vector<int> matchIndex_;

    int lastIncludedIndex_;
    int lastIncludedTerm_;
    int snapshotThreshold_;

    ConfigState configState_;

    ClusterConfig currentConfig_;
    ClusterConfig oldConfig_;
    ClusterConfig newConfig_;

    bool votingMember_;
    bool removed_;
    bool leaving_;

};

std::string nodeStateToString(NodeState state);

std::string encodeConfig(const std::vector<PeerInfo>& nodes);

std::vector<PeerInfo> decodeConfig(std::istream& iss);

static bool containsNodeId(const std::vector<int>& ids, int id) {
    for (int x : ids) {
        if (x == id) {
            return true;
        }
    }
    return false;
}

static void printConfig(const std::string& name,
    const std::vector<PeerInfo>& nodes) {
    std::cout << name << "={";
    for (const auto& n : nodes) {
        std::cout << n.id << "(" << n.ip << ":" << n.port << ") ";
    }
    std::cout << "}";
}


#endif