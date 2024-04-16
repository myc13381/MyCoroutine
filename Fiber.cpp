#include <mutex>
#include "Fiber.h"
#include "Scheduler.h"

// 全局静态变量，用于生成协程ID
static std::atomic<uint64_t> s_fiber_id{0};

// 全局静态变量，用于统计当前的协程数量
static std::atomic<uint64_t> s_fiber_count{0};

// 线程局部变量，代表当前线程正在运行的协程
static thread_local Fiber *t_fiber = nullptr;

// 线程局部变量，当前线程的主协程，切换到这个协程相当于切换到了主线程中运行，智能指针
static thread_local Fiber::ptr t_thread_fiber = nullptr;

// 协程栈大小，默认128KB
static uint32_t g_fiber_stack_size = 128 * 1024;


// malloc栈内存分配器
class MallocStackAllocator
{
public:
    static void *Alloc(size_t size) { return malloc(size); }
    static void Dealloc(void *vp, size_t size) { return free(vp); }
};
// 重命名
using StackAllocator =  MallocStackAllocator;

uint64_t Fiber::GetFiberId()
{
    if(t_fiber != nullptr)
    {
        return t_fiber->getID();
    }
    return static_cast<uint64_t>(0);
}

Fiber::Fiber()
{
    SetThis(this);
    m_state = RUNNING; // 设置状态为正在运行

    if(getcontext(&m_ctx))
    {
        // 程序异常，需要停止
        error_handling("getcontext error");
    }

    ++s_fiber_count;
    m_id = s_fiber_id++;

}

void Fiber::SetThis(Fiber *f)
{
    t_fiber = f;
}

// 获取当前协程，同时充当初始化当前线程主协程的作用,这个函数在使用协程之前要调用一下
Fiber::ptr Fiber::GetThis()
{
    if(t_fiber != nullptr)
    {
        // 返回智能指针
        return t_fiber->shared_from_this();
    }

    // 如果 t_fiber 不存在，则创建
    Fiber::ptr main_fiber(new Fiber);
    MYASSERT(t_fiber == main_fiber.get(), "t_fiber != main_fiber.get()");
    t_thread_fiber = main_fiber;
    return t_fiber->shared_from_this();
}

// 有参构造函数用于创建其他协程，需要分配栈
Fiber::Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler)
    : m_id(s_fiber_id++), m_cb(cb), m_runInScheduler(run_in_scheduler)
{
    ++s_fiber_count;
    m_stacksize = stacksize ? stacksize : g_fiber_stack_size;
    m_stack = StackAllocator::Alloc(m_stacksize); // 分配栈空间

    if (getcontext(&m_ctx)) 
    {
        MYASSERT(false, "getcontext");
    }

    // 设置上下文的栈
    m_ctx.uc_link = nullptr;
    m_ctx.uc_stack.ss_sp = m_stack;
    m_ctx.uc_stack.ss_size = m_stacksize;

    // 创建上下文
    makecontext(&m_ctx, &Fiber::MainFunc, 0);
}

// 析构函数，因为主协程没有分配栈和cb，析构时需要特殊处理
Fiber::~Fiber()
{
    --s_fiber_count;
    if(m_stack != nullptr)
    { // 有栈，说明不是主协程
        MYASSERT(m_state == TERM, "m_state != TERM");
        StackAllocator::Dealloc(m_stack, m_stacksize);
    }
    else
    { // 没有栈，说明时主协程
        MYASSERT(!m_cb, "main fiber do not hava callback");
        MYASSERT(m_state == RUNNING, "main fiber's state must be RUNNING");

        // 当前协程就是自己？？？
        Fiber *cur = t_fiber;
        if(cur == this) SetThis(nullptr);
    }
}

// 为了简化状态管理，强制只有TERM状态的协程才可以重置
// 其实刚创建好并且还未执行的协程也应该允许重置的
void Fiber::reset(std::function<void()> cb)
{
    MYASSERT(m_stack != nullptr, "main fiber cannot reset");
    MYASSERT(m_state == TERM, "reset error, m_state != TERM");
    m_cb = cb; // 设置回调函数
    if(getcontext(&m_ctx))
    {
        MYASSERT(false, "getcontext");
    }

    // 重置栈空间
    m_ctx.uc_link = nullptr;
    m_ctx.uc_stack.ss_sp = m_stack;
    m_ctx.uc_stack.ss_size = m_stacksize;

    // 创建上下文
    makecontext(&m_ctx, &Fiber::MainFunc, 0);
    m_state = READY; // 重置后为就绪状态
}

// 将当前协程切换到执行状态
void Fiber::resume()
{
    MYASSERT(m_state != TERM && m_state != RUNNING, "resume error");
    SetThis(this);
    m_state = RUNNING;

    // 如果协程参与调度器调度，那么应该和调度器的主协程进行swap，而不是线程主协程
    if(m_runInScheduler)
    {
        if(swapcontext(&(Scheduler::GetScheduleFiber()->m_ctx), &m_ctx))
        {
            MYASSERT(false, "swapcontext");
        }
    }
    else
    {
        if(swapcontext(&(t_thread_fiber->m_ctx), &m_ctx))
        {
            MYASSERT(false, "swapcontext");
        }
    }
}

// 协程让出执行权
// 协程运行完之后会自动yield一次，用于回到主协程
void Fiber::yield()
{
    MYASSERT(m_state == RUNNING || m_state == TERM, "yield error");
    SetThis(t_thread_fiber.get()); // 设置当前运行协程为主协程
    if(m_state != TERM) m_state = READY;

    // 如果协程参与调度器调度，那么应该和调度器的调度协程进行上下文切换，而非和线程主协程切换
    if(m_runInScheduler)
    {
        if(swapcontext(&m_ctx, &(Scheduler::GetScheduleFiber()->m_ctx)))
        {
            MYASSERT(false, "swapcontext");
        }
    }
    else 
    {
        if(swapcontext(&m_ctx, &(t_thread_fiber->m_ctx)))
        {
            MYASSERT(false, "swapcontext");
        }
    }
}

// 协程函数入口
void Fiber::MainFunc()
{
    Fiber::ptr cur = GetThis(); // GetThis()的shared_from_this()方法让引用计数加1
    assert(cur != nullptr);

    cur->m_cb(); // 调用真正要执行的任务
    cur->m_cb = nullptr;
    cur->m_state = TERM;

    auto raw_ptr = cur.get();
    cur.reset();
    raw_ptr->yield(); // 结束之后自动释放处理机资源
}