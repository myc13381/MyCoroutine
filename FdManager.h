// 文件句柄管理类
#pragma once
#include <memory>
#include <vector>
#include <chrono>
#include <shared_mutex>
#include <mutex>
#include "Singleton.h"

// 设置超时时间使用
constexpr int SO_RECVTIMEO = 20;
constexpr int SO_SENDTIMEO = 21;

// 文件句柄上下文类
// 管理文件句柄类型(是否是socket)，是否阻塞，是否关闭，读/写超时
class FdCtx : public std::enable_shared_from_this<FdCtx>
{
public:
    typedef std::shared_ptr<FdCtx> ptr;


    // 使用文件句柄构造FdCtx
    FdCtx(int fd);
    // 析构函数
    ~FdCtx() = default;

    // 是否初始化完成
    bool isInit() const {return m_isInit;}

    // 是不是socket
    bool isSocket() const {return m_isSocket;}

    // 是否关闭
    bool isClose() const {return m_isClosed;}

    // 用户是否主动设置了非阻塞
    void setUserNoBlock(bool flag) {m_userNoBlock = flag;}

    // 获取用户是否手动设置了非阻塞
    bool getUserNoBlock() const {return m_userNoBlock;}

    // 设置系统非阻塞
    void setSysNoBlock(bool flag) {m_sysNoBlock = flag;}

    // 获取系统非阻塞
    bool getSysNoBlock() const {return m_sysNoBlock;}

    // 设置超时时间
    void setTimeout(int type, std::chrono::milliseconds t);

    // 获取超时时间
    std::chrono::milliseconds getTimeout(int type) const;

protected:
    // 初始化
    bool init();

private:
    bool m_isInit       : 1;                    // 是否初始化
    bool m_isSocket     : 1;                    // 是否为socket
    bool m_sysNoBlock   : 1;                    // 是否hook非阻塞
    bool m_userNoBlock  : 1;                    // 是否用户设置非阻塞
    bool m_isClosed     : 1;                    // 是否关闭
    int m_fd;                                   // 文件句柄
    std::chrono::milliseconds m_recvTimeout;    // 读超时时间毫秒
    std::chrono::milliseconds m_sendTimeout;    // 写超时时间毫秒
};

// 文件句柄管理类
class FdManager
{
public:
    typedef std::shared_mutex RWMutex; // 读写锁
    typedef std::unique_lock<std::shared_mutex> WriteLock;
    typedef std::shared_lock<std::shared_mutex> ReadLock;
    // 无参构造函数
    FdManager();

    // 获取创建文件句柄类FdCtx::ptr
    FdCtx::ptr get(int fd, bool auto_create = false);

    // 删除文件句柄类
    void del(int fd);
private:
    RWMutex m_mutex;                    // 读写锁
    std::vector<FdCtx::ptr> m_datas;    // 文件句柄集合
};

// 文件句柄管理类单例对象
typedef Singleton<FdManager> FdMgr;