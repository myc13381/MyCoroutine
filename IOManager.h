// IO协程调度器

#pragma once
#include "Scheduler.h"
#include "Timer.h"

class IOManager : public Scheduler, public TimerManager
{
public:
    typedef std::shared_ptr<IOManager> ptr;

    // IO事件，继承自epoll对事件的定义
    // 这里只关心socket fd的读写事件，其他类型的epoll事件会归类到这两类事件中
    enum Event
    {
        NONE = 0x0, // 无事件 
        READ = 0x1, // 读事件 EPOLLIN
        WRITE = 0x4 // 写事件 EPOLLOUT
    };
private:
    // socket fd上下文类
    // 每一个socket fd都对应一个FdContext，包括fd值，fd上的事件以及fd的读写事件上下文
    struct FdContext
    {
        typedef std::mutex MutexType; 
        // 事件上下文
        // fd的每个事件都有一个事件上下文，保存这个事件的回调函数以及执行回调函数的调度器
        struct EventContext
        {
            Scheduler *scheduler = nullptr;     // 执行事件回调的调度器
            Fiber::ptr fiber;                   // 事件回调协程
            std::function<void()> cb;           // 事件回调函数
        };

        // 获取事件上下文
        EventContext &getEventContext(Event event);

        // 重置事件上下文
        void resetEventContext(EventContext &ctx);

        // 触发事件
        void triggerEvent(Event event);

        EventContext read;          // 读事件上下文
        EventContext write;         // 写事件上下文
        int fd = 0;                 // 事件
        Event events = NONE;        // fd关心什么时间
        MutexType mtx;              // 互斥锁
    };

public: 
    // 构造函数
    IOManager(size_t threads = 1, bool use_caller = true, const std::string &name = "IOManager");

    // 析构函数
    ~IOManager();

    // 添加事件，添加成功返回0，添加失败返回-1
    int addEvent(int fd, Event event, std::function<void()> cb = nullptr);

    // 删除事件
    bool delEvent(int fd, Event event);

    // 取消事件
    bool cancelEvent(int fd, Event event);

    // 取消所有事件
    bool cancalAll(int fd);

    // 返回当前的IOManager
    static IOManager *GetThis();

protected:
    // 通知调度器有任务要调度
    void tickle() override;

    // 判断是否可以停止
    // 判断条件是Scheduler::stopping()外加IOManager的m_pendingEventCount为0，表示没有IO事件可调度了
    bool stopping() override;

    // 判断是否可以停止，同时获取最近一个定时器的超时时间
    bool stopping(std::chrono::milliseconds &timeout);

    // idle协程，空闲调度
    void idle() override;

    // 当有定时器插入到最前端时，要重新更新epoll_wait的超时时间
    // 这里是唤醒idle协程以便使用新的超时时间
    void onTimerInsertedAtFront() override;

    // 重置socket句柄上下文的容器大小
    void contextResize(size_t size);

private:
    int m_epfd = 0;                                 // epoll 文件句柄
    int m_tickleFds[2];                             // pipe文件句柄，fd[0]读端口，fd[1]写端口，用于在定时器触发时及时退出epoll_wait
    std::atomic<size_t> m_pendingEventCount {0};    // 当前等待执行的IO事件数量
    RWMutexType m_mutex;                            // IOManager的mutex
    std::vector<FdContext *> m_fdContexts;          // socket事件上下文容器
};