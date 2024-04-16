#include "Timer.h"
#include <thread>

bool Timer::Comparator::operator()(const Timer::ptr &lhs, const Timer::ptr &rhs) const
{
    if(!lhs && !rhs) return false;
    // 至少一个智能指针是存在的，下面代码暂时没有看懂
    if(!lhs) return true;
    if(!rhs) return false; // lhs == nullptr && rhs != nullptr
    if(lhs->m_next < rhs->m_next) return true;
    if(lhs->m_next > rhs->m_next) return false;
    // lhs->m_next == rhs->m_next
    return lhs.get() < rhs.get(); // 比较地址
}

Timer::Timer(std::chrono::milliseconds ms, std::function<void()> cb, bool recurring, TimerManager *manager)
    : m_recurring(recurring), m_ms(ms), m_cb(cb), m_manager(manager), 
    m_next(getNow() + m_ms) // 通过当前时间点和m_ms来初始化
{}

Timer::Timer(std::chrono::milliseconds next) : m_next(next)
{}

bool Timer::cancel()
{
    WriteLock lk(m_manager->m_mutex);
    if(m_cb)
    { // 取消定时器的操作就是将该定时器回调函数置空，然后从对应的定时器管理器将该定时器中移除
        m_cb = nullptr;
        auto it = m_manager->m_timers.find(shared_from_this());
        m_manager->m_timers.erase(it);
        return true;
    }
    return false;
}

bool Timer::refresh()
{
    WriteLock lk(m_manager->m_mutex);
    if(!m_cb) {return false;}

    // 刷新的操作就是取出后重新设置时间然后重新插入
    auto it = m_manager->m_timers.find(shared_from_this());
    if(it == m_manager->m_timers.end()) return false; // 没有找到
    m_manager->m_timers.erase(it);
    m_next = getNow() + m_ms;
    m_manager->m_timers.insert(shared_from_this()); // 重新插入自己
    return true;
}

bool Timer::reset(std::chrono::milliseconds ms, bool from_now)
{
    if(m_ms == ms && !from_now) return true;
    WriteLock lk(m_manager->m_mutex);
    if(!m_cb) return false; // 任务不存在
    auto it = m_manager->m_timers.find(shared_from_this());
    if(it == m_manager->m_timers.end()) return false;

    m_manager->m_timers.erase(it);
    std::chrono::milliseconds start; // 重置的时间
    if(from_now) start = getNow();
    else start = m_next - ms;
    m_ms = ms;
    m_next = start + m_ms;
    m_manager->addTimer(shared_from_this(), lk);
    return true;
}

TimerManager::TimerManager() : m_previouseTime(getNow())
{}

TimerManager::~TimerManager() {}

Timer::ptr TimerManager::addTimer(std::chrono::milliseconds ms, std::function<void()> cb, bool recurring)
{
    Timer::ptr timer(new Timer(ms, cb, recurring, this));
    WriteLock lk(m_mutex);
    addTimer(timer, lk);
    return timer;
}

static void OnTimer(std::weak_ptr<void> weak_cond, std::function<void()> cb)
{
    std::shared_ptr<void> temp = weak_cond.lock();
    if(temp)
    {
        cb(); // 执行回调函数
    }
}

Timer::ptr TimerManager::addConditionTimer(std::chrono::milliseconds ms, std::function<void()> cb, 
                                    std::weak_ptr<void> weak_cond, bool recurring)
{
    return addTimer(ms, std::bind(&OnTimer, weak_cond, cb), recurring);
}

std::chrono::milliseconds TimerManager::getNextTimer()
{
    ReadLock lk(m_mutex);
    m_tickled = false;
    if(m_timers.empty()) return std::chrono::milliseconds(~0ull);

    const Timer::ptr &next = *m_timers.begin();
    std::chrono::milliseconds now = getNow();
    if(now >= next->m_next) return std::chrono::milliseconds(0);
    else return next->m_next - now;
}

void TimerManager::listExpiredCb(std::vector<std::function<void()>> &cbs)
{
    std::chrono::milliseconds now = getNow();
    std::vector<Timer::ptr> expired;
    { // 先使用写入锁判断
        ReadLock lk(m_mutex);
        if(m_timers.empty()) return;
    }

    WriteLock lk(m_mutex);
    if(m_timers.empty()) return;

    bool rollover = false; // 判断服务器时间是不是被调后了
    if(detectClockRollover(now))
    { 
        rollover = true;
    }

    if(!rollover && ((*m_timers.begin())->m_next > now))
    { // 不存在过期的事件
        return;
    }

    Timer::ptr now_timer(new Timer(now));
    auto it = rollover ? m_timers.end() : m_timers.lower_bound(now_timer); // 使用二分查找，找到大于等于的位置
    while(it != m_timers.end() && (*it)->m_next == now) ++it;

    expired.insert(expired.begin(), m_timers.begin(), it);
    m_timers.erase(m_timers.begin(), it);
    cbs.reserve(expired.size()); // 预留空间

    for(auto &timer : expired)
    {
        cbs.emplace_back(timer->m_cb);
        if(timer->m_recurring)
        { // 处理循环的任务
            timer->m_next = now + timer->m_ms;
            m_timers.insert(timer);
        }
        else 
        {
            // 不循环的任务，置空
            timer->m_cb = nullptr;
        }
    }
}

void TimerManager::addTimer(Timer::ptr val, WriteLock &lock)
{
    auto it = m_timers.insert(val).first;
    bool at_front = ((it == m_timers.begin()) && !m_tickled);
    if(at_front) m_tickled = true;
    if(lock.owns_lock()) 
    {
        //std::this_thread::sleep_for(std::chrono::milliseconds(1)); // 延迟一下，不然会死锁？？？
        lock.unlock();
    }
    if(at_front) 
    {
        onTimerInsertedAtFront();
    }
}

bool TimerManager::detectClockRollover(std::chrono::milliseconds now_ms)
{
    bool rollover = false;
    if(now_ms < m_previouseTime && now_ms < (m_previouseTime - std::chrono::milliseconds(60 * 60 * 1000)))
    {   // 如果系统时间后调了一个小时，则标记，在listExpiredCb中会执行所有定时器操作，这里处理比较粗糙
        rollover = true;
    }
    m_previouseTime = now_ms;
    return rollover;
}

bool TimerManager::hasTimer()
{
    ReadLock lk(m_mutex);
    return !m_timers.empty();
}


