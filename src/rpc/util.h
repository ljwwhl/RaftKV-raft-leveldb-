#ifndef RPC_UTIL_H
#define RPC_UTIL_H

#include <fcntl.h>
#include <unistd.h>


//设置 fd 非阻塞
inline void setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        flags = 0;
    }

    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

#endif