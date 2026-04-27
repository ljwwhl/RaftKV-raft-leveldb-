#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H

#include <vector>
#include <unordered_map>
#include <sys/epoll.h>
#include <unistd.h>
#include <iostream>
#include "Channel.h"

class Channel;

class EventLoop {
public:
    EventLoop() : quit_(false) {
        epollFd_ = epoll_create1(0);
        if (epollFd_ < 0) {
            std::cerr << "epoll_create1 failed\n";
        }
    }

    ~EventLoop() {
        close(epollFd_);
    }

    void loop() {
        const int MAX_EVENTS = 1024;
        std::vector<epoll_event> events(MAX_EVENTS);

        while (!quit_) {
            int n = epoll_wait(epollFd_, events.data(), MAX_EVENTS, 1000);

            for (int i = 0; i < n; i++) {
                Channel* ch = static_cast<Channel*>(events[i].data.ptr);
                ch->setRevents(events[i].events);
                ch->handleEvent();
            }
        }
    }

    void updateChannel(Channel* channel) {
        epoll_event ev;
        ev.events = channel->events();
        ev.data.ptr = channel;

        if (!channel->inEpoll()) {
            epoll_ctl(epollFd_, EPOLL_CTL_ADD, channel->fd(), &ev);
            channel->setInEpoll(true);
        } else {
            epoll_ctl(epollFd_, EPOLL_CTL_MOD, channel->fd(), &ev);
        }
    }

    void removeChannel(Channel* channel) {
        if (channel->inEpoll()) {
            epoll_ctl(epollFd_, EPOLL_CTL_DEL, channel->fd(), nullptr);
            channel->setInEpoll(false);
        }
    }

    void quit() {
        quit_ = true;
    }

private:
    int epollFd_;
    bool quit_;
};

#endif