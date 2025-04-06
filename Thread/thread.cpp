#include "thread.h"


static thread_local Thread* t_thread = nullptr;
static thread_local std::string t_thread_name = "UNKNOW";


Thread::Thread(std::function<void()> cb, const std::string &name) : m_cb(cb), m_name(name)
{
    /*
        由于静态成员函数无法直接访问对象的成员变量和非静态成员方法，所以常用的做法是：

        创建线程时，将当前对象的 this 指针作为参数传递：
        pthread_create(&m_thread, nullptr, &Thread::run, this);

        在静态 run 方法中，将参数转换回原对象指针，然后调用对象的成员方法：
        void* Thread::run(void* arg) 
        {
            Thread* thread = (Thread*)arg;  // 恢复 this 指针
            // 现在可以通过 thread 指针访问对象成员
        }
    */
    int rt = pthread_create(&m_thread, nullptr, &Thread::run, this);
    if(rt)
    {
        std::cerr << "pthread_create thread fail, rt=" << rt << " name=" << name << std::endl;
        throw std::logic_error("pthread_create error");
    }
    //等待线程函数完成初始化
    m_semaphore.wait();
}

Thread::~Thread()
{
    if(m_thread)
    {
        pthread_detach(m_thread);
        m_thread = 0;
    }
}

void Thread::join()
{
    if(m_thread)
    {
        int rt = pthread_join(m_thread, nullptr);
        if(rt)
        {
            std::cerr << "pthread_join thread fail, rt=" << rt << " name=" << m_name << std::endl;
            throw std::logic_error("pthread_join error");
        }
        m_thread = 0;
    }
}

pid_t Thread::getThreadId()
{
    return syscall(SYS_gettid);
}

Thread *Thread::getThis()
{
    return t_thread;
}

const std::string &Thread::getCurrentThreadName()
{
    return t_thread_name;
}

void Thread::setCurrentThreadName(const std::string &name)
{
    if(t_thread)
    {
        t_thread->m_name = name;
    }
    t_thread_name = name;
}

void *Thread::run(void *arg)
{
    Thread* thread = (Thread*)arg;

    t_thread = thread;
    t_thread_name = thread->m_name;
    thread->m_id = getThreadId();
    pthread_setname_np(pthread_self(), thread->m_name.substr(0, 15).c_str());

    std::function<void()> cb;
    cb.swap(thread->m_cb);  // swap -> 可以减少m_cb中智能指针的引用计数

    //初始化完成
    thread->m_semaphore.signal();

    cb();
    return 0;
}
