#pragma once

#include "fiber.h"
#include "thread.h"

#include <vector>

class Scheduler
{

public:
    Scheduler(size_t threads = 1, bool use_caller = true, const std::string& name = "Scheduler");
    virtual ~Scheduler();

    const std::string& getName() const { return m_name;}

public:
    //获取正在运行的调度器
    static Scheduler* getThis();

protected:
    //设置正在运行的调度器
    void setThis();

public:
    //添加任务到任务队列
    template <class FiberOrCb>
    void scheduleLock(FiberOrCb fc, int thread = -1)
    {
        bool need_tickle;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            need_tickle = m_tasks.empty();

            ScheduleTask task(fc, thread);
            if(task.fiber || task.cb)
            {
                m_tasks.push_back(task);
            }
        }

        if(need_tickle)
        {
            tickle();
        }
    }

    //启动线程池
    virtual void start();

    //关闭线程池
    virtual void stop();

protected:
    virtual void tickle();

    //线程函数
    virtual void run();

    //空闲协程函数
    virtual void idle();

    //是否可以关闭
    virtual bool stopping();

    bool hasIdleThreads() { return m_idle_thread_count > 0;}

private:
    //任务
    struct ScheduleTask
    {
        std::shared_ptr<Fiber> fiber;
        std::function<void()> cb;
        int thread; //指定任务需要运行的线程id

        ScheduleTask()
        {
            fiber = nullptr;
            cb = nullptr;
            thread = -1;
        }

        ScheduleTask(std::shared_ptr<Fiber> f, int thr)
        {
            fiber = f;
            thread = thr;
        }

        ScheduleTask(std::shared_ptr<Fiber>* f, int thr)
        {
            fiber.swap(*f);
            thread = thr;
        }

        ScheduleTask(std::function<void()> f, int thr)
        {
            cb = f;
            thread = thr;
        }

        ScheduleTask(std::function<void()>* f, int thr)
        {
            cb.swap(*f);
            thread = thr;
        }

        void reset()
        {
            fiber = nullptr;
            cb = nullptr;
            thread = -1;
        }



    };


private:
    std::string m_name;
    std::mutex m_mutex;

    //线程池
    std::vector<std::shared_ptr<Thread>> m_threads;

    //任务队列
    std::vector<ScheduleTask> m_tasks;

    //存储工作线程的线程id
    std::vector<int> m_thread_ids;
    
    //需要额外创建的线程数
    size_t m_thread_count = 0;

    //活跃线程数
    std::atomic<size_t> m_activate_thread_count = {0};

    //空闲线程数
    std::atomic<size_t> m_idle_thread_count = {0};

    //主线程是否用作工作线程
    bool m_use_caller;

    std::shared_ptr<Fiber> m_scheduler_fiber;

    int m_root_thread = -1;

    bool m_stopping = false;


};
