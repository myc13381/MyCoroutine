#include <unistd.h>     // pipe()
#include <sys/epoll.h>  // epoll
#include <fcntl.h>      // fcntl()
#include <string.h>     // memset
#include "IOManager.h"

IOManager::FdContext::EventContext &IOManager::FdContext::getEventContext(IOManager::Event event)
{
    switch(event)
    {
        case IOManager::READ:
            return read;
        case IOManager::WRITE:
            return write;
        default:
            MYASSERT(false, "getContext");
    }
    // 意外情况抛出异常
    throw std::invalid_argument("getContext invalid event");
}

void IOManager::FdContext::resetEventContext(EventContext &ctx)
{
    ctx.scheduler = nullptr;
    ctx.fiber.reset();
    ctx.cb = nullptr;
}

void IOManager::FdContext::triggerEvent(IOManager::Event event)
{
    // 待触发的事件必须已经被注册过
    assert(events & event);

    // 清除该事件，表示不再关注该事件了
    // 也就是说，注册的IO事件是一次性的，如果想持续关注某个socket fd的读写事件，那么每次触发事件之后都要重新添加
    events = static_cast<Event>(events & ~event);
    // 调度对应的协程
    EventContext &ctx = getEventContext(event);
    if(ctx.cb)
    { // 通过函数添加
        ctx.scheduler->schedule(ctx.cb);
    }
    else
    { // 通过协程添加
        ctx.scheduler->schedule(ctx.fiber);
    }
    resetEventContext(ctx);
    return;
}

IOManager::IOManager(size_t threads, bool use_caller, const std::string &name) : Scheduler(threads, use_caller, name)
{
    // 初始化epoll
    m_epfd = epoll_create(1024);
    assert(m_epfd > 0);
    
    // 创建管道
    int rt = pipe(m_tickleFds);
    assert(!rt);

    // pipe读句柄的可读事件，用于tickle协程
    epoll_event event;
    memset(&event, 0, sizeof(epoll_event));
    event.events = EPOLLIN | EPOLLET; // 读事件 + 边缘触发ET
    event.data.fd = m_tickleFds[0];

    // 非阻塞方式，配合边缘触发ET
    rt = fcntl(m_tickleFds[0], F_SETFL, O_NONBLOCK); // 非阻塞
    assert(!rt);

    rt = epoll_ctl(m_epfd, EPOLL_CTL_ADD, m_tickleFds[0], &event);
    assert(!rt);

    contextResize(32);
    // 这里直接开始了协程调度器的调度
    start();
}

IOManager::~IOManager()
{
    stop(); // 协程调度器停止
    // 释放文件句柄
    close(m_epfd);
    close(m_tickleFds[0]);
    close(m_tickleFds[1]);

    for(size_t i = 0; i < m_fdContexts.size(); ++i)
    {
        if(m_fdContexts[i])
        {
            delete m_fdContexts[i];
        }
    }
}

void IOManager::contextResize(size_t size)
{
    m_fdContexts.resize(size);
    
    // 如果需要的的话，初始化新加入的fd上下文
    for(size_t i = 0; i < m_fdContexts.size(); ++i)
    {
        if(!m_fdContexts[i])
        {
            m_fdContexts[i] = new FdContext;
            m_fdContexts[i]->fd = i;
        }
    }
}

int IOManager::addEvent(int fd, Event event, std::function<void()> cb)
{
    // 找到fd对应的FdContexts，如果不存在就分配一个 --> fdContexts 扩容
    FdContext *fd_ctx = nullptr;
    ReadLock lk_R(m_mutex);
    if(static_cast<int>(m_fdContexts.size()) > fd)
    {
        fd_ctx = m_fdContexts[fd];
        lk_R.unlock();
    }
    else
    { // m_fdContexts长度不够，进行扩容
        lk_R.unlock();
        WriteLock lk_W(m_mutex);
        contextResize(fd + fd > 1);
        fd_ctx = m_fdContexts[fd];
    }
    // 同一个fd不允许添加同一个事件
    MYASSERT(!(fd_ctx->events & event), "can not add same event in the same fd");

    // 将新的事件加入epoll_wait，使用epoll_event的私有指针存储FdContext的位置
    int op = fd_ctx->events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    epoll_event epevent;
    epevent.events = EPOLLET | fd_ctx->events | event;
    epevent.data.ptr = fd_ctx;

    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if(rt)
    {
        return -1;
    }

    // 待执行IO事件加一
    ++m_pendingEventCount;

    // 找到这个fd的event事件对应的EventContext，对其中的scheduler、cb、fiber进行赋值
    fd_ctx->events = static_cast<Event>(fd_ctx->events | event); // 设置事件类型
    FdContext::EventContext &event_ctx = fd_ctx->getEventContext(event);
    // 断言检查协程执行相关资源是否正常
    assert(!event_ctx.scheduler && !event_ctx.fiber && !event_ctx.cb);
    event_ctx.scheduler = Scheduler::GetThis(); // 设置事件调度器
    // 设置回调函数或者协程
    if(cb)
    {
        event_ctx.cb.swap(cb);
    }
    else
    {
        event_ctx.fiber = Fiber::GetThis();
        assert((event_ctx.fiber->getState() == Fiber::RUNNING));
    }
    return 0;
}

bool IOManager::delEvent(int fd, Event event)
{
    // 找到df对应的FdContext
    ReadLock lk_R(m_mutex);
    if(static_cast<int>(m_fdContexts.size() <= fd)) return false; // 不存在

    FdContext *fd_ctx = m_fdContexts[fd];
    lk_R.unlock();
    std::unique_lock<std::mutex> lk(fd_ctx->mtx);
    if(!(fd_ctx->events & event))
    { // 删除的事件类型不存在
        return false;
    }

    // 清除指定事件，表示不关心这个事件了，如果清除之后结果为0，则从epoll_wait中删除该文件描述符
    Event new_events = static_cast<Event>(fd_ctx->events & ~event);
    int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events = EPOLLET | new_events;
    epevent.data.ptr = fd_ctx;

    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if(rt) 
    {
        return false;
    }

    // 待执行事件数目减一
    --m_pendingEventCount;

    // 重置fd对应event事件的上下文
    fd_ctx->events = new_events;
    FdContext::EventContext &event_ctx = fd_ctx->getEventContext(event);
    fd_ctx->resetEventContext(event_ctx); // 重置回调函数事件
    return true;
}

bool IOManager::cancelEvent(int fd, Event event)
{
    // 找到fd对应的FdContext
    ReadLock lk_R(m_mutex);
    if(static_cast<int>(m_fdContexts.size() <= fd))
    {
        return false;
    }
    FdContext *fd_ctx = m_fdContexts[fd];
    lk_R.unlock();

    std::unique_lock<std::mutex> lk(fd_ctx->mtx);
    if(!(fd_ctx->events & event))
    { // 删除的事件类型不存在
        return false;
    }

    // 开始删除事件
    Event new_events = static_cast<Event>(fd_ctx->events & ~event);
    int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events = EPOLLET | new_events;
    epevent.data.ptr = fd_ctx;

    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if(rt)
    {
        return false;
    }

    // 删除之前触发一次事件
    fd_ctx->triggerEvent(event);
    // 活跃的事件数量减一
    --m_pendingEventCount;
    return true;
}

bool IOManager::cancalAll(int fd)
{
    // 找到fd对应的FdContext
    ReadLock lk_R(m_mutex);
    if(static_cast<int>(m_fdContexts.size() <= fd))
    {
        return false;
    }
    FdContext *fd_ctx = m_fdContexts[fd];
    lk_R.unlock();

    std::unique_lock<std::mutex> lk(fd_ctx->mtx);
    if(!fd_ctx->events)
    { // 如果没有任何类型事件可以删除 返回false
        return false;
    }

    // 删除全部事件
    int op = EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events = 0;
    epevent.data.ptr = fd_ctx;

    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if(rt) 
    {
        return false;
    }

    // 触发全部已经注册的事件
    if(fd_ctx->events & READ)
    {
        fd_ctx->triggerEvent(READ);
        --m_pendingEventCount;
    }
    if(fd_ctx->events & WRITE)
    {
        fd_ctx->triggerEvent(WRITE);
        --m_pendingEventCount;
    }

    assert(fd_ctx->events == 0);
    return true;
}

IOManager *IOManager::GetThis()
{ // 动态类型检查
    return dynamic_cast<IOManager *>(Scheduler::GetThis());
}

// 通知调度协程，也就是Scheduler::run()从idle中退出
// Scheduler::run()每次从idle协程中退出后，都会帮任务队列里的所有任务执行完了再重新进入idle
// 如果没有调度线程处于idle状态，那也就没必要发通知了
void IOManager::tickle()
{
    if(!hasIdleThreads())
    {
        return;
    }
    // 向管道中写数据
    int rt = write(m_tickleFds[1], "T", 1);
    assert(rt == 1);
}

bool IOManager::stopping()
{
    std::chrono::milliseconds timeout(0);
    return stopping(timeout);
}

bool IOManager::stopping(std::chrono::milliseconds &timeout)
{
    // 对于IOManager而言，必须等待所有待调度的IO事件都执行完毕以后才可以退出
    // 增加定时器功能之后，还应该保证没有剩余的定时器待触发
    timeout = getNextTimer();
    return (timeout == std::chrono::milliseconds(~0ull) && m_pendingEventCount == 0 && Scheduler::stopping());
}

// 调度协程无调度任务时会阻塞在idle协程上，对于IO调度器而言，idle状态应该关注两件事
// 一是有没有新的调度任务，对应Scheduler::schedule(),如果有新的调度任务，那应该立即退出idle状态并执行对应任务
// 二是关注当前注册的所有IO事件有没有触发，如果有触发，那么应该执行IO事件对应的回调函数

void IOManager::idle()
{
    // 一次epoll_wait最多检测256个事件，如果就绪事件超过了这个数，那么会在下一轮epoll_wait继续处理
    const uint64_t MAX_EVENTS = 256;
    epoll_event *events = new epoll_event[MAX_EVENTS];
    std::shared_ptr<epoll_event> shared_events(events, [](epoll_event *ptr)
        {delete [] ptr;}); // 自定义函数
    
    while(true)
    {
        // 判断调度器是否可以停止，同时获取下一次超时时间
        std::chrono::milliseconds next_timeout(0);
        if(stopping(next_timeout))
        {
            break;
        }

        // 阻塞在epoll_wait上，等待事件发生或者定时器超时
         int rt = 0;
        // do 
        // {
        //     //std::cout<<"epoll_wait...\n";
        //     // 默认超时5秒，如果下一个定时器的超时时间大于5秒，仍以5秒来计算超时
        //     // 避免定时器超时时间太大时，epoll_wait一直阻塞
        //     static const int MAX_TIMEOUT = 5000; // 5 seconds
        //     auto temp = std::chrono::milliseconds(~0ull);
        //     if(next_timeout != std::chrono::milliseconds(~0ull))
        //     {
        //         next_timeout = std::min(next_timeout, std::chrono::milliseconds(MAX_TIMEOUT));
        //     }
        //     else next_timeout = std::chrono::milliseconds(MAX_TIMEOUT); // 没有事件，也等待5秒

        //     // 调用epoll_wait
        //     int rt = epoll_wait(m_epfd, events, MAX_EVENTS, static_cast<int>(next_timeout.count()/1000)); // 单位秒
        //     if(rt < 0 && errno == EINTR)
        //     { // 如果遇到中断，继续处理
        //         continue;
        //     }
        //     else break; // 读取完毕
        // }while(true);

        // 处理定时器的操作
        // 收集所有已经超时的定时器，执行回调函数
        std::vector<std::function<void()>> cbs;
        listExpiredCb(cbs);
        if(!cbs.empty())
        {
            for(auto &cb : cbs)
            {
                schedule(cb);
            }
            cbs.clear();
        }

        // 遍历所有发生的事情，根据epoll_event的私有指针找到对应的FdContext，进行事件处理
        for(int i = 0; i < rt; ++i)
        {
            epoll_event &event = events[i];
            if(event.data.fd == m_tickleFds[0])
            { // m_tickleFds[0]用于通知协程调度，这时只需要把管道里的内容读完即可
                std::cout<<"定时器触发"<<std::endl;
                uint8_t dummy[256];
                while(read(m_tickleFds[0], dummy, sizeof(dummy)) > 0) {}
                continue;
            }

            FdContext *fd_ctx = static_cast<FdContext *>(event.data.ptr);
            std::unique_lock<std::mutex> lk(fd_ctx->mtx); // 临界资源的操作，上锁

            /*
             * EPOLLERR：出错，比如读写端已经关闭的pipe
             * EPOLLHUB：套接字对端关闭
             * 出现这两种事件，应该同时触发fd的读写事件，否则有可能出现注册的事件永远执行不到的情况
             */

            if(event.events & (EPOLLERR | EPOLLHUP))
            {
                event.events |= ((EPOLLIN | EPOLLOUT) & fd_ctx->events);
            }

            
            int real_events = NONE;
            if(event.events & EPOLLIN)  real_events |= READ;
            if(event.events & EPOLLOUT) real_events |= WRITE;

            if((fd_ctx->events & real_events) == NONE)
            { // 触发的事件类型和对调函数事件类型不匹配，直接跳过
                continue;
            }

            // 删除已经发生的事件，将剩下的事件重新加入epoll_wait
            int left_events = (fd_ctx->events & ~real_events); // 剩下的事件类型
            int op = left_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
            event.events = EPOLLET | left_events;

            if(epoll_ctl(m_epfd, op, fd_ctx->fd, &event))
            {
                std::cerr<<"epoll_ctl faild!\n";
                assert(false);
                continue;
            }

            // 处理已经发生的事件，也就是让调度器调度指定的函数或者协程
            if(real_events & READ)
            {
                fd_ctx->triggerEvent(READ);
                --m_pendingEventCount;
            }
            if(real_events & WRITE)
            {
                fd_ctx->triggerEvent(WRITE);
                --m_pendingEventCount;
            }
        } // for 循环结束

        // 一旦处理完所有事件，idle协程yield，这样可以让调度协程(Scheduler::run)重新检查是否有新的任务需要调度
        // 上面的triggerEvent实际上也只是将对应的fiber加入调度，实际执行还要等待idle协程退出
        Fiber::ptr cur = Fiber::GetThis();
        auto raw_ptr = cur.get();
        cur.reset();
        raw_ptr->yield();
    } // while(true) 结束
}

void IOManager::onTimerInsertedAtFront()
{
    tickle();
}