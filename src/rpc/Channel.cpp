#include "Channel.h"
#include "EventLoop.h"
#include <sys/epoll.h>

void Channel::enableReading() {
    events_ |= EPOLLIN;
    loop_->updateChannel(this);
}

void Channel::enableWriting() {
    events_ |= EPOLLOUT;
    loop_->updateChannel(this);
}

void Channel::disableWriting() {
    events_ &= ~EPOLLOUT;
    loop_->updateChannel(this);
}

void Channel::disableAll() {
    events_ = 0;
    loop_->updateChannel(this);
}