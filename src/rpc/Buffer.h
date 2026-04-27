#ifndef BUFFER_H
#define BUFFER_H

#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

class Buffer {
public:
    void append(const char* data, size_t len) {
        data_.append(data, len);
    }

    void append(const std::string& s) {
        data_.append(s);
    }

    size_t readableBytes() const {
        return data_.size();
    }

    const char* peek() const {
        return data_.data();
    }

    void retrieve(size_t len) {
        data_.erase(0, len);
    }

    std::string retrieveAllAsString() {
        std::string res = data_;
        data_.clear();
        return res;
    }

    bool readFd(int fd) {
        char buf[4096];

        while (true) {
            ssize_t n = recv(fd, buf, sizeof(buf), 0);

            if (n > 0) {
                append(buf, n);
            } else if (n == 0) {
                return false;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return true;
                }
                return false;
            }
        }
    }

    bool writeFd(int fd) {
        while (!data_.empty()) {
            ssize_t n = send(fd, data_.data(), data_.size(), 0);

            if (n > 0) {
                retrieve(n);
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return true;
                }
                return false;
            }
        }

        return true;
    }

private:
    std::string data_;
};

//编码响应/请求
static std::string encodePacket(const std::string& body) {
    uint32_t len = static_cast<uint32_t>(body.size());
    uint32_t netLen = htonl(len);

    std::string packet;
    packet.resize(4 + body.size());

    std::memcpy(&packet[0], &netLen, 4);
    std::memcpy(&packet[4], body.data(), body.size());

    return packet;
}

//从 Buffer 解包
static bool tryDecodePacket(Buffer& input, std::string& body) {
    if (input.readableBytes() < 4) {
        return false;
    }

    uint32_t netLen = 0;
    std::memcpy(&netLen, input.peek(), 4);

    uint32_t len = ntohl(netLen);

    if (len == 0 || len > 64 * 1024 * 1024) {
        return false;
    }

    if (input.readableBytes() < 4 + len) {
        return false;
    }

    input.retrieve(4);

    body.assign(input.peek(), len);
    input.retrieve(len);

    return true;
}

#endif