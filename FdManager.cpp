#include <sys/stat.h> // 获取文件描述符状态
#include "Hook.h"
#include "FdManager.h"

FdCtx::FdCtx(int fd) : 
    m_isInit(false),
    m_isSocket(false),
    m_sysNoBlock(false),
    m_userNoBlock(false),
    m_isClosed(false),
    m_fd(fd),
    m_recvTimeout(std::chrono::milliseconds(-1)),
    m_sendTimeout(std::chrono::milliseconds(-1))
{   
    init(); 
}

bool FdCtx::init()
{
    if(m_isInit == true) return true;
    m_recvTimeout = std::chrono::milliseconds(-1);
    m_sendTimeout = std::chrono::milliseconds(-1);
    
    // fstat函数来获取一个文件描述符m_fd的状态，并根据这个状态来设置两个布尔变量m_isInit和m_isSocket的值。
    struct stat fd_stat;
    if(fstat(m_fd, &fd_stat) == -1)
    {
        m_isInit = false;
        m_isSocket = false;
    }
    else 
    {
        m_isInit = true;
        m_isSocket = S_ISSOCK(fd_stat.st_mode); // 判断是不是socket
    }

    if(m_isSocket)
    {  // 对于socket描述符
        int flags = fcntl_f(m_fd, F_GETFL, 0);
        if(!(flags & O_NONBLOCK))
        {   // 内部设置为非阻塞
            fcntl_f(m_fd, F_SETFL, flags | O_NONBLOCK);
        }
        m_sysNoBlock = true;
    }
    else 
    {
        m_sysNoBlock = false;
    }

    m_userNoBlock = false; // 初始化时认为用户没有设置NoBlock
    m_isClosed = false;
    return m_isInit;
}

void FdCtx::setTimeout(int type, std::chrono::milliseconds t)
{
    if(type == SO_RECVTIMEO)
    {
        m_recvTimeout = t;
    }
    else // SO_SENDTIMEO
    {
        m_sendTimeout = t;
    }
}

std::chrono::milliseconds FdCtx::getTimeout(int type) const
{
    if(type == SO_RECVTIMEO)
    {
        return m_recvTimeout;
    }
    else // SO_SENDTIMEO
    {
        return m_sendTimeout;
    }
}

FdManager::FdManager()
{
    m_datas.resize(64);
}

FdCtx::ptr FdManager::get(int fd, bool auto_create)
{
    if(fd == -1) return nullptr;
    ReadLock lk_R(m_mutex);
    if(static_cast<int>(m_datas.size()) <= fd)
    {
        if(auto_create == false)
        {
            return nullptr;
        }
        else
        {
            if(m_datas[fd] || !auto_create)
            { // 已经存在，直接返回
                return m_datas[fd];
            }
        }
    }
    lk_R.unlock();

    // 到此为止m_datas[fd]要么为空，要么就不存在，因此需要自己创建
    WriteLock lk_W(m_mutex);
    FdCtx::ptr ctx(new FdCtx(fd));
    if(fd >= static_cast<int>(m_datas.size()))
    { // 如果有必要，直接扩容
        m_datas.resize(fd * 1.5);
    }
    m_datas[fd] = ctx;
    return ctx;
}

void FdManager::del(int fd)
{
    WriteLock lock(m_mutex);
    if(static_cast<int>(m_datas.size()) <= fd)
    {
        return;
    }
    m_datas[fd].reset();
}

