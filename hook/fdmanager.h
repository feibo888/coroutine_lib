#pragma once


#include <iostream>
#include <memory>
#include <vector>
#include <sys/stat.h>
#include <shared_mutex>
#include <sys/fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include "hook.h"

class FdCtx : public std::enable_shared_from_this<FdCtx>
{
private:
    bool m_isInit = false;  //标记文件描述符是否已初始化
    bool m_isSocket = false;    //标记文件描述符是否是一个套接字。
    bool m_sysNonblock = false; //标记文件描述符是否设置为系统非阻塞模式。
    bool m_userNonblock = false;    //标记文件描述符是否设置为用户非阻塞模式。
    bool m_isClosed = false;    //标记文件描述符是否已关闭。
    int m_fd;   //文件描述符的整数值

    uint64_t m_recvTimeout = (uint64_t)-1;  //读事件的超时时间，默认为 -1 表示没有超时限制。
    uint64_t m_sendTimeout = (uint64_t)-1;  //写事件的超时时间，默认为 -1 表示没有超时限制。

public:
    FdCtx(int fd);
    ~FdCtx();

    bool init();
    bool isInit() const { return m_isInit; }
    bool isSocket() const { return m_isSocket; }
    bool isClosed() const { return m_isClosed; }

    void setUserNonblock(bool v) { m_userNonblock = v;} //设置和获取用户层面的非阻塞状态。
    bool getUserNonblock() const { return m_userNonblock; }

    void setSysNonblock(bool v) { m_sysNonblock = v;}   //设置和获取系统层面的非阻塞状态。
    bool getSysNonblock() const { return m_sysNonblock; }

    //设置和获取超时时间，type 用于区分读事件和写事件的超时设置，v表示时间毫秒。
    void setTimeout(int type, uint64_t v);
    uint64_t getTimeout(int type) const;

};


class FdManager
{
public:
    FdManager();

    //获取指定文件描述符的 FdCtx 对象。如果 auto_create 为 true，在不存在时自动创建新的 FdCtx 对象。
    std::shared_ptr<FdCtx> get(int fd, bool auto_create = false);
    void del(int fd);

private:
    std::shared_mutex m_mutex;  //用于保护对 m_datas 的访问，支持共享读锁和独占写锁。
    std::vector<std::shared_ptr<FdCtx>> m_datas;    //存储所有 FdCtx 对象的共享指针。

};

template <class T>
class Singleton
{
private:
    Singleton();
    ~Singleton();
    Singleton(const Singleton&) = delete;
    Singleton& operator=(const Singleton&) = delete;

public:
    static T& getInstance()
    {
        /**
         * 局部静态特性的方式实现单实例。
         * 静态局部变量只在当前函数内有效，其他函数无法访问。
         * 静态局部变量只在第一次被调用的时候初始化，也存储在静态存储区，生命周期从第一次被初始化起至程序结束止。
         */
        static T instance;
        return instance;
    }
};

typedef Singleton<FdManager> fdMgr;
