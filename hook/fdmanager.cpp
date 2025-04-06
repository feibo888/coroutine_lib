#include "fdmanager.h"

FdCtx::FdCtx(int fd) : m_fd(fd)
{
    init();
}

FdCtx::~FdCtx()
{
}

bool FdCtx::init()
{
    if(m_isInit)
    {
        return true;
    }

    struct stat statbuf;

    // fstat 函数用于获取与文件描述符 m_fd 关联的文件状态信息存放到 statbuf 中。
    // 如果 fstat() 返回 -1，表示文件描述符无效或出现错误。
    if(fstat(m_fd, &statbuf) == -1)
    {
        m_isInit = false;
        m_isSocket = false;
    }
    else
    {
        m_isInit = true;
        m_isSocket = S_ISSOCK(statbuf.st_mode); // S_ISSOCK(statbuf.st_mode) 用于判断文件类型是否为套接字
    }


    if(m_isSocket)
    {
        int flags = fcntl_f(m_fd, F_GETFL, 0);  // 获取文件描述符的状态
        if(!(flags & O_NONBLOCK))
        {
            fcntl_f(m_fd, F_SETFL, flags | O_NONBLOCK); // 检查当前标志中是否已经设置了非阻塞标志。如果没有设置
        }
        m_sysNonblock = true;
    }
    else
    {
        m_sysNonblock = false;  // 如果不是一个 socket 那就没必要设置非阻塞了。
    }

    return m_isInit;

}

void FdCtx::setTimeout(int type, uint64_t v)
{
    if(type == SO_RCVTIMEO)
    {
        m_recvTimeout = v;
    }
    else
    {
        m_sendTimeout = v;
    }
}

uint64_t FdCtx::getTimeout(int type) const
{
    if(type == SO_RCVTIMEO)
    {
        return m_recvTimeout;
    }
    else
    {
        return m_sendTimeout;
    }
}

FdManager::FdManager()
{
    m_datas.resize(64);
}

std::shared_ptr<FdCtx> FdManager::get(int fd, bool auto_create)
{
    if(fd == -1)
    {
        return nullptr;
    }

    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    if(m_datas.size() <= fd)    // 描述符超出当前数组范围
    {
        if(auto_create == false)    // 不需要创建，直接返回空
        {
            return nullptr;
        }
    }
    else    // 描述符在数组范围内
    {
        if(m_datas[fd] || !auto_create)     // 返回现有对象(可能为nullptr) 或者不需要创建也返回nullptr
        {
            return m_datas[fd]; 
        }
    }
    read_lock.unlock();
    std::unique_lock<std::shared_mutex> write_lock(m_mutex);

    if(m_datas.size() <= fd)
    {
        m_datas.resize(fd * 1.5);
    }

    m_datas[fd] = std::make_shared<FdCtx>(fd);  // 创建新对象
    return m_datas[fd];

}

void FdManager::del(int fd)
{
    std::unique_lock<std::shared_mutex> write_lock(m_mutex);
    if(m_datas.size() <= fd)
    {
        return;
    }
    m_datas[fd].reset();
}
