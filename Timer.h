// 封装定时器
#pragma once
#include <functional>
#include <memory>
#include <vector>
#include <set>
#include <mutex>
#include <shared_mutex>
#include <chrono>
#include <iostream>




// 获得当前时间的时间戳，毫秒
inline std::chrono::milliseconds getNow()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock().now().time_since_epoch());
}

// forward declear
class TimerManager;

// 定时器
class Timer : public std::enable_shared_from_this<Timer>
{
    friend TimerManager;
public:
    typedef std::shared_ptr<Timer> ptr;
    typedef std::shared_mutex RWMutexType; // 读写锁
    typedef std::unique_lock<std::shared_mutex> WriteLock;
    typedef std::shared_lock<std::shared_mutex> ReadLock;
private:
    // 私有构造函数
    Timer(std::chrono::milliseconds ms, std::function<void()> cb, bool recurring, TimerManager *manager);
    Timer(std::chrono::milliseconds next);

public:
    // 取消定时器
    bool cancel();
    // 刷新设置定时器执行时间
    bool refresh();
    // 重置定时器的时间
    bool reset(std::chrono::milliseconds ms, bool from_now);

private:
    bool m_recurring = false;                                           // 是否循环
    std::chrono::milliseconds m_ms = std::chrono::milliseconds(0);      // 多久之后执行，相对于创建定时器时间戳的相对时间
    std::chrono::milliseconds m_next = std::chrono::milliseconds(0);    // 精确的执行绝对时间 == 创建时间戳 + m_ms      
    std::function<void()> m_cb;                                         // 回调函数
    TimerManager *m_manager = nullptr;                                  // 定时器管理器
private:
    // 定时器比较仿函数，按执行时间排序
    struct Comparator
    {
        bool operator()(const Timer::ptr &lhs, const Timer::ptr &rhs) const;
    };
};


// 定时器管理器
class TimerManager
{
    friend Timer;
public:
    typedef std::shared_mutex RWMutexType; // 读写锁
    typedef std::unique_lock<std::shared_mutex> WriteLock;
    typedef std::shared_lock<std::shared_mutex> ReadLock;
    // 构造函数
    TimerManager(); // 默认构造函数
    ~TimerManager(); // 析构函数

    // 添加定时器
    Timer::ptr addTimer(std::chrono::milliseconds ms, std::function<void()> cb, bool recurring = false);

    // 添加条件定时器
    Timer::ptr addConditionTimer(std::chrono::milliseconds ms, std::function<void()> cb, 
                                    std::weak_ptr<void> weak_cond, bool recurring = false);

    // 到最近一个定时器执行的时间间隔（毫秒）
    std::chrono::milliseconds getNextTimer();

    // 获取需要执行的过期的定时器的回调函数列表
    void listExpiredCb(std::vector<std::function<void()>> &cbs);

    // 是否有定时器
    bool hasTimer();

protected:
    // 有新的定时器插入到定时器的首部时，执行该函数
    virtual void onTimerInsertedAtFront() = 0; // 纯虚函数

    // 将定时器添加到管理器中
    void addTimer(Timer::ptr val, WriteLock &lock);

private:
    // 检测服务器时间是否被调后了
    bool detectClockRollover(std::chrono::milliseconds now_ms);

private:
    // Mutex
    RWMutexType m_mutex;                                                        // 读写锁
    std::set<Timer::ptr, Timer::Comparator> m_timers;                           // 定时器集合，内部保存定时器的智能指针
    bool m_tickled = false;                                                     // 是否触发 onTimerInsertedAtFront
    std::chrono::milliseconds m_previouseTime = std::chrono::milliseconds(0);   // 上次执行时间

};

