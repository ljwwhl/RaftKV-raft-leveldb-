#ifndef TCP_CONNECTION_H
#define TCP_CONNECTION_H

#include "Buffer.h"
#include "Channel.h"
#include <memory>
#include <string>
#include <functional>

class EventLoop;
class RaftNode;

class TcpConnection {
public:
    TcpConnection(EventLoop* loop, int fd, RaftNode* node);
    ~TcpConnection();

    void handleRead();
    void handleWrite();
    void handleClose();

    int fd() const {
        return fd_;
    }

    using CloseCallback = std::function<void(int)>;

    void setCloseCallback(CloseCallback cb) {
        closeCallback_ = cb;
    }

    CloseCallback closeCallback_;
private:
    std::string encodePacket(const std::string& body);
    bool tryDecodePacket(std::string& body);

private:
    EventLoop* loop_;
    int fd_;
    RaftNode* node_;
    std::unique_ptr<Channel> channel_;

    Buffer inputBuffer_;
    Buffer outputBuffer_;
};

#endif