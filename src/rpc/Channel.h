#ifndef CHANNEL_H
#define CHANNEL_H

#include <functional>
#include <sys/epoll.h>

class EventLoop;

class Channel {
public:
    using EventCallback = std::function<void()>;

    Channel(EventLoop* loop, int fd)
        : loop_(loop),
          fd_(fd),
          events_(0),
          revents_(0),
          inEpoll_(false) {}

    int fd() const {
        return fd_;
    }

    uint32_t events() const {
        return events_;
    }

    void setRevents(uint32_t revt) {
        revents_ = revt;
    }

    bool inEpoll() const {
        return inEpoll_;
    }

    void setInEpoll(bool in) {
        inEpoll_ = in;
    }

    void enableReading();
    void enableWriting();
    void disableWriting();
    void disableAll();

    bool isWriting() const {
        return events_ & EPOLLOUT;
    }

    void setReadCallback(EventCallback cb) {
        readCallback_ = cb;
    }

    void setWriteCallback(EventCallback cb) {
        writeCallback_ = cb;
    }

    void setCloseCallback(EventCallback cb) {
        closeCallback_ = cb;
    }

    void handleEvent() {
        if (revents_ & (EPOLLHUP | EPOLLERR)) {
            if (closeCallback_) closeCallback_();
            return;
        }

        if (revents_ & EPOLLIN) {
            if (readCallback_) readCallback_();
        }

        if (revents_ & EPOLLOUT) {
            if (writeCallback_) writeCallback_();
        }
    }

private:
    EventLoop* loop_;
    int fd_;
    uint32_t events_;
    uint32_t revents_;
    bool inEpoll_;

    EventCallback readCallback_;
    EventCallback writeCallback_;
    EventCallback closeCallback_;
};

#endif