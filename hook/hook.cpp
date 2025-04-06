#include "hook.h"




#define HOOK_FUN(XX) \
    XX(sleep) \
    XX(usleep) \
    XX(nanosleep) \
    XX(socket) \
    XX(connect) \
    XX(accept) \
    XX(read) \
    XX(readv) \
    XX(recv) \
    XX(recvfrom) \
    XX(recvmsg) \
    XX(write) \
    XX(writev) \
    XX(send) \
    XX(sendto) \
    XX(sendmsg) \
    XX(close) \
    XX(fcntl) \
    XX(ioctl) \
    XX(getsockopt) \
    XX(setsockopt) 


static thread_local bool t_hook_enable = false;

bool is_hook_enable()
{
    return t_hook_enable;
}

void set_hook_enable(bool flag)
{
    t_hook_enable = flag;
}

void hook_init()
{
    //通过一个静态变量来确保 hook_init() 只初始化一次，防止重复初始化。
    //C++ 11后，静态局部变量的初始化是线程安全的，所以不需要额外的线程同步操作。（在单例模式中也有体现）
    static bool is_inited = false;
    if(is_inited)
    {
        return;
    }

    is_inited = true;

    //##表示连接两个标识符
    //如：sleep_f = (sleep_fun)dlsym(RTLD_NEXT, "sleep"); 这里name为sleep
    //RTLD_NEXT特殊句柄：这是一个特殊的句柄，指示dlsym在加载顺序中搜索"下一个"提供指定符号的库。
    //这确保我们能获取到系统的原始实现，而不是我们自己的钩子版本。
    #define XX(name) name ## _f = (name ## _fun)dlsym(RTLD_NEXT, #name);
    HOOK_FUN(XX);
    #undef XX   //取消XX宏的定义，限制作用域
}


struct HookIniter
{
    HookIniter()
    {
        hook_init();
    }
};

//定义了一个静态的 HookIniter 实例。由于静态变量的初始化发生在 main() 函数之前，所以 hook_init() 会
//在程序开始时被调用，从而初始化钩子函数。
static HookIniter s_hook_initer;    

struct timer_info
{
    int cancelled = 0;
};


template<class OriginFun, class... Args>
static ssize_t do_io(int fd, OriginFun fun, 
                        const char* hook_fun_name, 
                        uint32_t event, 
                        int timeout_so, 
                        Args&&... args)
{
    if(!t_hook_enable)  //如果全局钩子功能未启用，则直接调用原始的 I/O 函数。
    {
        return fun(fd, std::forward<Args>(args)...);
    }

    //获取与文件描述符 fd 相关联的上下文 ctx。如果上下文不存在，则直接调用原始的 I/O 函数。
    std::shared_ptr<FdCtx> ctx = fdMgr::getInstance().get(fd);
    if(!ctx)
    {
        return fun(fd, std::forward<Args>(args)...);
    }

    if(ctx->isClosed())
    {
        errno = EBADF;  //表示文件描述符无效或已经关闭
        return -1;
    }

    //如果文件描述符不是一个socket或者用户设置了非阻塞模式，则直接调用原始的I/O操作函数。
    if(!ctx->isSocket() || ctx->getUserNonblock())
    {
        return fun(fd, std::forward<Args>(args)...);
    }

    //获取超时设置并初始化timer_info结构体，用于后续的超时管理和取消操作。
    uint64_t timeout = ctx->getTimeout(timeout_so);
    std::shared_ptr<timer_info> tinfo(new timer_info);


retry:
    //调用原始的I/O函数，如果由于系统中断（EINTR）导致操作失败，函数会重试。
    ssize_t n = fun(fd, std::forward<Args>(args)...);

    while(n == -1 && errno == EINTR)
    {
        n = fun(fd, std::forward<Args>(args)...);
    }

    //如果I/O操作因为资源暂时不可用（EAGAIN）而失败，函数会添加一个事件监听器来等待资源可用。
    //同时，如果有超时设置，还会启动一个条件计时器来取消事件。
    /*
    常见的EAGAIN触发场景
        1、读操作(read/recv)场景:
        非阻塞socket尝试读取数据，但接收缓冲区为空
        例如：HTTP客户端发送了请求，但服务器响应尚未到达

        2、写操作(write/send)场景:
        非阻塞socket尝试发送数据，但发送缓冲区已满
        例如：大文件上传过程中网络带宽不足，无法立即发送更多数据

        3、接受连接(accept)场景:
        非阻塞监听socket上尝试accept，但当前无新连接到达
        例如：服务器启动后等待首个客户端连接

        4、连接操作(connect)场景:
        非阻塞socket发起connect，连接建立需要时间（TCP三次握手）
        此时返回EINPROGRESS而非EAGAIN，但处理逻辑类似
    */
    if(n == -1 && errno == EAGAIN)
    {
        IOManager* iom = IOManager::getThis();
        std::shared_ptr<Timer> timer;
        std::weak_ptr<timer_info> winfo(tinfo);

        //如果执行的read等函数在Fdmanager管理的Fdctx中fd设置了超时时间，就会走到这里。添加addconditionTimer事件
        if(timeout != (uint64_t)-1) //这行代码检查是否设置了超时时间。
        {
            timer = iom->addConditionTimer(timeout, [winfo, fd, iom, event]()
            {
                auto t = winfo.lock();
                if(!t || t->cancelled)  // 如果 timer_info 对象已被释放（!t），或者操作已被取消（t->cancelled 非 0），则直接返回。
                {
                    return;
                }
                //如果超时时间到达并且事件尚未被处理(即cancelled任然是0)；
                // cancel this event and trigger once to return to this fiber	
                //取消该文件描述符上的事件，并立即触发一次事件（即恢复被挂起的协程）
                t->cancelled = ETIMEDOUT;
                iom->cancelEvent(fd, (IOManager::Event)(event));
            }, winfo);
        }


        //比如现在是recv，由于非阻塞读，但数据还没有到达，所以此时添加一个读事件，等数据到达时，会触发这个事件，然后调度协程来处理。
        int rt = iom->addEvent(fd, (IOManager::Event)(event));
        if(rt)
        {
            std::cout << hook_fun_name << " addEvent(" << fd << ", " << event << ")" << std::endl;
            //如果 rt 为-1，说明 addEvent 失败。此时，会打印一条调试信息，并且因为添加事件失败所以要取消之前设置的定时器，避免误触发。
            if(timer)
            {
                timer->cancel();
            }
            return -1;
        }
        else
        {
            //如果 addEvent 成功（rt 为 0），当前协程会调用 yield() 函数，将自己挂起，等待事件的触发。
            Fiber::getThis()->yield();

            //当协程被恢复时（例如，事件触发后），它会继续执行 yield() 之后的代码。
            //如果之前设置了定时器（timer 不为 nullptr），则在事件处理完毕后取消该定时器。取消定时器的原因是，
            //该定时器的唯一目的是在 I/O 操作超时时取消事件。如果事件已经正常处理完毕，那么定时器就不再需要了。
            if(timer)
            {
                timer->cancel();
            }

            //接下来检查 tinfo->cancelled 是否等于 ETIMEDOUT。如果等于，说明该操作因超时而被取消，
            //因此设置 errno 为 ETIMEDOUT 并返回 -1，表示操作失败。
            if(tinfo->cancelled == ETIMEDOUT)
            {
                errno = tinfo->cancelled;
                return -1;
            }
            //如果没有超时，则跳转到 retry 标签，重新尝试这个操作。
            goto retry;
        }
    }
    return n;
}

extern "C"
{

    //sleep_fun sleep_f = nullptr;
#define XX(name) name ## _fun name ## _f = nullptr;
    HOOK_FUN(XX);
#undef XX

unsigned int sleep(unsigned int seconds)
{
    if(!t_hook_enable)
    {
        return sleep_f(seconds);
    }

    //获取当前正在执行的协程（Fiber），并将其保存到 fiber 变量中。
    std::shared_ptr<Fiber> fiber = Fiber::getThis();
    IOManager* iom = IOManager::getThis();

    iom->addTimer(seconds * 1000, [fiber, iom]()
    {
        iom->scheduleLock(fiber, -1);
    });
    fiber->yield(); //挂起当前协程的执行，将控制权交还给调度器。
    return 0;
}

int usleep(useconds_t usec)
{
    if(!t_hook_enable)
    {
        return usleep_f(usec);
    }

    //useconds_t一个无符号整数类型，通常用于表示微秒数。
    //在这个函数中，usec表示延时的微秒数，将其转换为毫秒数(usec/1000)后用于定时器。
    std::shared_ptr<Fiber> fiber = Fiber::getThis();
    IOManager* iom = IOManager::getThis();
    iom->addTimer(usec / 1000, [fiber, iom]()
    {
        iom->scheduleLock(fiber, -1);
    });
    fiber->yield();
    return 0;
}

int nanosleep(const struct timespec* req, struct timespec* rem)
{
	if(!t_hook_enable)
	{
		return nanosleep_f(req, rem);
	}	

	int timeout_ms = req->tv_sec*1000 + req->tv_nsec/1000/1000;

	std::shared_ptr<Fiber> fiber = Fiber::getThis();
	IOManager* iom = IOManager::getThis();
	// add a timer to reschedule this fiber
	iom->addTimer(timeout_ms, [fiber, iom](){iom->scheduleLock(fiber, -1);});
	// wait for the next resume
	fiber->yield();	
	return 0;
}

int socket(int domain, int type, int protocol)
{
    if(t_hook_enable)
    {
        return socket_f(domain, type, protocol);
    }

    int fd = socket_f(domain, type, protocol);
    if(fd == -1)
    {
        std::cerr << "socket() failed: " << strerror(errno) << std::endl;
        return fd;
    }
    //如果socket创建成功会利用Fdmanager的文件描述符管理类来进行管理，判断是否在其管理的文件描述符中，
    //如果不在扩展存储文件描述数组大小，并且利用FDctx进行初始化判断是是不是套接字，是不是系统非阻塞模式。
    fdMgr::getInstance().get(fd, true);
    return fd;
}

int connect_with_timeout(int fd, const struct sockaddr* addr, socklen_t addrlen, uint64_t timeout_ms)
{
    if(!t_hook_enable)
    {
        return connect_f(fd, addr, addrlen);
    }

    //获取文件描述符 fd 的上下文信息 FdCtx
    std::shared_ptr<FdCtx> ctx = fdMgr::getInstance().get(fd);
    if(!ctx || ctx->isClosed())
    {
        errno = EBADF;  //EBAD表示一个无效的文件描述符
        return -1;
    }

    //如果不是一个套接字调用原始的
    if(!ctx->isSocket())
    {
        return connect_f(fd, addr, addrlen);
    }

    //检查用户是否设置了非阻塞模式。如果是非阻塞模式，
    if(ctx->getUserNonblock())
    {
        return connect_f(fd, addr, addrlen);
    }

    int n = connect_f(fd, addr, addrlen);   //尝试进行 connect 操作，返回值存储在 n 中。
    if(n == 0)
    {
        return 0;
    }
    else if(n != -1 || errno != EINPROGRESS)    //说明连接请求未处于等待状态，直接返回结果。
    {
        return n;
    }

    IOManager* iom = IOManager::getThis();  //获取当前线程的 IOManager 实例。
    std::shared_ptr<Timer> timer;   //声明一个定时器对象。
    std::shared_ptr<timer_info> tinfo(new timer_info);  //创建追踪定时器是否取消的对象
    std::weak_ptr<timer_info> winfo(tinfo); //判断追踪定时器对象是否存在

    if(timeout_ms != (uint64_t)-1)
    {
        //添加一个定时器，当超时时间到达时，取消事件监听并设置 cancelled 状态。
        timer = iom->addConditionTimer(timeout_ms, [winfo, fd, iom]()
        {
            auto t = winfo.lock();  //判断追踪定时器对象是否存在或者追踪定时器的成员变量是否大于0.大于0就意味着取消了
            if(!t || t->cancelled)
            {
                return;
            }
            t->cancelled = ETIMEDOUT;
            iom->cancelEvent(fd, IOManager::WRITE);
        }, winfo);
    }

    int rt = iom->addEvent(fd, IOManager::WRITE);   //为文件描述符 fd 添加一个写事件监听器。这样的目的是为了上面的回调函数处理指定文件描述符
    if(rt == 0)
    {
        Fiber::getThis()->yield();
        
        if(timer)
        {
            timer->cancel();    
        }

        if(tinfo->cancelled)    //发生超时错误或者用户取消
        {
            errno = tinfo->cancelled;
            return -1;
        }
    }
    else
    {
        if(timer)
        {
            timer->cancel();
        }
        std::cerr << "connect addEvent(" << fd << ", WRITE) failed" << std::endl;
    }

    int error = 0;
    socklen_t len = sizeof(int);
    if(getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len))
    {
        return -1;
    }

    if(!error)
    {
        return 0;
    }
    else
    {
        errno = error;
        return -1;
    }

}

//s_connect_timeout 是一个 static 变量，表示默认的连接超时时间，类型为 uint64_t，可以存储 64 位无符号整数。
//-1 通常用于表示一个无效或未设置的值。由于它是无符号整数，-1 实际上会被解释为 UINT64_MAX，表示没有超时限制。
static uint64_t s_connect_timeout = -1;
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
	return connect_with_timeout(sockfd, addr, addrlen, s_connect_timeout);
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
	int fd = do_io(sockfd, accept_f, "accept", IOManager::READ, SO_RCVTIMEO, addr, addrlen);	
	if(fd>=0)
	{
		fdMgr::getInstance().get(fd, true);
	}
	return fd;
}

ssize_t read(int fd, void *buf, size_t count)
{
	return do_io(fd, read_f, "read", IOManager::READ, SO_RCVTIMEO, buf, count);	
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
{
	return do_io(fd, readv_f, "readv", IOManager::READ, SO_RCVTIMEO, iov, iovcnt);	
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags)
{
	return do_io(sockfd, recv_f, "recv", IOManager::READ, SO_RCVTIMEO, buf, len, flags);	
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen)
{
	return do_io(sockfd, recvfrom_f, "recvfrom", IOManager::READ, SO_RCVTIMEO, buf, len, flags, src_addr, addrlen);	
}

ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags)
{
	return do_io(sockfd, recvmsg_f, "recvmsg", IOManager::READ, SO_RCVTIMEO, msg, flags);	
}

ssize_t write(int fd, const void *buf, size_t count)
{
	return do_io(fd, write_f, "write", IOManager::WRITE, SO_SNDTIMEO, buf, count);	
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
	return do_io(fd, writev_f, "writev", IOManager::WRITE, SO_SNDTIMEO, iov, iovcnt);	
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags)
{
	return do_io(sockfd, send_f, "send", IOManager::WRITE, SO_SNDTIMEO, buf, len, flags);	
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen)
{
	return do_io(sockfd, sendto_f, "sendto", IOManager::WRITE, SO_SNDTIMEO, buf, len, flags, dest_addr, addrlen);	
}

ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags)
{
	return do_io(sockfd, sendmsg_f, "sendmsg", IOManager::WRITE, SO_SNDTIMEO, msg, flags);	
}

int close(int fd)
{
	if(!t_hook_enable)
	{
		return close_f(fd);
	}	

	std::shared_ptr<FdCtx> ctx = fdMgr::getInstance().get(fd);

	if(ctx)
	{
		auto iom = IOManager::getThis();
		if(iom)
		{	
			iom->cancelAll(fd);
		}
		// del fdctx
		fdMgr::getInstance().del(fd);
	}
	return close_f(fd);
}

int fcntl(int fd, int cmd, ... /* arg */ )
{
  	va_list va; // to access a list of mutable parameters

    va_start(va, cmd);
    switch(cmd) 
    {
        case F_SETFL:
            {
                int arg = va_arg(va, int); // Access the next int argument
                va_end(va);
                std::shared_ptr<FdCtx> ctx = fdMgr::getInstance().get(fd);
                if(!ctx || ctx->isClosed() || !ctx->isSocket()) 
                {
                    return fcntl_f(fd, cmd, arg);
                }
                // 用户是否设定了非阻塞
                ctx->setUserNonblock(arg & O_NONBLOCK);
                // 最后是否阻塞根据系统设置决定
                //无论用户设置如何，都会强制设置O_NONBLOCK标志
                if(ctx->getSysNonblock()) 
                {
                    arg |= O_NONBLOCK;
                } 
                else 
                {
                    arg &= ~O_NONBLOCK;
                }
                return fcntl_f(fd, cmd, arg);
            }
            break;

        case F_GETFL:
            {
                va_end(va);
                int arg = fcntl_f(fd, cmd);
                std::shared_ptr<FdCtx> ctx = fdMgr::getInstance().get(fd);
                if(!ctx || ctx->isClosed() || !ctx->isSocket()) 
                {
                    return arg;
                }
                // 这里是呈现给用户 显示的为用户设定的值
                // 但是底层还是根据系统设置决定的
                if(ctx->getUserNonblock()) 
                {
                    return arg | O_NONBLOCK;
                } else 
                {
                    return arg & ~O_NONBLOCK;
                }
            }
            break;

        case F_DUPFD:
        case F_DUPFD_CLOEXEC:
        case F_SETFD:
        case F_SETOWN:
        case F_SETSIG:
        case F_SETLEASE:
        case F_NOTIFY:
#ifdef F_SETPIPE_SZ
        case F_SETPIPE_SZ:
#endif
            {
                int arg = va_arg(va, int);
                va_end(va);
                return fcntl_f(fd, cmd, arg); 
            }
            break;


        case F_GETFD:
        case F_GETOWN:
        case F_GETSIG:
        case F_GETLEASE:
#ifdef F_GETPIPE_SZ
        case F_GETPIPE_SZ:
#endif
            {
                va_end(va);
                return fcntl_f(fd, cmd);
            }
            break;

        case F_SETLK:
        case F_SETLKW:
        case F_GETLK:
            {
                struct flock* arg = va_arg(va, struct flock*);
                va_end(va);
                return fcntl_f(fd, cmd, arg);
            }
            break;

        case F_GETOWN_EX:
        case F_SETOWN_EX:
            {
                struct f_owner_exlock* arg = va_arg(va, struct f_owner_exlock*);
                va_end(va);
                return fcntl_f(fd, cmd, arg);
            }
            break;

        default:
            va_end(va);
            return fcntl_f(fd, cmd);
    }	
}

int ioctl(int fd, unsigned long request, ...)
{
    va_list va;
    va_start(va, request);
    void* arg = va_arg(va, void*);
    va_end(va);

    if(FIONBIO == request) 
    {
        bool user_nonblock = !!*(int*)arg;
        std::shared_ptr<FdCtx> ctx = fdMgr::getInstance().get(fd);
        if(!ctx || ctx->isClosed() || !ctx->isSocket()) 
        {
            return ioctl_f(fd, request, arg);
        }
        ctx->setUserNonblock(user_nonblock);
    }
    return ioctl_f(fd, request, arg);
}

int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen)
{
	return getsockopt_f(sockfd, level, optname, optval, optlen);
}

int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen)
{
    if(!t_hook_enable) 
    {
        return setsockopt_f(sockfd, level, optname, optval, optlen);
    }

    if(level == SOL_SOCKET) 
    {
        if(optname == SO_RCVTIMEO || optname == SO_SNDTIMEO) 
        {
            std::shared_ptr<FdCtx> ctx = fdMgr::getInstance().get(sockfd);
            if(ctx) 
            {
                const timeval* v = (const timeval*)optval;
                ctx->setTimeout(optname, v->tv_sec * 1000 + v->tv_usec / 1000);
            }
        }
    }
    return setsockopt_f(sockfd, level, optname, optval, optlen);	
}


}
