// 协程调度器
#pragma once
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <mutex>
#include <thread>
#include "Fiber.h"

// 协程调度器
// 封装的是N-M的协程调度器，内部有一个线程池，支持协程在线程池里面切换


class Scheduler
{
public:
    typedef std::shared_ptr<Scheduler> ptr;
    typedef std::mutex MutexType;

    // 构造函数
    Scheduler(size_t threads = 1, bool use_caller = true, const std::string &name = "scheduler");

    // 析构函数
    virtual ~Scheduler();

    // 获取调度器的名称
    const std::string &getName() const {return m_name;}

    // 获取当前线程调度器指针
    static Scheduler *GetThis();

    // 获取当前线程的调度协程
    static Fiber *GetScheduleFiber();

    // 添加调度任务
    template <typename FiberOrcb>
    void schedule(FiberOrcb fc, std::thread::id thread = std::thread::id(-1))
    {
        bool need_tickle = false;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            need_tickle = scheduleNoLock(fc, thread);
        }
        if(need_tickle) tickle(); // 唤醒idle协程
    }

    // 启动调度器
    void start();

    // 停止调度器
    void stop();

protected:
    // 通知协程调度器有任务了
    virtual void tickle();

    // 协程调度函数
    void run();

    // 无任务调度时执行idle协程
    virtual void idle();

    // 返回是否可以停止
    virtual bool stopping();

    // 设置当前协程的调度器
    void setThis();

    // 返回是否有空闲线程
    bool hasIdleThreads() { return m_idleThreadCount > 0; }


private:
    // 添加调度任务，无锁
    // FiberOrCB 可以是协程对象也可以是函数指针
    template <typename FiberOrCb>
    bool scheduleNoLock(FiberOrCb fc, std::thread::id thread)
    {
        bool need_tickle = m_tasks.empty();
        ScheduleTask task(fc, thread);
        if(task.fiber || task.cb)
        {
            m_tasks.push_back(task);
        }
        return need_tickle;
    }

private:

    // 调度任务，协程/函数二选一，可指定在哪个线程上调度
    struct ScheduleTask
    {
        Fiber::ptr fiber; 
        std::function<void()> cb;
        std::thread::id thread; // 在哪个线程上调度
        
        // 在这里创建协程
        ScheduleTask(Fiber::ptr f, std::thread::id thr) : fiber(f), thread(thr) {}
        ScheduleTask(Fiber::ptr *f, std::thread::id thr)
        {
            fiber.swap(*f);
            thread = thr;
        }
        ScheduleTask(std::function<void()> f, std::thread::id thr)
        {
            cb = f;
            thread = thr;
        }
        ScheduleTask() { thread = std::thread::id(-1); }

        void reset()
        {
            fiber = nullptr;
            cb = nullptr;
            thread = std::thread::id(-1);
        }
    };

private:
    std::string m_name;                                 // 协程调度器名称
    MutexType m_mutex;                                  // 互斥锁
    std::vector<std::shared_ptr<std::thread>> m_threads;// 线程池
    std::list<ScheduleTask> m_tasks;                    // 任务队列
    std::vector<std::thread::id> m_threadIds;           // 记录工作线程的id
    size_t m_threadCount = 0;                           // 工作线程的数量，不包含 use_caller 的主线程
    std::atomic<size_t> m_activeThreadCount {0};        // 活跃的线程数量
    std::atomic<size_t> m_idleThreadCount {0};          // idle线程数量

    bool m_useCaller;                                   // 是否直接利用main函数所在线程进行协程工作
    Fiber::ptr m_rootFiber;                             // m_useCaller为true时，调度器所在线程的调度协程
    std::thread::id m_rootThread = std::thread::id(-1); // m_userCaller为true时，调度器所在线程的ID

    bool m_stopping = false;                            // 调度器是否正在停止
};