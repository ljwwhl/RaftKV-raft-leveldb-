#include <iostream>
#include <string>
#include "client/clerk.h"

int main(int argc, char* argv[]) {
    std::string configPath = "./config/cluster.conf";

    Clerk clerk(configPath);

    std::cout << "Commands:\n";
    std::cout << "  put <key> <value>\n";
    std::cout << "  get <key>\n";
    std::cout << "  del <key>\n";
    std::cout << "  add <nodeId>\n";
    std::cout << "  remove <nodeId>\n";
    std::cout << "  quit\n";

    std::string cmd;

    while (std::cin >> cmd) {
        if (cmd == "put") {
            std::string key, value;
            std::cin >> key >> value;

            bool ok = clerk.put(key, value);
            std::cout << (ok ? "put success" : "put failed") << std::endl;
        } else if (cmd == "get") {
            std::string key;
            std::cin >> key;

            std::string value;
            bool ok = clerk.get(key, value);

            if (ok) {
                std::cout << "value = " << value << std::endl;
            } else {
                std::cout << "get failed or no key" << std::endl;
            }
        } else if (cmd == "del") {
            std::string key;
            std::cin >> key;

            bool ok = clerk.del(key);
            std::cout << (ok ? "delete success" : "delete failed") << std::endl;
        } else if (cmd == "add") {
            int nodeId;
            std::cin >> nodeId;

            bool ok = clerk.addNode(nodeId);
            std::cout << (ok ? "add success" : "add failed") << std::endl;
        } else if (cmd == "remove") {
            int nodeId;
            std::cin >> nodeId;

            bool ok = clerk.removeNode(nodeId);
            std::cout << (ok ? "remove success" : "remove failed") << std::endl;
        } else if (cmd == "quit") {
            break;
        } else {
            std::cout << "Unknown command: " << cmd << std::endl;
        }
    }

    return 0;
}