#include "ioscheduler.h"

static bool debug = false;

IOManager::IOManager(size_t threads, bool use_caller, const std::string &name) :
                    Scheduler(threads, use_caller, name), TimerManager()
{
    m_epfd = epoll_create(5000);
    assert(m_epfd > 0);

    int rt = pipe(m_tickle_fds);    //创建管道的函数规定了m_tickleFds[0]是读端，1是写端
    assert(!rt);

    //将管道的监听注册到epoll上
    epoll_event event;
    event.events = EPOLLIN | EPOLLET;   // Edge Triggered，设置标志位，并且采用边缘触发和读事件。
    event.data.fd = m_tickle_fds[0];

    //修改管道文件描述符以非阻塞的方式，配合边缘触发。
    rt = fcntl(m_tickle_fds[0], F_SETFL, O_NONBLOCK);
    assert(!rt);

    //将 m_tickleFds[0];作为读事件放入到event监听集合中
    rt = epoll_ctl(m_epfd, EPOLL_CTL_ADD, m_tickle_fds[0], &event);
    assert(!rt);

    //初始化了一个包含 32 个文件描述符上下文的数组
    contextResize(32);

    //启动 Scheduler，开启线程池，准备处理任务。
    start();
}

IOManager::~IOManager()
{
    stop(); //关闭scheduler类中的线程池，让任务全部执行完后线程安全退出
    close(m_epfd);
    close(m_tickle_fds[0]);
    close(m_tickle_fds[1]);

    //将fdcontext文件描述符一个个关闭
    for(size_t i = 0; i < m_fd_contexts.size(); ++i)
    {
        if(m_fd_contexts[i])
        {
            delete m_fd_contexts[i];
        }
    }
}

int IOManager::addEvent(int fd, Event event, std::function<void()> cb)
{
    //查找FdContext对象
    FdContext* fd_ctx = nullptr;

    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    if((int)m_fd_contexts.size() > fd)  //如果说传入的fd在数组里面则查找然后初始化FdContext的对象
    {
        fd_ctx = m_fd_contexts[fd];
        read_lock.unlock();
    }
    else    //不存在则重新分配数组的size来初始化FdContext的对象
    {
        read_lock.unlock();
        std::unique_lock<std::shared_mutex> write_lock(m_mutex);
        contextResize(fd * 1.5);
        fd_ctx = m_fd_contexts[fd];
    }

    //一旦找到或者创建Fdcontextt的对象后，加上互斥锁，确保Fdcontext的状态不会被其他线程修改
    std::lock_guard<std::mutex> lock(fd_ctx->mutex);

    if(fd_ctx->events & event)  //判断事件是否存在存在？是就返回-1，因为相同的事件不能重复添加
    {
        return -1;
    }

    //如果已经存在就fd_ctx->events本身已经有读或写，就是修改已经有事件，如果不存在就是none事件的情况，就添加事件。
    int op = fd_ctx->events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    epoll_event epevent;
    epevent.events = EPOLLET | fd_ctx->events | event;
    epevent.data.ptr = fd_ctx;

    //将事件添加到 epoll 中
    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if(rt)
    {
        std::cerr << "addEvent::epoll_ctl failed: " << strerror(errno) << std::endl;
    }

    ++m_pending_event_count;    //原子计数器，待处理的事件++；

    fd_ctx->events = (Event)(fd_ctx->events | event);   //更新 FdContext 的 events 成员，记录当前的所有事件

    FdContext::EventContext& event_ctx = fd_ctx->getEventContext(event);
    assert(!event_ctx.scheduler && !event_ctx.fiber && !event_ctx.cb);//确保 EventContext 中没有其他正在执行的调度器、协程或回调函数。
    event_ctx.scheduler = Scheduler::getThis();//设置调度器为当前的调度器实例（Scheduler::GetThis()）。
    //如果提供了回调函数 cb，则将其保存到 EventContext 中；否则，将当前正在运行的协程保存到 EventContext 中，
    //并确保协程的状态是正在运行。
    if(cb)
    {
        event_ctx.cb.swap(cb);
    
    }
    else
    {
        event_ctx.fiber = Fiber::getThis(); //需要确保存在主协程
        assert(event_ctx.fiber->getState() == Fiber::RUNNING);
    }

    return 0;
}

bool IOManager::delEvent(int fd, Event event)
{
    //和添加事件类似
    FdContext* fd_ctx = nullptr;

    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    if((int)m_fd_contexts.size() > fd)
    {
        fd_ctx = m_fd_contexts[fd];
        read_lock.unlock();
    }
    else
    {
        read_lock.unlock();
        return false;
    }

    std::lock_guard<std::mutex> lock(fd_ctx->mutex);
    
    if(!(fd_ctx->events & event))
    {
        return false;
    }

    Event new_events = (Event)(fd_ctx->events & ~event);
    int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events = EPOLLET | new_events;
    epevent.data.ptr = fd_ctx;  //这一步是为了在 epoll 事件触发时能够快速找到与该事件相关联的 FdContext 对象

    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if(rt)
    {
        std::cerr << "delEvent::epoll_ctl failed: " << strerror(errno) << std::endl;
        return -1;
    }

    --m_pending_event_count;

    fd_ctx->events = new_events;

    FdContext::EventContext& event_ctx = fd_ctx->getEventContext(event);
    fd_ctx->resetEventContext(event_ctx);
    return true;

}

bool IOManager::cancelEvent(int fd, Event event)
{
    //先检查文件描述符是否存在
    FdContext* fd_ctx = nullptr;

    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    if((int)m_fd_contexts.size() > fd)
    {
        fd_ctx = m_fd_contexts[fd];
        read_lock.unlock();
    }
    else
    {
        read_lock.unlock();
        return false;
    }

    std::lock_guard<std::mutex> lock(fd_ctx->mutex);

    //再检查要取消的事件是否存在
    if(!(fd_ctx->events & event))
    {
        return false;
    }

    Event new_events = (Event)(fd_ctx->events & ~event);
    int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events = EPOLLET | new_events;
    epevent.data.ptr = fd_ctx;

    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if(rt)
    {
        std::cerr << "cancelEvent::epoll_ctl failed: " << strerror(errno) << std::endl;
        return -1;
    }

    --m_pending_event_count;

    fd_ctx->triggerEvent(event);        //和delEvent不同的是，这里触发了事件
    return true;

}

bool IOManager::cancelAll(int fd)
{
    //先检查文件描述符是否存在
    FdContext* fd_ctx = nullptr;

    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    if((int)m_fd_contexts.size() > fd)
    {
        fd_ctx = m_fd_contexts[fd];
        read_lock.unlock();
    }
    else
    {
        read_lock.unlock();
        return false;
    }

    std::lock_guard<std::mutex> lock(fd_ctx->mutex);

    //再检查是否有事件，因为这是取消所有事件
    if(!fd_ctx->events)
    {
        return false;
    }

    int op = EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events = 0;
    epevent.data.ptr = fd_ctx;

    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if(rt)
    {
        std::cerr << "cancelAll::epoll_ctl failed: " << strerror(errno) << std::endl;
        return -1;
    }

    if(fd_ctx->events & READ)
    {
        fd_ctx->triggerEvent(READ);
        --m_pending_event_count;
    }

    if(fd_ctx->events & WRITE)
    {
        fd_ctx->triggerEvent(WRITE);
        --m_pending_event_count;
    }

    assert(fd_ctx->events == 0);
    return true;
}

IOManager *IOManager::getThis()
{
    return dynamic_cast<IOManager*>(Scheduler::getThis());
}

void IOManager::tickle()
{
    if(!hasIdleThreads())
    {
        //这个函数在scheduler检查当前是否有线程处于空闲状态。如果没有空闲线程，函数直接返回，不执行后续操作。
        return;
    }
    //如果有空闲线程，函数会向管道 m_tickleFds[1] 写入一个字符 "T"。这个写操作的目的是向等待
    //在 m_tickleFds[0]（管道的另一端）的线程发送一个信号，通知它有新任务可以处理了。
    int rt = write(m_tickle_fds[1], "T", 1);
    assert(rt == 1);
}

bool IOManager::stopping()
{
    uint64_t timeout = getNextTimer();
    //检查定时器、挂起事件以及调度器状态，以决定是否可以安全地停止运行。
    return timeout == ~0ull && m_pending_event_count == 0 && Scheduler::stopping();
}

void IOManager::idle()
{
    //定义了 epoll_wait 能同时处理的最大事件数。
    static const uint64_t MAX_EVENTS = 256;
    //用于存储从 epoll_wait 获取的事件
    std::unique_ptr<epoll_event[]> events(new epoll_event[MAX_EVENTS]);

    while(true)
    {
        if(debug) std::cout << "IOManger::idle(), run in thread: " << Thread::getThreadId() << std::endl;
        if(stopping())
        {
            if(debug) std::cout << "name = " << getName() << " idle exists in thread: " << Thread::getThreadId() << std::endl;  
            break;
        }

        int rt = 0;
        while(true)
        {
            static const uint64_t MAX_TIMEOUT = 5000;
            uint64_t next_timeout = getNextTimer();
            //获取下一个定时器的超时时间，并将其与 MAX_TIMEOUT 取较小值，避免等待时间过长。
            next_timeout = std::min(next_timeout, MAX_TIMEOUT);

            //epoll_wait陷入阻塞，等待tickle信号的唤醒，
            //并且使用了定时器堆中最早超时的定时器作为epoll_wait超时时间。
            rt = epoll_wait(m_epfd, events.get(), MAX_EVENTS, (int)next_timeout);
            if(rt < 0 && errno == EINTR)    //rt小于0代表无限阻塞，errno是EINTR(表示信号中断)则继续等待
            {
                continue;
            }
            else
            {
                break;
            }
        }

        std::vector<std::function<void()>> cbs; //用于存储超时的回调函数。
        listExpiredCb(cbs); //用来获取所有超时的定时器回调，并将它们添加到 cbs中。
        if(!cbs.empty())
        {
            for(const auto& cb : cbs)
            {
                scheduleLock(cb);
            }
            cbs.clear();
        }

        //遍历所有的rt，代表有多少个事件准备了
        for(int i = 0; i < rt; ++i)
        {
            epoll_event& event = events[i]; //获取第 i 个 epoll_event，用于处理该事件。

            // tickle event
			//检查当前事件是否是 tickle 事件（即用于唤醒空闲线程的事件）。
            if(event.data.fd == m_tickle_fds[0])
            {
                uint8_t dummy[256];
                while(read(m_tickle_fds[0], dummy, sizeof(dummy)) > 0);
                continue;
            }

            //通过 event.data.ptr 获取与当前事件关联的 FdContext 指针 fd_ctx，该指针包含了与文件描述符相关的上下文信息。
            FdContext* fd_ctx = (FdContext*)event.data.ptr; 
            std::lock_guard<std::mutex> lock(fd_ctx->mutex);

            //如果当前事件是错误或挂起（EPOLLERR 或 EPOLLHUP），则将其转换为可读或可写事件（EPOLLIN 或 EPOLLOUT），以便后续处理。
            if(event.events & (EPOLLERR | EPOLLHUP))
            {
                event.events |= (EPOLLIN | EPOLLOUT) & fd_ctx->events;
            }

            //确定实际发生的事件类型（读取、写入或两者）。
            int real_events = NONE;
            if(event.events & EPOLLIN)
            {
                real_events |= READ;
            }
            if(event.events & EPOLLOUT)
            {
                real_events |= WRITE;
            }

            if((fd_ctx->events & real_events) == NONE)
            {
                continue;
            }

            //这里进行取反就是计算剩余未发送的的事件
            int left_events = (fd_ctx->events & ~real_events);
            int op = left_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
            //如果left_event没有事件了那么就只剩下边缘触发了
            event.events = EPOLLET | left_events;

            //根据之前计算的操作（op），调用 epoll_ctl 更新或删除 epoll 监听，如果失败，打印错误并继续处理下一个事件。
            int rt2 = epoll_ctl(m_epfd, op, fd_ctx->fd, &event);
            if(rt2)
            {
                std::cerr << "epoll_ctl failed: " << strerror(errno) << std::endl;
                continue;
            }

            //触发事件，事件的执行
            if(real_events & READ)
            {
                fd_ctx->triggerEvent(READ);
                --m_pending_event_count;
            }
            if(real_events & WRITE)
            {
                fd_ctx->triggerEvent(WRITE);
                --m_pending_event_count;
            }
        }
        //当前线程的协程主动让出控制权，调度器可以选择执行其他任务或再次进入 idle 状态。
        Fiber::getThis()->yield();
    }
}

void IOManager::onTimerInsertedAtFront()
{
    tickle();
}

void IOManager::contextResize(size_t size)
{
    m_fd_contexts.resize(size);

    for(size_t i = 0; i < m_fd_contexts.size(); ++i)
    {
        if(m_fd_contexts[i] == nullptr)
        {
            m_fd_contexts[i] = new FdContext;
            m_fd_contexts[i]->fd = i;
        }
    }
}

IOManager::FdContext::EventContext &IOManager::FdContext::getEventContext(Event event)
{
    assert(event == READ || event == WRITE);
    switch (event)
    {
    case READ:
        return read;
    case WRITE:
        return write;  
    default:
        break;
    }
    throw std::invalid_argument("Unsupported event type");
}

void IOManager::FdContext::resetEventContext(EventContext &ctx)
{
    ctx.scheduler = nullptr;
    ctx.fiber.reset();
    ctx.cb = nullptr;
}

void IOManager::FdContext::triggerEvent(Event event)
{
    assert(events && event);

    // 清理该事件，表示不再关注，也就是说，注册IO事件是一次性的，
    //如果想持续关注某个Socket fd的读写事件，那么每次触发事件后都要重新添加
    events = (Event)(events & ~event);  //因为使用了十六进制位，所以对标志位取反就是相当于将event从events中删除

    EventContext& ctx = getEventContext(event);
    //把真正要执行的函数放入到任务队列中等线程取出后任务后，协程执行，执行完成后返回主协程继续，执行run方法取任务执行任务
    if(ctx.cb)
    {
        ctx.scheduler->scheduleLock(ctx.cb);
    }
    else
    {
        ctx.scheduler->scheduleLock(ctx.fiber);
    }

    resetEventContext(ctx);
    return;
}
