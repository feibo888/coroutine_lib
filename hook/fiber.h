#pragma once

#include <iostream>
#include <mutex>
#include <memory>
#include <functional>
#include <ucontext.h>
#include <atomic>
#include <assert.h>

class Fiber : public std::enable_shared_from_this<Fiber>
{

public:
    //协程状态
    enum State
    {
        READY,
        RUNNING,
        TREM
    };

private:
    Fiber();

public:
    Fiber(std::function<void()> cb, size_t stacksize = 0, bool run_in_scheduler = true);
    ~Fiber();

    // 重用一个协程
    void reset(std::function<void()> cb);

    //任务线程恢复执行
    void resume();

    //任务线程切换到后台
    void yield();

    uint64_t getId() const { return m_id;}
    State getState() const { return m_state;}

public:
    // 设置当前运行的协程
    static void setThis(Fiber* f);

    // 得到当前运行的协程
    static std::shared_ptr<Fiber> getThis();

    //设置调度协程(默认为主协程)
    static void setSchedulerFiber(Fiber* f);

    //得到当前运行的协程id
    static uint64_t getFiberId();

    // 协程函数
    static void mainFunc();

private:
    //id
    uint64_t m_id = 0;
    //栈大小
    uint32_t m_stacksize = 0;
    //协程状态
    State m_state = READY;
    //上下文
    ucontext_t m_ctx;
    //栈指针
    void* m_stack = nullptr;
    //协程函数
    std::function<void()> m_cb;
    //是否让出执行权交给调度协程
    bool m_run_in_scheduler;

public:
    std::mutex m_mutex;

};
