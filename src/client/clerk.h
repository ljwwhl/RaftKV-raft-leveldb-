#ifndef CLERK_H
#define CLERK_H

#include <string>
#include <vector>
#include "../common/types.h"

class Clerk {
public:
    explicit Clerk(const std::string& configPath);

    bool put(const std::string& key, const std::string& value);
    bool get(const std::string& key, std::string& value);
    bool del(const std::string& key);

    bool addNode(int nodeId);
    bool removeNode(int nodeId);

private:
    bool reloadServersFromConfig();
    bool findNodeById(int nodeId, PeerInfo& node);
    bool sendAdminCommand(const std::string& req);

private:
    std::string configPath_;
    std::vector<PeerInfo> servers_;
    int leaderHint_;
};



#endif