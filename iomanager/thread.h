#pragma once

#include <mutex>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <sys/syscall.h>
#include <unistd.h>


class Semaphore
{
public:
    explicit Semaphore(int count = 0) : m_count(count){}

    //P操作
    void wait()
    {
        std::unique_lock<std::mutex> lock(mtx);
        while(m_count == 0)
        {
            cv.wait(lock);
        }
        m_count--;
    }

    //V操作
    void signal()
    {
        std::unique_lock<std::mutex> lock(mtx);
        m_count++;
        cv.notify_one();
    }


private:
    std::mutex mtx;
    std::condition_variable cv;
    int m_count;

};

// 一共两种线程: 1 由系统自动创建的主线程 2 由Thread类创建的线程 
class Thread
{
public:
    Thread(std::function<void()> cb, const std::string& name);
    ~Thread();

    pid_t getId() const {return m_id;}
    const std::string& getName() const {return m_name;}

    void join();

public:
    //获取系统分配的线程id
    static pid_t getThreadId();

    //获取当前所在线程
    static Thread* getThis();

    //获取当前线程的名称
    static const std::string& getCurrentThreadName();

    //设置当前线程的名称
    static void setCurrentThreadName(const std::string& name);

private:
    //线程函数
    /*
        非静态成员函数实际上有一个隐含的参数 —— this 指针，它在函数调用时会被编译器自动添加：
        当你定义这样的成员函数:
        void Thread::someMethod(int a) { ... }

        实际上编译器会处理为类似于:
        void Thread_someMethod(Thread* this, int a) { ... }

        而pthread_create函数的第三个参数是一个函数指针，它的类型是 void* (*)(void*)，也就是说它是一个函数指针，
        指向一个函数，这个函数的参数和返回值都是 void* 类型。 为了让一个非静态成员函数符合这个函数指针的类型，我
        们可以将这个非静态成员函数转换为一个静态成员函数，这样就没有 this 指针了，也就符合了 pthread_create 函数
        的要求。
    */
    static void* run(void* arg);

private:
    pid_t m_id = -1;
    pthread_t m_thread = 0;

    //线程需要运行的函数
    std::function<void()> m_cb;
    std::string m_name;

    Semaphore m_semaphore;

};

