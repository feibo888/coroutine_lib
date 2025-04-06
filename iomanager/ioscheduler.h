#pragma once

#include "scheduler.h"
#include "timer.h"
#include <fcntl.h>
#include <sys/epoll.h>
#include <string>
#include <cstring>
#include <unistd.h>

// 1 注册事件 -> 2 等待事件 -> 3 事件触发调度回调 -> 4 注销事件回调后从epoll注销 -> 5 执行回调进入调度器中执行调度。
class IOManager : public Scheduler, public TimerManager
{
public:
    enum Event
    {
        NONE = 0x0,
        //READ == EPOLLIN
        READ = 0x1,
        //WRITE == EPOLLOUT
        WRITE = 0x4
    };

private:
    struct FdContext    //用于描述一个文件描述的事件上下文
    {
        struct EventContext //描述一个具体事件的上下文，如读事件或写事件。
        {
            //scheduler
            Scheduler* scheduler = nullptr; //关联的调度器。
            //callback fiber
            std::shared_ptr<Fiber> fiber;   //关联的回调线程（协程）。
            //callback function
            std::function<void()> cb;   //关联的回调函数。
        };

        //read event context
        EventContext read;  //read 和write表示读和写的上下文
        //write event context
        EventContext write;
        int fd = 0;
        //events registered
        Event events = NONE;    ;//当前注册的事件目前是没有事件，但可能变成 READ、WRITE 或二者的组合。
        std::mutex mutex;

        EventContext& getEventContext(Event event); //根据事件类型获取相应的事件上下文（如读事件上下文或写事件上下文）。
        void resetEventContext(EventContext& ctx);  //重置事件上下文。
        void triggerEvent(Event event);             //触发事件。
    };

public:
    IOManager(size_t threads = 1, bool use_caller = true, const std::string& name = "IOManager");
    ~IOManager();

    //事件管理方法
    //添加一个事件到文件描述符 fd 上，并关联一个回调函数 cb。
    int addEvent(int fd, Event event, std::function<void()> cb = nullptr);
    //删除文件描述符fd上的某个事件
    bool delEvent(int fd, Event event);
    //取消文件描述符上的某个事件，并触发其回调函数
    bool cancelEvent(int fd, Event event);
    //取消文件描述符 fd 上的所有事件，并触发所有回调函数。
    bool cancelAll(int fd);

    static IOManager* getThis();

protected:
    //通知调度器有任务调度
    //写pipe让idle协程从epoll_wait退出，待idle协程yield之后Scheduler::run就可以调度其他任务.
    void tickle() override;

    //判断调度器是否可以停止
    //判断条件是Scheduler::stopping()外加IOManager的m_pendingEventCount为0，表示没有IO事件可调度
    bool stopping() override;

    //实际是idle协程只负责收集所有已触发的fd的回调函数并将其加⼊调度器
    //的任务队列，真正的执⾏时机是idle协程退出后，调度器在下⼀轮调度时执⾏
    void idle() override;

    //因为Timer类的成员函数重写当有新的定时器插入到前面时的处理逻辑
    void onTimerInsertedAtFront() override;

    //调整文件描述符上下文数组的大小。
    void contextResize(size_t size);

private:
    int m_epfd = 0; //用于epoll的文件描述符。

    int m_tickle_fds[2];    //用于线程间通信的管道文件描述符，fd[0] 是读端，fd[1] 是写端。

    std::atomic<size_t> m_pending_event_count = {0};    //记录待处理的事件数量

    std::shared_mutex m_mutex;

    std::vector<FdContext*> m_fd_contexts;  //文件描述符上下文数组，用于存储每个文件描述符的 FdContext。

};


