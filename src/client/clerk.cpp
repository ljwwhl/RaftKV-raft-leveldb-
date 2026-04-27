#include "clerk.h"
#include "../rpc/tcp_client.h"
#include <iostream>
#include <fstream>
#include <sstream>

static bool loadClusterConfig(const std::string& path,
                              std::vector<PeerInfo>& nodes) {
    std::ifstream ifs(path.c_str());
    if (!ifs.is_open()) {
        std::cerr << "[Clerk] open cluster config failed: "
                  << path << std::endl;
        return false;
    }

    nodes.clear();

    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty()) {
            continue;
        }

        if (line[0] == '#') {
            continue;
        }

        std::istringstream iss(line);

        int id = 0;
        std::string ip;
        int port = 0;

        iss >> id >> ip >> port;

        if (iss.fail()) {
            std::cerr << "[Clerk] invalid config line: "
                      << line << std::endl;
            return false;
        }

        nodes.push_back(PeerInfo(id, ip, port));
    }

    return true;
}


Clerk::Clerk(const std::string& configPath)
    : configPath_(configPath),
      leaderHint_(0) {
    reloadServersFromConfig();
}

bool Clerk::put(const std::string& key, const std::string& value) {
    reloadServersFromConfig();
    if (servers_.empty()) {
        return false;
    }

    int start = leaderHint_;

    for (size_t i = 0; i < servers_.size(); i++) {
        int idx = (start + static_cast<int>(i)) %
                  static_cast<int>(servers_.size());

        const PeerInfo& server = servers_[idx];

        // 字符串协议：
        // CPUT key value
        std::string req = "CPUT " + key + " " + value;
        std::string resp;

        bool ok = TcpClient::sendMessage(server.ip, server.port, req, resp);
        if (!ok) {
            continue;
        }

        if (resp == "OK") {
            leaderHint_ = idx;
            return true;
        }

        if (resp == "NOT_LEADER") {
            continue;
        }

        if (resp == "FAIL") {
            continue;
        }
    }

    return false;
}

bool Clerk::get(const std::string& key, std::string& value) {
    reloadServersFromConfig();
    if (servers_.empty()) {
        return false;
    }

    int start = leaderHint_;

    for (size_t i = 0; i < servers_.size(); i++) {
        int idx = (start + static_cast<int>(i)) %
                  static_cast<int>(servers_.size());

        const PeerInfo& server = servers_[idx];

        // 字符串协议：
        // CGET key
        std::string req = "CGET " + key;
        std::string resp;

        bool ok = TcpClient::sendMessage(server.ip, server.port, req, resp);
        if (!ok) {
            continue;
        }

        // 返回格式：
        // VALUE xxx
        if (resp.rfind("VALUE ", 0) == 0) {
            value = resp.substr(6);
            leaderHint_ = idx;
            return true;
        }

        if (resp == "NO_KEY") {
            value.clear();
            leaderHint_ = idx;
            return false;
        }

        if (resp == "NOT_LEADER") {
            continue;
        }

        if (resp == "READ_FAIL") {
            continue;
        }
    }

    return false;
}

bool Clerk::sendAdminCommand(const std::string& req) {
    reloadServersFromConfig();

    if (servers_.empty()) {
        return false;
    }

    int start = leaderHint_;

    for (size_t i = 0; i < servers_.size(); i++) {
        int idx = (start + static_cast<int>(i)) %
                  static_cast<int>(servers_.size());

        const PeerInfo& server = servers_[idx];

        std::string resp;
        bool ok = TcpClient::sendMessage(server.ip, server.port, req, resp);

        if (!ok) {
            continue;
        }

        if (resp == "OK") {
            leaderHint_ = idx;
            reloadServersFromConfig();
            return true;
        }

        if (resp == "NOT_LEADER") {
            continue;
        }

        if (resp == "FAIL") {
            continue;
        }
    }

    return false;
}

bool Clerk::addNode(int nodeId) {
    PeerInfo newNode;

    if (!findNodeById(nodeId, newNode)) {
        std::cout << "[Clerk] node " << nodeId
                  << " not found in " << configPath_ << std::endl;
        return false;
    }

    std::string req = "CADD " +
                      std::to_string(newNode.id) + " " +
                      newNode.ip + " " +
                      std::to_string(newNode.port);

    return sendAdminCommand(req);
}

bool Clerk::removeNode(int nodeId) {
    std::string req = "CREMOVE " + std::to_string(nodeId);
    return sendAdminCommand(req);
}

bool Clerk::del(const std::string& key) {
    reloadServersFromConfig();
    if (servers_.empty()) {
        return false;
    }

    int start = leaderHint_;

    for (size_t i = 0; i < servers_.size(); i++) {
        int idx = (start + static_cast<int>(i)) %
                  static_cast<int>(servers_.size());

        const PeerInfo& server = servers_[idx];

        // 字符串协议：
        // CDEL key
        std::string req = "CDEL " + key;
        std::string resp;

        bool ok = TcpClient::sendMessage(server.ip, server.port, req, resp);
        if (!ok) {
            continue;
        }

        if (resp == "OK") {
            leaderHint_ = idx;
            return true;
        }

        if (resp == "NOT_LEADER") {
            continue;
        }

        if (resp == "FAIL") {
            continue;
        }
    }

    return false;
}

bool Clerk::reloadServersFromConfig() {
    std::vector<PeerInfo> nodes;

    if (!loadClusterConfig(configPath_, nodes)) {
        return false;
    }

    servers_ = nodes;

    if (leaderHint_ >= static_cast<int>(servers_.size())) {
        leaderHint_ = 0;
    }

    return true;
}

bool Clerk::findNodeById(int nodeId, PeerInfo& node) {
    std::vector<PeerInfo> nodes;

    if (!loadClusterConfig(configPath_, nodes)) {
        return false;
    }

    for (const auto& n : nodes) {
        if (n.id == nodeId) {
            node = n;
            return true;
        }
    }

    return false;
}



