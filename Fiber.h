#pragma once
#include <iostream>
#include <atomic>
#include <functional>
#include <memory>
#include <cassert>
#include <ucontext.h>

inline void error_handling(std::string &&expression)
{
    std::cerr<<expression<<std::endl;
    assert(false);
}


inline void error_handling(std::string &expression)
{
    std::cerr<<expression<<std::endl;
    assert(false);
}


inline void MYASSERT(bool flag, std::string &expression)
{
    if(flag == false)
    {
        std::cerr<<expression<<std::endl;
        assert(false);
    }
}

inline void MYASSERT(bool flag, std::string &&expression)
{
    if(flag == false)
    {
        std::cerr<<expression<<std::endl;
        assert(false);
    }
}

class Fiber : public std::enable_shared_from_this<Fiber>
{
public:
    typedef std::shared_ptr<Fiber> ptr;

    // 协程状态定义 经过简化，只设置三种状态
    enum State
    {
        READY = 0, // 就绪状态，刚刚创建或者 yield 之后的状态
        RUNNING, // 运行态，resume 之后的状态
        TERM // 结束态，协程的回调函数执行完之后为 TERM 状态
    };

private:
    // 构造函数设置为私有，不允许默认构造
    Fiber();

public:
    // 构造函数，用于创建用户线程
    Fiber(std::function<void()> cb, size_t stack_size = 0, bool run_in_scheduler = true);

    // 析构函数
    ~Fiber();

    // 重置协程状态和入口函数，复用栈空间，不重新创建栈
    void reset(std::function<void()> cb);

    // 将当前协程切换到执行状态
    void resume();

    // 当前线程让出执行权
    void yield();

    // 获取协程ID
    uint64_t getID() const { return this->m_id; }

    // 获取协程状态
    State getState() const { return this->m_state; }

public:
    // 设置当前正在运行的协程，也就是设置线程局部变量 t_fiber 的值
    static void SetThis(Fiber *f);

    // 返回当前正在执行的协程
    static Fiber::ptr GetThis();

    // 获取协程总数
    // static uint64_t TotalFibers();

    // 协程入口函数
    static void MainFunc();

    // 获取当前协程id
    static uint64_t GetFiberId();

private:
    
    uint64_t m_id = 0;          // 协程ID
    uint32_t m_stacksize = 0;   // 协程栈大小
    State m_state = READY;      // 协程状态
    ucontext_t m_ctx;           // 协程上下文
    void *m_stack;              // 协程栈地址
    std::function<void()> m_cb; // 协程函数入口
    bool m_runInScheduler;      // 本协程是否参与调度器调度
};

