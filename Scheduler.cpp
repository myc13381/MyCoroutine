#include "Scheduler.h"

// 当前线程的调度器，同一个调度器下所有协程共享同一个实例
static thread_local Scheduler *t_scheduler = nullptr;

// 当前线程的调度协程
static thread_local Fiber *t_scheduler_fiber = nullptr;

Scheduler::Scheduler(size_t threads, bool use_caller, const std::string &name)
{
    assert(threads > 0);

    m_useCaller = use_caller;
    m_name = name;

    if(use_caller)
    { // main函数所在线程是否分出资源进行协程任务的工作
        --threads;
        Fiber::GetThis();
        assert(this->GetThis() == nullptr);
        t_scheduler = this;

        // caller线程的主协程不会被线程的调度协程run进行调度，而且线程的调度协程停止时，应该返回caller线程的主协程
        // 在use caller情况下，把caller线程的主协程暂时保存起来，等调度协程结束时，再resume caller协程
        m_rootFiber.reset(new Fiber(std::bind(&Scheduler::run, this), 0, false));

        t_scheduler_fiber = m_rootFiber.get();
        m_rootThread = std::this_thread::get_id();
        m_threadIds.emplace_back(m_rootThread);
    }
    else m_rootThread = std::thread::id(-1);

    m_threadCount = threads;
}

Scheduler *Scheduler::GetThis()
{
    return t_scheduler;
}

Fiber *Scheduler::GetScheduleFiber()
{
    return t_scheduler_fiber;
}

void Scheduler::setThis()
{
    t_scheduler = this;
}

Scheduler::~Scheduler()
{
    assert(this->m_stopping);
    if(GetThis() == this) t_scheduler = nullptr;
}

void Scheduler::start()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if(m_stopping)
    {
        return;
    }
    // 调度器刚刚启动时应该没有线程执行
    MYASSERT(m_threads.empty(), "m_threads is not empty");
    m_threads.resize(m_threadCount);
    for(size_t i = 0; i < m_threadCount; ++i)
    {
        m_threads[i].reset(new std::thread(std::bind(&Scheduler::run, this), m_name + "_" + std::to_string(i)));
        m_threadIds.emplace_back(m_threads[i]->get_id());
    }
}

bool Scheduler::stopping()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return (m_stopping && m_tasks.empty() && m_activeThreadCount == 0);
}

void Scheduler::idle()
{
    while(!stopping())
    {
        Fiber::GetThis()->yield();
    }
}

void Scheduler::tickle()
{

}

void Scheduler::stop()
{
    if(stopping())
    {
        return;
    }
    m_stopping = true;

    // 如果协程被调度器调度，则只能由调度器发起停止
    if(m_useCaller)
    {
        MYASSERT(GetThis() == this, "only scheduler could stop");
    }
    else 
    {
        MYASSERT(GetThis() != this, "only scheduler could stop");
    }

    for(size_t i = 0; i < m_threadCount; i++)
    {
        // 因为要结束调度器，所以通知其他线程赶快工作
        tickle();
    }
    // 在use caller情况下，调度器协程结束时，应该返回caller协程
    if(m_rootFiber)
    {
        tickle();
        m_rootFiber->resume();
    }
    
    std::vector<std::shared_ptr<std::thread>> thrs;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        thrs.swap(m_threads);
    }
    for(auto &t : thrs) t->join();
}

// 协程调度函数
void Scheduler::run()
{
    // 刚创建线程时的初始化操作
    setThis(); // 设置当前线程的调度器
    if(std::this_thread::get_id() != m_rootThread)
    { // 在非caller线程里，调度协程就是非caller线程的主协程，也就是说每个线程都必须有自己的调度协程用于进行协程间的切换
        t_scheduler_fiber = Fiber::GetThis().get();
    }
    Fiber::ptr idle_fiber(new Fiber(std::bind(&Scheduler::idle, this)));

    // 进行协程调度
    Fiber::ptr cb_fiber;
    ScheduleTask task;
    for(;;)
    {
        task.reset();
        bool tickle_me = false;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            auto it = m_tasks.begin();
            // 遍历所有任务
            while(it != m_tasks.end())
            {
                if(it->thread != std::thread::id(-1) && it->thread != std::this_thread::get_id())
                { // 指定了调度线程，但是不是在当前线程上调度，标记一下需要通知其他线程进行调度
                    ++it;
                    tickle_me = true;
                    continue;
                }
                // [fix bug]
                // 多线程高并发情境下，有可能发生刚添加事件就被触发的情况，如果此时当前协程还未来得及yield，则这里就有可能出现协程状态仍为RUNNING的情况
                // 这里简单地跳过这种情况，以损失一点性能为代价，否则整个协程框架都要大改
                if(it->fiber && it->fiber->getState() == Fiber::RUNNING)
                {
                    ++it;
                    continue;
                }

                // 到此位置，找到一个调度任务，准备开始调度，将其从任务队列中剔除，活动线程数加1
                task = *it;
                m_tasks.erase(it++);
                ++m_activeThreadCount;
                break;
            }
            // 当前线程拿完一个任务后，发现任务队列还有剩余，那么tickle一下其他线程
            tickle_me |= (it != m_tasks.end());
        }

        if(tickle_me)
        {
            tickle();
        }
        // 执行任务
        if(task.fiber)
        {
            // resume协程，resume返回时，协程要么执行完了，要么半路yield了，总之这个任务就算完成了，活跃线程数减1
            task.fiber->resume();
            --m_activeThreadCount;
            task.reset();
        }
        else if(task.cb)
        { // 转化为协程
            if(cb_fiber) cb_fiber->reset(task.cb);
            else cb_fiber.reset(new Fiber(task.cb));
            task.reset();
            cb_fiber->resume();
            --m_activeThreadCount;
            cb_fiber.reset();
        }
        else
        { // 至此，任务队列空了，调度idle协程
            if(idle_fiber->getState() == Fiber::TERM)
            { // 如果调度器没有调度任务，那么idle协程会不停地resume/yield，不会结束，如果idle协程结束了，那一定是调度器停止了
                break;
            }

            ++m_idleThreadCount;
            idle_fiber->resume();
            --m_idleThreadCount;
        }
    }
}
