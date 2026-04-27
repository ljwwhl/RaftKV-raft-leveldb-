#include "tcp_client.h"

#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <cstring>
#include <iostream>

static bool writeN(int fd, const char* buf, int n) {
    int total = 0;

    while (total < n) {
        ssize_t ret = send(fd, buf + total, n - total, 0);
        if (ret <= 0) {
            return false;
        }

        total += static_cast<int>(ret);
    }

    return true;
}

static bool readN(int fd, char* buf, int n) {
    int total = 0;

    while (total < n) {
        ssize_t ret = recv(fd, buf + total, n - total, 0);
        if (ret <= 0) {
            return false;
        }

        total += static_cast<int>(ret);
    }

    return true;
}

static std::string encodePacket(const std::string& body) {
    uint32_t len = static_cast<uint32_t>(body.size());
    uint32_t netLen = htonl(len);

    std::string packet;
    packet.resize(4 + body.size());

    std::memcpy(&packet[0], &netLen, 4);
    std::memcpy(&packet[4], body.data(), body.size());

    return packet;
}

bool TcpClient::sendMessage(const std::string& ip,
                            int port,
                            const std::string& request,
                            std::string& response) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        return false;
    }

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
        close(sockfd);
        return false;
    }

    if (connect(sockfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(sockfd);
        return false;
    }

    std::string packet = encodePacket(request);

    if (!writeN(sockfd, packet.data(), static_cast<int>(packet.size()))) {
        close(sockfd);
        return false;
    }

    uint32_t netLen = 0;
    if (!readN(sockfd, reinterpret_cast<char*>(&netLen), 4)) {
        close(sockfd);
        return false;
    }

    uint32_t len = ntohl(netLen);
    if (len == 0 || len > 64 * 1024 * 1024) {
        close(sockfd);
        return false;
    }

    response.resize(len);

    if (!readN(sockfd, &response[0], len)) {
        close(sockfd);
        return false;
    }

    close(sockfd);
    return true;
}