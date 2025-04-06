#include "fiber.h"


static bool debug = false;


//正在运行的协程
static thread_local Fiber* t_fiber = nullptr;

//主协程
static thread_local std::shared_ptr<Fiber> t_thread_fiber = nullptr;

//调度协程
static thread_local Fiber* t_scheduler_fiber = nullptr;



//协程id
static std::atomic<uint64_t> s_fiber_id{0};

//协程计数器
static std::atomic<uint64_t> s_fiber_count{0};

/*
主协程构造函数 (Fiber())

直接获取当前上下文作为初始状态
不分配额外栈空间，直接使用线程栈
*/
Fiber::Fiber()
{
    setThis(this);
    m_state = RUNNING;

    if(getcontext(&m_ctx))
    {
        std::cerr << "Fiber() failed" << std::endl;
        pthread_exit(nullptr);
    }

    m_id = s_fiber_id++;
    s_fiber_count++;
    if(debug)
    {
        std::cout << "Fiber(): main id = " << m_id << std::endl;
    }
}


/*
普通协程构造函数

分配独立栈空间
初始化上下文并设置入口函数为 mainFunc
初始状态为 READY
*/
Fiber::Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler) :
                m_cb(cb), m_stacksize(stacksize), m_run_in_scheduler(run_in_scheduler)
{
    m_state = READY;

    //分配协程栈空间
    m_stacksize = stacksize ? stacksize : 128000;
    m_stack = malloc(m_stacksize);

    if(getcontext(&m_ctx))
    {
        std::cerr << "Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler) failed" << std::endl;
        pthread_exit(nullptr);
    }

    m_ctx.uc_link = nullptr;
    m_ctx.uc_stack.ss_sp = m_stack;
    m_ctx.uc_stack.ss_size = m_stacksize;
    makecontext(&m_ctx, &Fiber::mainFunc, 0);

    m_id = s_fiber_id++;
    s_fiber_count++;
    if(debug)
    {
        std::cout << "Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler): id = " << m_id << std::endl;
    }

}

Fiber::~Fiber()
{
    s_fiber_count--;
    if(m_stack)
    {
        free(m_stack);
    }
    if(debug)
    {
        std::cout << "~Fiber(): id = " << m_id << std::endl;
    }
}

void Fiber::reset(std::function<void()> cb)
{
    assert(m_stack != nullptr && m_state == TREM);

    m_state = READY;
    m_cb = cb;

    if(getcontext(&m_ctx))
    {
        std::cerr << "reset() failed" << std::endl;
        pthread_exit(nullptr);
    }

    m_ctx.uc_link = nullptr;
    m_ctx.uc_stack.ss_sp = m_stack;
    m_ctx.uc_stack.ss_size = m_stacksize;
    makecontext(&m_ctx, &Fiber::mainFunc, 0);
}

void Fiber::resume()
{
    assert(m_state == READY);

    m_state = RUNNING;

    if(m_run_in_scheduler)
    {
        setThis(this);
        if(swapcontext(&(t_scheduler_fiber->m_ctx), &m_ctx))
        {
            std::cerr << "resume() to t_scheduler_fiber failed" << std::endl;
            pthread_exit(nullptr);
        }
    }
    else
    {
        setThis(this);
        if(swapcontext(&(t_thread_fiber->m_ctx), &m_ctx))
        {
            std::cerr << "resume() to t_thread_fiber failed" << std::endl;
            pthread_exit(nullptr);
        }
    }
}

void Fiber::yield()
{
    assert(m_state == RUNNING || m_state == TREM);

    if(m_state != TREM)
    {
        m_state = READY;
    }

    if(m_run_in_scheduler)
    {
        setThis(t_scheduler_fiber);
        if(swapcontext(&m_ctx, &(t_scheduler_fiber->m_ctx)))
        {
            std::cerr << "yield() to t_scheduler_fiber failed" << std::endl;
            pthread_exit(nullptr);
        }
    }
    else
    {
        setThis(t_thread_fiber.get());
        if(swapcontext(&m_ctx, &(t_thread_fiber->m_ctx)))
        {
            std::cerr << "yield() to t_thread_fiber failed" << std::endl;
            pthread_exit(nullptr);
        }
    }
}

void Fiber::setThis(Fiber *f)
{
    t_fiber = f;
}


// 首先运行该函数创建主协程
std::shared_ptr<Fiber> Fiber::getThis()
{
    if(t_fiber)
    {
        return t_fiber->shared_from_this();
    }

    std::shared_ptr<Fiber> main_fiber(new Fiber());
    t_thread_fiber = main_fiber;
    t_scheduler_fiber = main_fiber.get();   //除非主动设置，主协程默认为调度协程

    assert(t_fiber == main_fiber.get());
    return t_fiber->shared_from_this();
}

void Fiber::setSchedulerFiber(Fiber *f)
{
    t_scheduler_fiber = f;
}

uint64_t Fiber::getFiberId()
{
    if(t_fiber)
    {
        return t_fiber->getId();
    }
    return (uint64_t)-1;
}

void Fiber::mainFunc()
{
    std::shared_ptr<Fiber> curr = getThis();
    assert(curr != nullptr);

    curr->m_cb();
    curr->m_cb = nullptr;
    curr->m_state = TREM;

    //运行完毕，让出执行权
    Fiber* raw_ptr = curr.get();  // 获取原始指针（不影响引用计数）
    curr.reset();               // 释放共享指针（减少引用计数）
    raw_ptr->yield();           // 使用原始指针完成上下文切换
}
