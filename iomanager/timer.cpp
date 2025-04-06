#include "timer.h"

bool Timer::cancel()
{
    std::unique_lock<std::shared_mutex> write_lock(m_manage->m_mutex);

    if(m_cb == nullptr)
    {
        return false;
    }
    else
    {
        m_cb = nullptr;
    }

    auto it = m_manage->m_timers.find(shared_from_this());  //从定时管理器中找到需要删除的定时器
    if(it != m_manage->m_timers.end())
    {
        m_manage->m_timers.erase(it);   //删除定时器
    }
    return true;
}

 // refresh 只会向后调整
bool Timer::refresh()
{
    std::unique_lock<std::shared_mutex> write_lock(m_manage->m_mutex);

    if(!m_cb)
    {
        return false;
    }

    auto it = m_manage->m_timers.find(shared_from_this());
    if(it == m_manage->m_timers.end())
    {
        return false;
    }

    m_manage->m_timers.erase(it);
    m_next = std::chrono::system_clock::now() + std::chrono::milliseconds(m_ms);
    m_manage->m_timers.insert(shared_from_this());
    return true;

}

bool Timer::reset(uint64_t ms, bool from_now)
{
    if(ms == m_ms && !from_now) //检查是否要重置
    {
        return true;    //代表不需要重置
    }

    //如果不满足上面的条件需要重置，删除当前的定时器然后重新计算超时时间并重新插入定时器
    {
        std::unique_lock<std::shared_mutex> write_lock(m_manage->m_mutex);

        if(!m_cb)   //如果为空，说明该定时器已经被取消或未初始化，因此无法重置
        {
            return false;
        }

        //否则就是定时器已经初始化了
        auto it = m_manage->m_timers.find(shared_from_this());  //寻找定时器
        if(it == m_manage->m_timers.end())  //没找到定时器
        {
            return false;
        }
        m_manage->m_timers.erase(it);   //找到删除
    }

    //reinsert
    //如果为true则重新计算超时时间，为false就需要上一次的起点开始
    auto start = from_now ? std::chrono::system_clock::now() : m_next - std::chrono::milliseconds(m_ms);
    m_ms = ms;
    m_next = start + std::chrono::milliseconds(m_ms);
    m_manage->addTimer(shared_from_this());  // insert with lock
    return true;
}

Timer::Timer(uint64_t ms, std::function<void()> cb, bool recurring, TimerManager *manager):
            m_recurring(recurring), m_ms(ms), m_cb(cb), m_manage(manager)
{
    auto now = std::chrono::system_clock::now();
    m_next = now + std::chrono::milliseconds(m_ms);
}

bool Timer::Comparator::operator()(const std::shared_ptr<Timer> &lhs, const std::shared_ptr<Timer> &rhs) const
{
    assert(lhs != nullptr && rhs != nullptr);
    return lhs->m_next < rhs->m_next;
}



TimerManager::TimerManager()
{
    m_previousTime = std::chrono::system_clock::now();
}

TimerManager::~TimerManager()
{
}

std::shared_ptr<Timer> TimerManager::addTimer(uint64_t ms, std::function<void()> cb, bool recurring)
{
   std::shared_ptr<Timer> timer(new Timer(ms, cb, recurring, this));
   addTimer(timer);
   return timer;
}

static void onTimer(std::weak_ptr<void> weak_cond, std::function<void()> cb)
{
    std::shared_ptr<void> tmp = weak_cond.lock();
    if(tmp)
    {
        cb();
    }
}

std::shared_ptr<Timer> TimerManager::addConditionTimer(uint64_t ms, std::function<void()> cb, std::weak_ptr<void> weak_cond, bool recurring)
{
    return addTimer(ms, std::bind(&onTimer, weak_cond, cb), recurring);
}

uint64_t TimerManager::getNextTimer()
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);

    //reset m_tickled
    /*
        m_tickled是一个标志：用于指示是否需要在定时器插入到时间堆的前端时触发额外的处理操作，
        例如唤醒一个等待的线程或进行其他管理操作。 设置为false的意义就在于能继续在addtimer
        重新触发插入定时器如果是最早的超时定时器，能正常触发as_fornt;
    */
    m_tickled = false;

    if(m_timers.empty())
    {
        //返回最大值
        return ~0ull;
    }

    auto now = std::chrono::system_clock::now();
    auto time = (*m_timers.begin())->m_next;

    if(now >= time)
    {
        //已经有timer超时
        return 0;
    }
    else
    {
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(time - now);
        return static_cast<uint64_t>(duration.count());
    }
}

void TimerManager::listExpiredCb(std::vector<std::function<void()>> &cbs)
{
    auto now = std::chrono::system_clock::now();

    std::unique_lock<std::shared_mutex> write_lock(m_mutex);

    bool rollover = detectClockRollover();

    //如果时间回滚发生或者定时器的超时时间早于或等于当前时间，则需要处理这些定时器
    while(!m_timers.empty() && (rollover || (*m_timers.begin())->m_next <= now))
    {
        std::shared_ptr<Timer> temp = *m_timers.begin();
        m_timers.erase(m_timers.begin());

        cbs.push_back(temp->m_cb);

        if(temp->m_recurring)
        {
            //重新加入时间堆
            temp->m_next = now + std::chrono::milliseconds(temp->m_ms);
            m_timers.insert(temp);
        }
        else
        {
            //清理cb
            temp->m_cb = nullptr;
        }
    }
}

bool TimerManager::hasTimer()
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    return !m_timers.empty();
}

void TimerManager::addTimer(std::shared_ptr<Timer> timer)
{
    bool at_front = false;
    {
        std::unique_lock<std::shared_mutex> write_lock(m_mutex);
        auto it = m_timers.insert(timer).first;
        at_front = (it == m_timers.begin()) && !m_tickled;

        if(at_front)
        {
            m_tickled = true;
        }
    }

    if(at_front)
    {
        onTimerInsertedAtFront();
    }

}

bool TimerManager::detectClockRollover()
{
    bool rollover = false;
    auto now = std::chrono::system_clock::now();
    if(now < (m_previousTime - std::chrono::milliseconds(60 * 60 * 1000)))
    {
        rollover = true;
    }
    m_previousTime = now;
    return rollover;
}
