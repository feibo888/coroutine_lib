#pragma once


#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <iostream>
#include <memory>
#include "fdmanager.h"
#include "ioscheduler.h"
#include <cstdarg>

bool is_hook_enable();//用于判断钩子功能是否启用。
void set_hook_enable(bool flag);//用于设置钩子功能的启用或禁用状态。


extern "C"//确保正确调用c库中的系统调用，c++编译器不会对这些函数名进行修饰。
{
    // track the original version
 	//C函数原型声明与重定向
    typedef unsigned int (*sleep_fun) (unsigned int seconds);
    extern sleep_fun sleep_f;

    typedef int (*usleep_fun) (useconds_t usec);

    extern usleep_fun usleep_f;

    typedef int (*nanosleep_fun) (const struct timespec* req, struct timespec* rem);
    extern nanosleep_fun nanosleep_f;

    typedef int (*socket_fun) (int domain, int type, int protocol);
    extern socket_fun socket_f;

    typedef int (*connect_fun) (int sockfd, const struct sockaddr *addr, socklen_t addrlen);
    extern connect_fun connect_f;

    typedef int (*accept_fun) (int sockfd, struct sockaddr *addr, socklen_t *addrlen);
    extern accept_fun accept_f;

    typedef ssize_t (*read_fun) (int fd, void *buf, size_t count);
    extern read_fun read_f;

    typedef ssize_t (*readv_fun)(int fd, const struct iovec *iov, int iovcnt);
    extern readv_fun readv_f;

    typedef ssize_t (*recv_fun) (int sockfd, void *buf, size_t len, int flags);
    extern recv_fun recv_f;

    typedef ssize_t (*recvfrom_fun) (int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen);
    extern recvfrom_fun recvfrom_f;

    typedef ssize_t (*recvmsg_fun) (int sockfd, struct msghdr *msg, int flags);  extern recvmsg_fun recvmsg_f;

    typedef ssize_t (*write_fun) (int fd, const void *buf, size_t count);
    extern write_fun write_f;
      typedef ssize_t (*writev_fun) (int fd, const struct iovec *iov, int iovcnt);
    extern writev_fun writev_f;

    typedef ssize_t (*send_fun) (int sockfd, const void *buf, size_t len, int flags);
    extern send_fun send_f;

    typedef ssize_t (*sendto_fun) (int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen);
    extern sendto_fun sendto_f;

    typedef ssize_t (*sendmsg_fun) (int sockfd, const struct msghdr *msg, int flags);
    extern sendmsg_fun sendmsg_f;

    typedef int (*close_fun) (int fd);
    extern close_fun close_f;

    typedef int (*fcntl_fun) (int fd, int cmd, ... /* arg */ );
    extern fcntl_fun fcntl_f;

    typedef int (*ioctl_fun) (int fd, unsigned long request, ...);
    extern ioctl_fun ioctl_f;

    typedef int (*getsockopt_fun) (int sockfd, int level, int optname, void *optval, socklen_t *optlen);
    extern getsockopt_fun getsockopt_f;

    typedef int (*setsockopt_fun) (int sockfd, int level, int optname, const void *optval, socklen_t optlen);
    extern setsockopt_fun setsockopt_f;

    // function prototype -> 对应.h中已经存在 可以省略
    // sleep function
 	//函数重定义
 	//可以通过判断是否启用了钩子来决定是调用原始的系统函数，还是执行自定义的逻辑。
    unsigned int sleep(unsigned int seconds);
    int usleep(useconds_t usce);
    int nanosleep(const struct timespec* req, struct timespec* rem);

    // socket funciton
    int socket(int domain, int type, int protocol);
    int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
    int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);

    // read
    ssize_t read(int fd, void *buf, size_t count);
    ssize_t readv(int fd, const struct iovec *iov, int iovcnt);

    ssize_t recv(int sockfd, void *buf, size_t len, int flags);
    ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen);
    ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags);


    // write
    ssize_t write(int fd, const void *buf, size_t count);
    ssize_t writev(int fd, const struct iovec *iov, int iovcnt);

    ssize_t send(int sockfd, const void *buf, size_t len, int flags);
    ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen);
    ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags);

    // fd
    int close(int fd);

    // socket control
    int fcntl(int fd, int cmd, ... /* arg */ );
    int ioctl(int fd, unsigned long request, ...);

    int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen);
    int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen);
}

