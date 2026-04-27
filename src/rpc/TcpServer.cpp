#include "TcpServer.h"
#include "EventLoop.h"
#include "Channel.h"
#include "TcpConnection.h"
#include "util.h"

#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <cstring>
#include <iostream>

TcpServer::TcpServer(RaftNode* node, int port)
    : node_(node),
      port_(port),
      listenFd_(-1),
      running_(false),
      loop_(new EventLoop()) {}

TcpServer::~TcpServer() {
    stop();
}

void TcpServer::start() {
    running_ = true;
    loopThread_ = std::thread(&TcpServer::startInLoop, this);
}

void TcpServer::stop() {
    running_ = false;

    if (loop_) {
        loop_->quit();
    }

    if (loopThread_.joinable()) {
        loopThread_.join();
    }

    if (listenFd_ >= 0) {
        close(listenFd_);
        listenFd_ = -1;
    }
}

void TcpServer::startInLoop() {
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        std::cerr << "socket create failed\n";
        return;
    }

    setNonBlocking(listenFd_);

    int opt = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "bind failed\n";
        return;
    }

    if (listen(listenFd_, 128) < 0) {
        std::cerr << "listen failed\n";
        return;
    }

    acceptChannel_.reset(new Channel(loop_.get(), listenFd_));
    acceptChannel_->setReadCallback(std::bind(&TcpServer::handleAccept, this));
    acceptChannel_->enableReading();

    std::cout << "[Server] epoll listen on port " << port_ << std::endl;

    loop_->loop();
}

void TcpServer::handleAccept() {
    while (true) {
        sockaddr_in clientAddr;
        socklen_t len = sizeof(clientAddr);

        int connfd = accept(listenFd_, reinterpret_cast<sockaddr*>(&clientAddr), &len);

        if (connfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }

            std::cerr << "accept error\n";
            break;
        }

        setNonBlocking(connfd);

        std::shared_ptr<TcpConnection> conn(
            new TcpConnection(loop_.get(), connfd, node_));

        connections_[connfd] = conn;

        conn->setCloseCallback([this](int fd) {
            connections_.erase(fd);
        });
    }
}