#include <dlfcn.h>
#include <stdio.h>
#include <stdarg.h>
#include "Hook.h"
#include "FdManager.h"
#include "IOManager.h"

static thread_local bool t_hook_enable = false; // 每一个线程是否开启hook

#define HOOK_FUN(XX) \
    XX(sleep) \
    XX(socket)\
    XX(connect)\
    XX(accept)\
    XX(read) \
    XX(readv) \
    XX(recv) \
    XX(recvfrom) \
    XX(recvmsg) \
    XX(write) \
    XX(writev) \
    XX(send) \
    XX(sendto) \
    XX(sendmsg) \
    XX(close) \
    XX(fcntl) \
    XX(ioctl) \
    XX(getsockopt) \
    XX(setsockopt)


// 宏定义中 #变量名 表示将变量字符串化 ## 表示将两个宏连接起来
void hook_init()
{
    static bool is_inited = false;
    if(is_inited) 
    {
        return;
    }
#define XX(name) name ## _f = (name ## _fun)dlsym(RTLD_NEXT, #name);
    HOOK_FUN(XX);
#undef XX
}

static std::chrono::milliseconds s_connect_timeout(-1);
struct _HookIniter {
    _HookIniter() {
        hook_init();
        s_connect_timeout = std::chrono::milliseconds(3000);
    }
};

static _HookIniter s_hook_initer;


bool is_hook_enable()
{
    return t_hook_enable;
}

void set_hook_enable(bool flag)
{
    t_hook_enable = flag;
}

struct timer_info
{
    int cancelled = 0;
};

template <typename OriginFun, typename... Args>
static ssize_t do_io(int fd, OriginFun fun, const char *hook_fun_name, uint32_t event, int timeout_so, Args &&... args)
{
    if(!t_hook_enable)
    {
        return fun(fd, std::forward<Args>(args)...);
    }
    
    FdCtx::ptr ctx = FdMgr::GetInstance()->get(fd);
    if(!ctx)
    {
        return fun(fd, std::forward<Args>(args)...);
    }

    /*
     * EBADF 是一个特定的错误代码，它的名字是 "Error BAD File descriptor" 的缩写。
     * 这个错误通常表示一个函数接收了一个无效的文件描述符。文件描述符是一个用于访问文件或套接字等I/O资源的非负整数。
     * 如果传递给某个函数的文件描述符不是一个有效的、打开的描述符，那么该函数可能会失败并设置 errno 为 EBADF。
     */
    if(ctx->isClose())
    {
        errno = EBADF;
        return -1;
    }

    if(!ctx->isSocket() || ctx->getUserNoBlock())
    { // 如果fd不是socket描述符，或者已经被用户设置为非阻塞，可以直接返回该函数，不需要改装
        bool a = ctx->isSocket();
        bool f = ctx->getUserNoBlock();
        return fun(fd, std::forward<Args>(args)...);
    }

    // 处理超时
    std::chrono::milliseconds to = ctx->getTimeout(timeout_so);
    std::shared_ptr<timer_info> tinfo(new timer_info);

retry:
    ssize_t n;
    do
    {
        n = fun(fd, std::forward<Args>(args)...);
    }
    while(n == -1  && errno == EINTR); // 如果因为中断停止，继续尝试

    if(n == -1 && errno == EAGAIN)
    { // EAGAIN表示资源暂时不可用，因此此时会阻塞
        IOManager *iom = IOManager::GetThis();
        Timer::ptr timer;
        std::weak_ptr<timer_info> winfo(tinfo); // 使用弱指针
        // to = std::chrono::milliseconds(1);
        if(to != std::chrono::milliseconds(-1))
        { // 超时时间合法，手动设置一个定时器
            timer = iom->addConditionTimer(to, [winfo, fd, iom, event](){
                auto t = winfo.lock();
                if(!t || t->cancelled)
                {
                    return;
                }
                t->cancelled = ETIMEDOUT;
                iom->cancelEvent(fd, static_cast<IOManager::Event>(event)); // 触发事件让此协程继续
            }, winfo);
        }
        // idle 协程会删除已经触发的事件
        int rt = iom->addEvent(fd, static_cast<IOManager::Event>(event)); // cb为空表示传入当前协程
        if(rt == -1)
        { // 添加失败
            if(timer) timer->cancel();
            return -1;
        }
        else // rt == 0
        { // 添加成功
            Fiber::GetThis()->yield();
            // 协程继续执行有两种情况，一是超时触发，而是epoll检测可读/写
            if(timer) timer->cancel();
            if(tinfo->cancelled)
            {
                errno = tinfo->cancelled;
                return -1;
            }
            goto retry;
        }
    } 
    return n;
}

extern "C"
{
#define XX(name) name ## _fun name ## _f = nullptr;
    HOOK_FUN(XX);
#undef XX

unsigned int sleep(unsigned int seconds)
{
    if(!t_hook_enable)
    {
        return sleep_f(seconds);
    }

    Fiber::ptr fiber = Fiber::GetThis();
    IOManager *iom = IOManager::GetThis();
    iom->addTimer(std::chrono::milliseconds(seconds * 1000), std::bind((void(Scheduler::*)
            (Fiber::ptr, std::thread::id thread))&IOManager::schedule, iom/*this 指针*/, fiber, std::thread::id(-1)));
    Fiber::GetThis()->yield();
    return 0;
}

int socket(int domain, int type, int protocol)
{
    int fd = socket_f(domain, type, protocol);
    if(!t_hook_enable || fd == -1)
    { // 不hook或者fd创建失败，直接返回fd
        return fd;
    }

    FdMgr::GetInstance()->get(fd, true); // 文件句柄管理中注册fd
    return fd;
}

int connect_with_timeout(int fd, const struct sockaddr *addr, socklen_t addrlen, std::chrono::milliseconds timeout_ms)
{
    if(!t_hook_enable) 
    {
        int ret = connect_f(fd, addr, addrlen);
        return ret;
    }
    FdCtx::ptr ctx = FdMgr::GetInstance()->get(fd);

    // int flags = fcntl_f(fd, F_GETFL, 0);
    // if((flags & O_NONBLOCK))
    //     {   // 内部设置为非阻塞
    //         fcntl_f(fd, F_SETFL, flags & ~O_NONBLOCK);
    //     }
    // ctx->setSysNoBlock(false);

    if(!ctx || ctx->isClose())
    {
        errno = EBADF;
        return -1;
    }
    
    if(!ctx->isSocket() || ctx->getUserNoBlock())
    {
        return connect_f(fd, addr, addrlen);
    }

    int n = connect_f(fd, addr, addrlen);
    if(n == 0) 
    {
        // int flags = fcntl_f(fd, F_GETFL, 0);
        // if(!(flags & O_NONBLOCK))
        // {   // 内部设置为非阻塞
        //     fcntl_f(fd, F_SETFL, flags | O_NONBLOCK);
        // }
        // ctx->setSysNoBlock(true);
        return 0;
    }
    else if(n != -1 || errno != EINPROGRESS) return n; // EINPROGRESS 是一个特定的错误码，表示连接操作正在进行中

    IOManager *iom = IOManager::GetThis();
    Timer::ptr timer;
    std::shared_ptr<timer_info> tinfo(new timer_info);
    std::weak_ptr<timer_info> winfo(tinfo);

    // 设置超时定时器
    if(timeout_ms != std::chrono::milliseconds(-1))
    { // 超时时间合法
        timer = iom->addConditionTimer(timeout_ms, [winfo, fd, iom](){
            auto t = winfo.lock();
            if(!t || t->cancelled)
            {
                return;
            }
            t->cancelled = ETIMEDOUT;
            iom->cancelEvent(fd, IOManager::WRITE);
        }, winfo);
    }

    int rt = iom->addEvent(fd, IOManager::WRITE);
    if(rt == 0)
    { // 添加事件成功
        Fiber::GetThis()->yield(); // ???
        if(timer) timer->cancel();
        if(tinfo->cancelled)
        {
            errno = tinfo->cancelled;
            return -1;
        }
    }
    else
    { // 添加事件失败
        if(timer) timer->cancel();
    }

    int error = 0;
    socklen_t len = sizeof(error);
    if(getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) == -1)
    {
        return -1;
    }
    if(!error)
    {
        return 0;
    }
    else 
    {
        errno = error;
        return -1;
    }
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    return connect_with_timeout(sockfd, addr, addrlen, s_connect_timeout);
}

int accept(int s, struct sockaddr *addr, socklen_t *addrlen) {
    int fd = do_io(s, accept_f, "accept", IOManager::READ, SO_RCVTIMEO, addr, addrlen);
    if(fd >= 0) {
        FdMgr::GetInstance()->get(fd, true);
    }
    return fd;
}

ssize_t read(int fd, void *buf, size_t count) {
    return do_io(fd, read_f, "read", IOManager::READ, SO_RCVTIMEO, buf, count);
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt) {
    return do_io(fd, readv_f, "readv", IOManager::READ, SO_RCVTIMEO, iov, iovcnt);
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    return do_io(sockfd, recv_f, "recv", IOManager::READ, SO_RCVTIMEO, buf, len, flags);
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen) {
    return do_io(sockfd, recvfrom_f, "recvfrom", IOManager::READ, SO_RCVTIMEO, buf, len, flags, src_addr, addrlen);
}

ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags) {
    return do_io(sockfd, recvmsg_f, "recvmsg", IOManager::READ, SO_RCVTIMEO, msg, flags);
}

ssize_t write(int fd, const void *buf, size_t count) {
    return do_io(fd, write_f, "write", IOManager::WRITE, SO_SNDTIMEO, buf, count);
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
    return do_io(fd, writev_f, "writev", IOManager::WRITE, SO_SNDTIMEO, iov, iovcnt);
}

ssize_t send(int s, const void *msg, size_t len, int flags) {
    return do_io(s, send_f, "send", IOManager::WRITE, SO_SNDTIMEO, msg, len, flags);
}

ssize_t sendto(int s, const void *msg, size_t len, int flags, const struct sockaddr *to, socklen_t tolen) {
    return do_io(s, sendto_f, "sendto", IOManager::WRITE, SO_SNDTIMEO, msg, len, flags, to, tolen);
}

ssize_t sendmsg(int s, const struct msghdr *msg, int flags) {
    return do_io(s, sendmsg_f, "sendmsg", IOManager::WRITE, SO_SNDTIMEO, msg, flags);
}

int close(int fd)
{
    if(!t_hook_enable)
    {
        return close_f(fd);
    }
    // 如果已经被hook，那么要先取得对应的描述FdCtx，然后将IOManager中该fd添加的所有事件加入协程调度中，最后从FdManager中删除该fd，然后close(fd)
    FdCtx::ptr ctx = FdMgr::GetInstance()->get(fd);
    if(ctx)
    {
        auto iom = IOManager::GetThis();
        if(iom)
        {
            iom->cancalAll(fd);
        }
        FdMgr::GetInstance()->del(fd);
    }
    return close_f(fd);
}

int fcntl(int fd, int cmd, ... /* arg */ ) {
    va_list va;
    va_start(va, cmd);
    switch(cmd) {
        case F_SETFL:
            {
                int arg = va_arg(va, int);
                va_end(va);
                FdCtx::ptr ctx = FdMgr::GetInstance()->get(fd);
                if(!ctx || ctx->isClose() || !ctx->isSocket()) {
                    return fcntl_f(fd, cmd, arg);
                }
                ctx->setUserNoBlock(arg & O_NONBLOCK);
                if(ctx->getSysNoBlock()) {
                    arg |= O_NONBLOCK;
                } else {
                    arg &= ~O_NONBLOCK;
                }
                return fcntl_f(fd, cmd, arg);
            }
            break;
        case F_GETFL:
            {
                va_end(va);
                int arg = fcntl_f(fd, cmd);
                FdCtx::ptr ctx = FdMgr::GetInstance()->get(fd);
                if(!ctx || ctx->isClose() || !ctx->isSocket()) {
                    return arg;
                }
                if(ctx->getUserNoBlock()) {
                    return arg | O_NONBLOCK;
                } else {
                    return arg & ~O_NONBLOCK;
                }
            }
            break;
        case F_DUPFD:
        case F_DUPFD_CLOEXEC:
        case F_SETFD:
        case F_SETOWN:
        case F_SETSIG:
        case F_SETLEASE:
        case F_NOTIFY:
#ifdef F_SETPIPE_SZ
        case F_SETPIPE_SZ:
#endif
            {
                int arg = va_arg(va, int);
                va_end(va);
                return fcntl_f(fd, cmd, arg); 
            }
            break;
        case F_GETFD:
        case F_GETOWN:
        case F_GETSIG:
        case F_GETLEASE:
#ifdef F_GETPIPE_SZ
        case F_GETPIPE_SZ:
#endif
            {
                va_end(va);
                return fcntl_f(fd, cmd);
            }
            break;
        case F_SETLK:
        case F_SETLKW:
        case F_GETLK:
            {
                struct flock* arg = va_arg(va, struct flock*);
                va_end(va);
                return fcntl_f(fd, cmd, arg);
            }
            break;
        case F_GETOWN_EX:
        case F_SETOWN_EX:
            {
                struct f_owner_exlock* arg = va_arg(va, struct f_owner_exlock*);
                va_end(va);
                return fcntl_f(fd, cmd, arg);
            }
            break;
        default:
            va_end(va);
            return fcntl_f(fd, cmd);
    }
}

int ioctl(int d, unsigned long int request, ...) {
    va_list va;
    va_start(va, request);
    void* arg = va_arg(va, void*);
    va_end(va);

    if(FIONBIO == request) {
        bool user_nonblock = !!*(int*)arg;
        FdCtx::ptr ctx = FdMgr::GetInstance()->get(d);
        if(!ctx || ctx->isClose() || !ctx->isSocket()) {
            return ioctl_f(d, request, arg);
        }
        ctx->setUserNoBlock(user_nonblock);
    }
    return ioctl_f(d, request, arg);
}


int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen) 
{
    return getsockopt_f(sockfd, level, optname, optval, optlen);
}

int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen)
{
    if(!t_hook_enable)
    {
        return setsockopt_f(sockfd, level, optname, optval, optlen);
    }
    if(level == SOL_SOCKET)
    { 
        if(optname == SO_RECVTIMEO || optname == SO_SENDTIMEO)
        {
            FdCtx::ptr ctx = FdMgr::GetInstance()->get(sockfd);
            if(ctx)
            {
                const timeval *v = static_cast<const timeval *>(optval);
                ctx->setTimeout(optname, std::chrono::milliseconds(v->tv_sec * 1000 + v->tv_usec / 1000));
            }
        }
    }
    return setsockopt_f(sockfd, level, optname, optval, optlen);
}
} //  end of extern "C"