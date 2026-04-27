#ifndef TYPES_H
#define TYPES_H

#include <string>
#include <vector>
#include <map>

enum class CommandType {
    PUT,
    GET,
    DEL,
    NONE,

    CONFIG_JOINT,   // 联合配置日志
    CONFIG_NEW      // 新配置日志
};

enum class NodeState {
    FOLLOWER,
    CANDIDATE,
    LEADER
};


struct PeerInfo {
    int id;
    std::string ip;
    int port;

    PeerInfo() : id(-1), port(0) {}
    PeerInfo(int nodeId, const std::string& nodeIp, int nodePort)
        : id(nodeId), ip(nodeIp), port(nodePort) {}
};

struct LogEntry {
    int term;
    int index;
    CommandType op;
    std::string key;
    std::string value;

    // 成员变更使用
    std::vector<PeerInfo> oldConfig;
    std::vector<PeerInfo> newConfig;

    LogEntry() : term(0), index(0), op(CommandType::NONE) {}

    LogEntry(int t, int i, CommandType cmd,
             const std::string& k, const std::string& v)
        : term(t), index(i), op(cmd), key(k), value(v) {}
};

struct RequestVoteArgs {
    int term;
    int candidateId;
    int lastLogIndex;
    int lastLogTerm;

    RequestVoteArgs() : term(0), candidateId(-1), lastLogIndex(0), lastLogTerm(0) {}
};

struct RequestVoteReply {
    int term;
    bool voteGranted;

    RequestVoteReply() : term(0), voteGranted(false) {}
};

struct AppendEntriesArgs {
    int term;
    int leaderId;
    int prevLogIndex;
    int prevLogTerm;
    std::vector<LogEntry> entries;
    int leaderCommit;

    AppendEntriesArgs()
        : term(0), leaderId(-1), prevLogIndex(0), prevLogTerm(0), leaderCommit(0) {}
};

struct AppendEntriesReply {
    int term;
    bool success;

    AppendEntriesReply() : term(0), success(false) {}
};


struct InstallSnapshotArgs {
    int term;
    int leaderId;
    int lastIncludedIndex;
    int lastIncludedTerm;
    std::map<std::string, std::string> kvData;

    InstallSnapshotArgs()
        : term(0),
          leaderId(-1),
          lastIncludedIndex(0),
          lastIncludedTerm(0) {}
};

struct InstallSnapshotReply {
    int term;

    InstallSnapshotReply() : term(0) {}
};

enum class ReadResult {
    OK,
    NO_KEY,
    NOT_LEADER,
    READ_FAIL
};

struct ClusterConfig {
    std::vector<PeerInfo> nodes;

    bool contains(int nodeId) const {
        for (const auto& n : nodes) {
            if (n.id == nodeId) {
                return true;
            }
        }
        return false;
    }

    int size() const {
        return static_cast<int>(nodes.size());
    }
};

enum class ConfigState {
    STABLE,     // 稳定配置
    JOINT      // 联合配置
};

#endif