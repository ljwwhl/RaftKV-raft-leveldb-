#include <iostream>
#include <vector>
#include <thread>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

#include "raftCore/raft_node.h"
#include "rpc/TcpServer.h"

static bool loadClusterConfig(const std::string& path,
                              std::vector<PeerInfo>& allNodes) {
    std::ifstream ifs(path.c_str());
    if (!ifs.is_open()) {
        std::cerr << "open cluster config failed: " << path << std::endl;
        return false;
    }

    allNodes.clear();

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
            std::cerr << "invalid config line: " << line << std::endl;
            return false;
        }

        allNodes.push_back(PeerInfo(id, ip, port));
    }

    return true;
}

static bool findSelfNode(const std::vector<PeerInfo>& allNodes,
                         const std::string& selfIp,
                         int selfPort,
                         PeerInfo& self) {
    for (const auto& n : allNodes) {
        if (n.ip == selfIp && n.port == selfPort) {
            self = n;
            return true;
        }
    }

    return false;
}

static std::vector<PeerInfo> buildPeers(const std::vector<PeerInfo>& allNodes,
                                        int selfId) {
    std::vector<PeerInfo> peers;

    for (const auto& n : allNodes) {
        if (n.id != selfId) {
            peers.push_back(n);
        }
    }

    return peers;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cout << "Usage:\n";
        std::cout << "  ./node <self_ip> <self_port> [--learner]\n";
        std::cout << "\nExample:\n";
        std::cout << "  ./node 127.0.0.1 9001\n";
        std::cout << "  ./node 127.0.0.1 9004 --learner\n";
        return 1;
    }

    std::string configPath = "./config/cluster.conf";

    std::string selfIp = argv[1];
    int selfPort = std::atoi(argv[2]);

    bool learner = false;

    for (int i = 3; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "--learner") {
            learner = true;
        }
    }

    std::vector<PeerInfo> allNodes;
    if (!loadClusterConfig(configPath, allNodes)) {
        return 1;
    }

    PeerInfo self;
    if (!findSelfNode(allNodes, selfIp, selfPort, self)) {
        std::cerr << "self node not found in " << configPath
                  << ": " << selfIp << ":" << selfPort << std::endl;
        return 1;
    }

    int nodeId = self.id;
    std::vector<PeerInfo> peers = buildPeers(allNodes, nodeId);

    std::srand(static_cast<unsigned>(std::time(nullptr)) + nodeId);

    std::cout << "[Node " << nodeId << "] loaded from "
              << configPath << ", address="
              << selfIp << ":" << selfPort << std::endl;

    std::cout << "[Node " << nodeId << "] peers: ";
    for (const auto& p : peers) {
        std::cout << p.id << "(" << p.ip << ":" << p.port << ") ";
    }
    std::cout << std::endl;

    RaftNode node(nodeId);

    node.setSelfAddress(selfIp, selfPort);
    node.setPeers(peers);

    if (learner) {
        node.setVotingMember(false);
    }

    TcpServer server(&node, selfPort);
    server.start();

    node.start();

    std::cout << "Commands:\n";
    std::cout << "  status\n";
    std::cout << "  logs\n";

    std::string cmd;
    while (std::cin >> cmd) {
        if (cmd == "status") {
            node.printStatus();
        } else if (cmd == "logs") {
            node.printLogs();
        } else {
            std::cout << "Unknown command: " << cmd << std::endl;
        }
    }

    node.stop();
    server.stop();

    return 0;
}