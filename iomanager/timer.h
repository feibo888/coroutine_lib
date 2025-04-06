#pragma once

#include <iostream>
#include <memory>
#include <chrono>
#include <functional>
#include <vector>
#include <shared_mutex>
#include <set>
#include <mutex>
#include <assert.h>

class TimerManager;


class Timer : public std::enable_shared_from_this<Timer>
{
    friend class TimerManager;  //设置成友元访问timerManager类的函数和成员变量
public:
    //从时间堆中删除timer
    bool cancel();
    //刷新timer
    bool refresh();
    //重设timer的超时时间
    bool reset(uint64_t ms, bool from_now);

private:
    Timer(uint64_t ms, std::function<void()> cb, bool recurring, TimerManager* manager);

private:
    //是否循环
    bool m_recurring = false;
    //超时时间
    uint64_t m_ms = 0;
    //绝对超时时间
    std::chrono::time_point<std::chrono::system_clock> m_next;
    //超时时触发的回调函数
    std::function<void()> m_cb;
    //管理此Timer的管理器
    TimerManager* m_manage = nullptr;

private:
    //实现最小堆的比较函数
    struct Comparator
    {
        bool operator()(const std::shared_ptr<Timer>& lhs, const std::shared_ptr<Timer>& rhs) const;
    };


};

class TimerManager
{
    friend class Timer;
public:
    TimerManager();
    virtual ~TimerManager();

    //添加Timer
    std::shared_ptr<Timer> addTimer(uint64_t ms, std::function<void()> cb, bool recurring = false);

    //添加条件Timer
    std::shared_ptr<Timer> addConditionTimer(uint64_t ms, std::function<void()> cb, 
                                                std::weak_ptr<void> weak_cond, 
                                                bool recurring = false);

    //拿到堆中最近的超时时间
    uint64_t getNextTimer();

    //取出所有超时定时器的回调函数
    void listExpiredCb(std::vector<std::function<void()>>& cbs);

    //堆中是否有timer
    bool hasTimer();

protected:
    //当一个最早的timer加入到堆中，调用该函数
    //设置为protected，只能在TimerManager中调用或者子类中重写调用
    virtual void onTimerInsertedAtFront(){}

    //添加timer
    //设置为protected，和公共接口addTimer()配合使用，公共接口负责创建timer对象，而protected接口负责添加到时间堆中
    void addTimer(std::shared_ptr<Timer> timer);

private:
    //当系统时间改变时，调用该函数
    bool detectClockRollover();

private:
    std::shared_mutex m_mutex;
    //时间堆
    std::set<std::shared_ptr<Timer>, Timer::Comparator> m_timers;

    // 在下次getNextTime()执行前 onTimerInsertedAtFront()是否已经被触发了 -> 
    //在此过程中 onTimerInsertedAtFront()只执行一次
    bool m_tickled = false;
    //上次检查系统时间是否回退的绝对时间
    std::chrono::time_point<std::chrono::system_clock> m_previousTime;

};
