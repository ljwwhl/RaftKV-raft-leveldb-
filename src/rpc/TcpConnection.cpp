#include "TcpConnection.h"
#include "EventLoop.h"
#include "../raftCore/raft_node.h"
#include "util.h"

#include <unistd.h>
#include <arpa/inet.h>
#include <cstring>
#include <iostream>

TcpConnection::TcpConnection(EventLoop* loop, int fd, RaftNode* node)
    : loop_(loop),
      fd_(fd),
      node_(node),
      channel_(new Channel(loop, fd)) {
    setNonBlocking(fd_);

    channel_->setReadCallback(std::bind(&TcpConnection::handleRead, this));
    channel_->setWriteCallback(std::bind(&TcpConnection::handleWrite, this));
    channel_->setCloseCallback(std::bind(&TcpConnection::handleClose, this));

    channel_->enableReading();
}

TcpConnection::~TcpConnection() {
    if (fd_ >= 0) {
        close(fd_);
    }
}

std::string TcpConnection::encodePacket(const std::string& body) {
    uint32_t len = static_cast<uint32_t>(body.size());
    uint32_t netLen = htonl(len);

    std::string packet;
    packet.resize(4 + body.size());

    std::memcpy(&packet[0], &netLen, 4);
    std::memcpy(&packet[4], body.data(), body.size());

    return packet;
}

bool TcpConnection::tryDecodePacket(std::string& body) {
    if (inputBuffer_.readableBytes() < 4) {
        return false;
    }

    uint32_t netLen = 0;
    std::memcpy(&netLen, inputBuffer_.peek(), 4);

    uint32_t len = ntohl(netLen);
    if (len == 0 || len > 64 * 1024 * 1024) {
        return false;
    }

    if (inputBuffer_.readableBytes() < 4 + len) {
        return false;
    }

    inputBuffer_.retrieve(4);

    body.assign(inputBuffer_.peek(), len);
    inputBuffer_.retrieve(len);

    return true;
}

void TcpConnection::handleRead() {
    bool ok = inputBuffer_.readFd(fd_);
    if (!ok) {
        handleClose();
        return;
    }

    std::string req;

    while (tryDecodePacket(req)) {
        std::string resp = node_->handleRpcText(req);

        std::string packet = encodePacket(resp);
        outputBuffer_.append(packet);

        channel_->enableWriting();
    }
}

void TcpConnection::handleWrite() {
    bool ok = outputBuffer_.writeFd(fd_);
    if (!ok) {
        handleClose();
        return;
    }

    if (outputBuffer_.readableBytes() == 0) {
        channel_->disableWriting();

        // 如果你仍然想保持短连接模式，写完后可以关闭。
        // 如果想长连接，注释掉这一行。
        handleClose();
    }
}

void TcpConnection::handleClose() {
    loop_->removeChannel(channel_.get());

    if (fd_ >= 0) {
        close(fd_);
    }

    int oldfd = fd_;
    fd_ = -1;

    if (closeCallback_) {
        closeCallback_(oldfd);
    }
}