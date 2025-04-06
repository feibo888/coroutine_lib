#include "scheduler.h"


static bool debug = false;

static thread_local Scheduler* t_scheduler = nullptr;



/*
    首先初始化了线程池的数量threads，调度器的线程或主线程是否参与调度use_caller，设置了调度器的名字等。 
然后判断use_caller是否需要主线程或调度线程作为工作线程，如果需要则线程池的数量-1，并且在前面提到使
用协程之前需要调用一次GetThis初始化线程局部变量的主协程和调度协程，通过reset创建新的调度协程，覆盖
Getthis初始化的调度协程，将主线程的id存储到工作线程的线程id中，最后将剩余的线程数threads的总和放入
到m_threadCount中。
*/
Scheduler::Scheduler(size_t threads, bool use_caller, const std::string &name) :
                    m_use_caller(use_caller), m_name(name)
{
    assert(threads > 0 && Scheduler::getThis() == nullptr);

    setThis();

    Thread::setCurrentThreadName(m_name);

    //使用主线程当作工作线程
    if(use_caller)
    {
        threads--;

        //创建主协程
        Fiber::getThis();

        //创建调度协程
        m_scheduler_fiber.reset(new Fiber(std::bind(&Scheduler::run, this), 0, false)); // false -> 该调度协程退出后将返回主协程
        Fiber::setSchedulerFiber(m_scheduler_fiber.get());

        m_root_thread = Thread::getThreadId();
        m_thread_ids.push_back(m_root_thread);
    }

    m_thread_count = threads;
    if(debug) std::cout << "Scheduler::Scheduler() success" << std::endl;

}

void Scheduler::start()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if(m_stopping)
    {
        std::cerr << "Scheduler::start() scheduler is stopping" << std::endl;
        return;
    }

    assert(m_threads.empty());
    m_threads.resize(m_thread_count);
    for(size_t i = 0; i < m_thread_count; ++i)
    {
        m_threads[i].reset(new Thread(std::bind(&Scheduler::run, this), m_name + "_" + std::to_string(i)));
        m_thread_ids.push_back(m_threads[i]->getId());
    }
    if(debug) std::cout << "Scheduler::start() success" << std::endl;
}

bool Scheduler::stopping()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stopping && m_tasks.empty() && m_activate_thread_count == 0;
}

//作用：调度器的核心，负责从任务队列中取出任务并通过协程执行
void Scheduler::run()
{
    int thread_id = Thread::getThreadId();
    if(debug) std::cout << "Scheduler::run() thread_id = " << thread_id << std::endl;

    setThis();

    //不是主线程的话，需要创建主协程和调度协程，主线程的主协程在Scheduler构造函数中创建
    if(thread_id != m_root_thread)
    {
        Fiber::getThis();
    }

    //空闲协程
    std::shared_ptr<Fiber> idle_fiber = std::make_shared<Fiber>(std::bind(&Scheduler::idle, this));
    ScheduleTask task;

    while(true)
    {
        task.reset();
        bool tickle_me = false; //是否唤醒了其他线程进行任务调度

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_tasks.begin();

            // 1、遍历任务队列
            while(it != m_tasks.end())
            {
                // 检查任务是否指定了特定线程执行
                if(it->thread != -1 && it->thread != thread_id) 
                {
                    it++;
                    tickle_me = true;
                    continue;
                }

                // 2、取出任务
                /*
                    完整引用计数变化流程
                        1.取出任务前：
                        引用计数=1（只由队列持有）

                        2.取出任务后：
                        引用计数=1（由局部变量 task 持有）

                        3.执行 resume() 进入协程：
                        t_fiber 被设置为当前协程的原始指针
                        尚未创建新的 shared_ptr，引用计数=1
                        
                        4.在 mainFunc() 中调用 getThis()：
                        通过 shared_from_this() 创建新引用，引用计数=2
                        局部变量 task 和 curr 各持有一份
                        
                        5.执行完毕：
                        curr.reset() 释放一份引用，引用计数=1
                        执行 yield() 返回调度器
                        
                        6.调度器继续执行：
                        task.reset() 释放最后一份引用，引用计数=0
                        协程对象被销毁
                */
                assert(it->fiber || it->cb);
                task = *it;
                m_tasks.erase(it);
                m_activate_thread_count++;
                break;
            }
            tickle_me = tickle_me || (it != m_tasks.end()); //确保仍然存在未处理的任务
        }

        if(tickle_me)
        {
            tickle();
        }

        // 3、执行任务
        if(task.fiber)
        {
            //resume协程，resume返回时此时任务要么执行完了，要么半路yield了，总之任务完成了，活跃线程-1；
            {
                std::lock_guard<std::mutex> lock(task.fiber->m_mutex);
                if(task.fiber->getState() != Fiber::TREM)
                {
                    task.fiber->resume();
                }
            }
            m_activate_thread_count--;
            task.reset();
        }
        else if(task.cb)
        {
            //对于函数也应该被调度，具体做法就封装成协程加入调度。
            std::shared_ptr<Fiber> cb_fiber = std::make_shared<Fiber>(task.cb);
            {
                std::lock_guard<std::mutex> lock(cb_fiber->m_mutex);
                cb_fiber->resume();
            }
            m_activate_thread_count--;
            task.reset();
        }
        // 4、没有任务则执行空闲函数
        else
        {
            // 系统关闭 -> idle协程将从死循环跳出并结束 -> 此时的idle协程状态为TERM -> 再次进入将跳出循环并退出run()
            if(idle_fiber->getState() == Fiber::TREM)
            {
                //如果调度器没有调度任务，那么idle协程会不断的resume/yield,不会结束进入一个忙等待，如果idele协程结束了
                //一定是调度器停止了，直到有任务才执行上面的if/else，在这里idle_fiber就是不断的和主协程进行交互的子协程
                if(debug) std::cout << "Scheduler::run() ends in thread: " << thread_id << std::endl;
                break;
            }
            m_idle_thread_count++;
            idle_fiber->resume();
            m_idle_thread_count--;
        }
    }
}

void Scheduler::idle()
{
    while(!stopping())
    {
        if(debug) std::cout << "Scheduler::idle() thread_id = " << Thread::getThreadId() << std::endl;
        sleep(1);   //降低空闲协程在无任务时对cpu占用率，避免空转浪费资源
        Fiber::getThis()->yield();
    }
}

void Scheduler::stop()
{
    if(debug) std::cout << "Scheduler::stop() starts in thread: " << Thread::getThreadId() << std::endl;
    
    if(stopping())
    {
        return;
    }

    m_stopping = true;

    if(m_use_caller)
    {
        assert(getThis() == this);
    }
    else
    {
        assert(getThis() != this);
    }

    for(size_t i = 0; i < m_thread_count; ++i)
    {
        tickle();
    }

    if(m_scheduler_fiber)
    {
        tickle();
    }

    if(m_scheduler_fiber)
    {
        m_scheduler_fiber->resume();
        if(debug) std::cout << "m_scheduler_fiber ends in thread: " << Thread::getThreadId() << std::endl;
    }

    std::vector<std::shared_ptr<Thread>> thrs;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        thrs.swap(m_threads);
    }

    for(auto& i : thrs)
    {
        i->join();
    }

    if(debug) std::cout << "Scheduler::stop() ends in thread: " << Thread::getThreadId() << std::endl; 

}

Scheduler::~Scheduler()
{
    assert(stopping() == true);
    if(getThis() == this)
    {
        t_scheduler = nullptr;
    }
    if(debug) std::cout << "Scheduler::~Scheduler() success" << std::endl;
}

Scheduler *Scheduler::getThis()
{
    return t_scheduler;
}

void Scheduler::setThis()
{
    t_scheduler = this;
}

void Scheduler::tickle()
{

}
