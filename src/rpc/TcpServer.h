#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include <unordered_map>
#include <memory>
#include <thread>

class EventLoop;
class Channel;
class TcpConnection;
class RaftNode;

class TcpServer {
public:
    TcpServer(RaftNode* node, int port);
    ~TcpServer();

    void start();
    void stop();

private:
    void startInLoop();
    void handleAccept();

private:
    RaftNode* node_;
    int port_;
    int listenFd_;

    bool running_;
    std::unique_ptr<EventLoop> loop_;
    std::unique_ptr<Channel> acceptChannel_;
    std::thread loopThread_;

    std::unordered_map<int, std::shared_ptr<TcpConnection>> connections_;
};

#endif