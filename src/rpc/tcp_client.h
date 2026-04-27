#ifndef TCP_CLIENT_H
#define TCP_CLIENT_H

#include <string>


class TcpClient {
public:
    static bool sendMessage(const std::string& ip,
                        int port,
                        const std::string& request,
                        std::string& response);
};

#endif