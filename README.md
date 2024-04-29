`enable_shared_from_this` 是什么
https://blog.csdn.net/breadheart/article/details/112451022

## 协程的设计 -- Fiber

协程是对进程的进一步划分，他完全工作在用户态，操作系统不知道他的存在也不会参与他的调度。为了实现一个线程中多个协程的运行，每个协程需要有自己的**栈**，也需要自己的**上下文**；此外，协程的存在是有其任务的，所以协程应该有一个工作函数，我们称之为**回调函数**(callback)；考虑到一个线程中会存在多个协程，有的协程在执行，有的协程在等待，有的协程则已经完成了自己的任务，因此协程需要有自己的**状态**；为了有序组织各个协程有序工作，需要有个**协程调度器**(Scheduler)，有的协程被调度有的协程则不被调度，需要加以区分；最后为了区分各个协程，每个协程需要有自己的编号

```cpp
private:
    uint64_t m_id = 0;          // 协程ID
    uint32_t m_stacksize = 0;   // 协程栈大小
    State m_state = READY;      // 协程状态
    ucontext_t m_ctx;           // 协程上下文
    void *m_stack;              // 协程栈地址
    std::function<void()> m_cb; // 协程函数入口
    bool m_runInScheduler;      // 本协程是否参与调度器调度
```

这里设计的协程是非对称协程，每个线程会有一个主协程和其他真正处理任务的子协程。协程不能嵌套调用，子协程只能和主协程进行切换，也就是说如果想从一个工作的子协程切换到另一个工作中的子协程中，必须通过主协程来协调中间状态。

为了方便子协程能够找到主协程，以及管理各个子协程，我们使用全局静态变量来记录**主协程**以及**当前正在工作的协程**。注意到进程也许会开启多线程，而协程有工作在线程之上，为了不让不同线程上协程互相影响，上述的全局静态变量需要使用线程局部存储技术。

```cpp
// 线程局部变量，代表当前线程正在运行的协程
static thread_local Fiber *t_fiber = nullptr;

// 线程局部变量，当前线程的主协程，切换到这个协程相当于切换到了主线程中运行，智能指针
static thread_local Fiber::ptr t_thread_fiber = nullptr;
```

为了实现协程的基本功能，首先需要实现协程的切换，协程的切换其实就是切换上下文和栈空间，这里我们使用的是`<ucontext.h>`头文件中的相关接口

[参考文章](https://developer.aliyun.com/article/52886)

在`<ucontext>`头文件中，提供了四个函数，可以实现用户级的上下文切换。

```cpp
typedef struct ucontext {
	struct ucontext *uc_link; 		// 下一个上下文，如果为NULL，执行完后线程退出
	sigset_t         uc_sigmask;	// 阻塞信号集合
	stack_t          uc_stack;		// 栈空间
	mcontext_t       uc_mcontext;	// 保存的上下文的特定机器表示，包括调用线程的特定寄存器等，对程序员透明
	// ... 其他成员
} ucontext_t;
```

```cpp
int getcontext(ucontext_t *ucp);
// 将当前运行的上下文保存在ucp中
// 执行成功返回0
```

```cpp
void makecontext(ucontext_t *ucp, void (*func)(), int argc, ...);
// 创建一个上下文
// 注意，该函数的第一个参数需要是从getcontext函数中获得上下文，内部对其修改
// 这意味着，makecontext函数需要和getcontext函数一起调用，如果有必要需要手动为其开辟栈空间
// 创建上下文之后可以使用setcontext或者swapcontext进行上下文切换
```

```cpp
int setcontext(const ucontext_t *ucp);
// 设置当前的上下文为ucp
// 如果调用成功则不会返回，直接执行新的上下文
// 调用失败则返回-1，并设置对应的erron
```

```cpp
int swapcontext(ucontext_t *oucp, ucontext_t *ucp);
// 保存当前上下文到oucp中，然后切换到上下文ucp中
// 如果调用成功则不会返回，直接执行新的上下文
// 调用失败则返回-1，并设置对应的erron
```

接下来需要实现协程的基本功能

```cpp
// 构造函数
Fiber(std::function<void()> cb, size_t stack_size = 0, bool run_in_scheduler = true);
// 第一个参数是回调函数也就是任务函数
```

```cpp
// 重置协程状态和入口函数，复用栈空间，不重新创建栈
void reset(std::function<void>()> cb);
// 该函数比较简单，核心操作就两步
// 一，使用getcontext和makecontext函数创建新的上下文
// 二，将协程状态设置为就绪(READY)
```

```cpp
// 将当前协程切换到执行状态
void resume();
// 首先判断当前协程的类型
// 一、当前协程是任务协程，也就是说他参与调度，同时也说明正在运行的协程是调度协程，那么交换调度协程的上下文与当前协程上下文即可
// 二、当前协程不是任务协程，也就是不参与调度，同时也说明正在运行的协程是线程主协程，那么交换主协程和当前协程即可
// 规律：只有线程主协程才会调用调度协程的resume方法，而只用调度协程才会去调用任务协程的resume方法。
```

```cpp
// 当前线程让出执行权
void yield();
// 具体操作其实和resume函数很像
// 一、当前协程是任务协程，即自己参与调度，那么将交换当前协程与调度协程的上下文即可
// 二、当前协程是调度协程，即自己不参与调度，那么交换当前协程与线程主协程即可
// 规律：协程自己内部才会调用yield方法，然后进入就被调度的状态
```

```cpp
// 如何使用协程
// 一、创建一个协程，对于有参函数可以使用std::bind进行参数绑定
// 二、只要协程没有正式开始执行，可以使用reset函数进行重设
// 三、调用协程的resume方法开始运行协程
// 四、调用协程yield方法临时终止，后续可以继续使用yield方法继续执行
```

## 协程调度器的设计 -- Scheduler

协程调度器的作用是用来消耗协程的，协程用来执行一个一个任务，因此调度器自然需要维护一个**任务队列**来保存这些任务，然后将他们分发给协程；协程虽然是对进程的细分，但是程序可以通过开启多线程来利用多核特性进一步提高协程的工作效率，因此我们可以使用**线程池**来管理这些线程，同时由于使用了多线程技术，所以需要考虑到任务队列的资源竞争问题因此需要一个**互斥锁**来保持任务队列操作的原子性。在设计协程时考虑到主线程(main函数所在线程)既可以只进行调度工作而让其他线程进行协程任务工作，同时也可以让主线程也参与到协程具体工作上来，这样主线程既需要处理协程调度，也需要处理协程任务。

综合考虑，协程调度器应该有如下成员变量：

```cpp
private:
    std::string m_name;                                 // 协程调度器名称
    MutexType m_mutex;                                  // 互斥锁
    std::vector<std::shared_ptr<std::thread>> m_threads;// 线程池
    std::list<ScheduleTask> m_tasks;                    // 任务队列
    std::vector<std::thread::id> m_threadIds;           // 记录工作线程的id
    size_t m_threadCount = 0;                           // 工作线程的数量，不包含 use_caller 的主线程
    std::atomic<size_t> m_activeThreadCount {0};        // 活跃的线程数量
    std::atomic<size_t> m_idleThreadCount {0};          // idle线程数量

    bool m_useCaller;                                   // 是否直接利用主线程进行协程工作
    Fiber::ptr m_rootFiber;                             // m_useCaller为true时，调度器所在线程的调度协程
    std::thread::id m_rootThread = std::thread::id(-1); // m_userCaller为true时，调度器所在线程的ID

    bool m_stopping = false;                            // 调度器是否正在停止
```

下面记录协程调度器相关方法的设计

如果不考虑调度器自己的行为，那么每一个线程如果想让自己其中的协程正常工作，那么其内部必须有两种协程：第一种是调度协程，用于不断将任务队中的任务取出来然后创建一个协程去执行以及作为两个工作协程之间切换的中间人身份(其实在这里二者是一致的，因为这里设计的协程一旦yield就认为执行结束了，如果确实没有执行完毕，也应该有程序员手动将未执行的协程重新加入任务队列中)；第二种是任务协程，执行具体的任务。

```cpp
// 构造函数
Scheduler(size_t threads = 1, bool use_caller = true, const std::string &name = "scheduler");
// threads 表示真正进行协程工作的线程数量，如果主线程也参与协程工作那么只需要额外启动threads-1个线程
// use_caller 表示主线程是否参与协程工作任务
// 如果use_caller==true，那么main函数将会存在三种协程，一种是主协程，主要是创建调度器，向任务队列加入任务的工作
// 第二种是主线程的调度协程，第三种是主线程的工作协程，有若干个
// 如果主线程参与执行协程工作，那么需要构造函数中会在主线程中创建主线程的调度协程，并将threads的值减1
```

```cpp
// 添加调度任务
template <typename FiberOrcb>
void schedule(FiberOrcb fc, std::thread::id thread = std::thread::id(-1));
// fc 是一个可调用的对象，可以是协程也可以是函数
// thread 是期望该协程工作在哪一个线程中
// 其内部工作流程比较简单
// 首先将互斥锁上锁，然后任务加入到任务队列中即可
```

```cpp
// 协程调度函数
void run();
// 每个线程的调度协程会执行该函数
// 该函数开始部分，首先设置好线程局部变量t_scheduler_fiber，该变量指向了每个线程的调度协程
// 接下来是一个死循环，每次循环尝试从任务队列中取出一个任务，如果该任务指定了线程且不是当前线程，那么继续查找下一个任务
// 如果该任务是一个协程，那么执行，如果是一个函数，那么将他封装成协程然后执行
// 如果任务队列已经没有任务可取了，那么会转而执行idle协程用于等待任务 --> 这里idle协程的实现是什么也不做
```

```cpp
// 空闲调度函数
virtual void idle();
// 无任务执行时线程会执行idle协程，idle协程内部执行该函数
// 在此调度器中idle什么事也不做，直接执行yield
// 该函数是虚函数，此调度器的子类可以重写该方法
```

```cpp
// 通知协程调度器有任务
virtual void tickle();
// 该函数什么也没做
```

```cpp
// 启动调度器
void start();
// 前面提到过我们可以使用多线程来同时执行更多的协程，因此启动调度器的作用就是创建这些线程
// 线程的入口函数Scheduler::run，其实也就是创建了每个线程的调度协程
// 如果确实开启了多线程，那么执行了start函数之后就会直接开始协程调度工作
```

```cpp
// 停止调度器
void stop();
// 停止调度器意味着将不再允许继续添加任务了，但是未执行的任务还是会继续执行
// 前面提到过主线程也可以参与协程任务执行，而主线程为了能够保证调度器的正常工作，只有在停止调度器后才会执行协程任务
// 不难推断出，如果不开启多线程，那么只有在停止协程调度器之后才会开始真正的协程调度
```

几点说明：

* 当前版本的idle函数也就是空闲等待协程啥也没做，最后导致的结果就是线程不断轮询直到找到一个可执行任务，这样CPU利用率不高
* 协程如何处理异常？正如线程处理方式一样，协程不会处理任何异常，所有所有异常应该由程序员自己处理
* 未处理完成的协程自己yield了之后如何继续处理？协程调度器不会自动将未完成但是提前yield的协程自动加入调度队列中，如果需要的话，需要程序员手动完成。这里思想和上面一条一样，一个成熟的协程应该学会自己管理。
* 协程调度的策略：先来先服务



## 定时器 -- timer

### 时间堆

直接将超时时间当作tick周期，具体操作是每次都取出所有定时器中超时时间最小的超时值作为一个tick，这样，一旦tick触发，超时时间最小的定时器必然到期。处理完已超时的定时器后，再从剩余的定时器中找出超时时间最小的一个，并将这个最小时间作为下一个tick，如此反复，就可以实现较为精确的定时。

使用小根堆实现定时器，所有定时事件根据超时的绝对时间进行排序。每次取出离当前时间最近的一个超时时间点，计算出超时需要等待的时间，然后等待超时。达到超时时间点后（实际时间也许会有所推迟），把小根堆中所有超时的时间事件都收集出来，执行他们的回调函数。

### 定时器的设计

为了实现定时器的功能，我们需要设计两个类。

一个是时间事件本身，我们称之为Timer，也就是**定时器**。一个定时器，最基本的就是什么时候执行，这里我们使用两个变量来表现这个属性，分别是定时器**多久之后执行**(相对时间)和执行的**具体时间**(绝对时间)。同时定时器是需要执行一定任务的，所以需要一个**回调函数**。有的时间事件是会重复执行的，因此需要一个标志用来记录。最后定时器本身被某个定时器管理器管理，因此它会有一个指向定时器管理器的指针。

```cpp
private:
    bool m_recurring = false;                                           // 是否循环
    std::chrono::milliseconds m_ms = std::chrono::milliseconds(0);      // 多久之后执行，相对于创建定时器时间戳的相对时间
    std::chrono::milliseconds m_next = std::chrono::milliseconds(0);    // 精确的执行绝对时间 == 创建时间戳 + m_ms      
    std::function<void()> m_cb;                                         // 回调函数
    TimerManager *m_manager = nullptr;                                  // 定时器管理器
```

定时器本身的方法很简单，只有构造定时器，取消定时器，刷新定时器，重置定时器。同时还需要自定义比较函数

```cpp
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
    // 定时器比较仿函数，按执行时间排序
    struct Comparator
    {
        bool operator()(const Timer::ptr &lhs, const Timer::ptr &rhs) const;
    };
```



第二设计的对象是**定时器管理器**，顾名思义，就是用来管理定时器的。既然是管理定时器，必然需要拥有定时器，这里使用`std::set`最为存**储定时器的数据结构**，其内部是有序存储的。前面提到定时器本身也有指向对应定时器管理器的指针，因此在多线程的情况下，定时器管理器自身其实一个临界资源，所以其需要一个**锁**来实现资源的互斥访问，为了实现性能的提升，这里使用了`std::shared_mutex`来模仿读写锁的行为。有时候我们插入的定时器的过期时间可能早于当前定时器管理器中的所有其他定时器，对于这种行为，我们需要考虑是否做出回应，因此需要有个变量记录一下。

```cpp
// 使用 std::shared_mutex 来模拟读写锁
typedef std::shared_mutex RWMutexType; // 读写锁
typedef std::unique_lock<std::shared_mutex> WriteLock;
typedef std::shared_lock<std::shared_mutex> ReadLock;

private:
    // Mutex
    RWMutexType m_mutex;                                                        // 读写锁
    std::set<Timer::ptr, Timer::Comparator> m_timers;                           // 定时器集合，内部保存定时器的智能指针
    bool m_tickled = false;                                                     // 是否触发 onTimerInsertedAtFront
    std::chrono::milliseconds m_previouseTime = std::chrono::milliseconds(0);   // 上次执行时间
```

现在考虑一个定时器应该提供什么样的操作

```cpp
public:
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
    // 检测服务器时间是否被调后了，该函数实现比较粗糙
    bool detectClockRollover(std::chrono::milliseconds now_ms);
```

相对比较复杂的函数是`listExpiredCb`。该函数的作用是返回所有已经超时的定时器的回调函数，同时如果超时定时器是循环执行，那么还会将他重写插入到定时器管理器中。内部主要是对`set`容器操作。

## 协程 + IO

### 概述

使用协程封装`epoll`多路复用，简化编程。IO协程调度支持协程调度的所有功能，因为他**继承于协程调度**。此外，IO协程调度还增加了IO事件调度的功能，这个功能是针对描述符的（一般是套接字描述符）。IO协程调度支持为描述符注册可读可写事件的回调函数，当描述符可读可写时，执行对应的回调函数。（这里可以直接把回调函数等效成协程，所以这个功能被称为IO协程调度）。

IO协程调度模块基于epoll实现，只支持Linux平台。对每个文件描符，支持读事件和写事件对应`EPOLLIN`和`EPOLLOUT`，我们事件的类型枚举值直接继承epoll。

对于IO协程调度来说，每次调度都包含一个三元组信息，分别是{**描述符--事件类型--回调函数**}，描述符和事件类型用于`epoll_wait`，回调函数用于协程调度。这个三元组对应源码上的`FdContext`结构体，通过`epoll_event.data.ptr`来保存。

IO协程调度还支持定时器模块功能，用于处理定时任务

### 具体实现

IO协程调度功能被封装在类`IOManager`中。

首先是事件类型，这里直接继承了epoll的事件类型：

```cpp
enum Event{
	NONE = 0x0, // 无事件 
	READ = 0x1, // 读事件 EPOLLIN
	WRITE = 0x4 // 写事件 EPOLLOUT
};
```

接下来考虑描述符事件上下文`FdContext`如何设计。前面提到过`FdContext`内部保存**描述符--事件类型--回调函数**三元组，因此需要一个事件类型变量，一个文件描述符变量，同时一个fd可能同时会有读写两种事件，因此至少需要两个回调函数。有些情况下，同一个fd的读写事件可能会同时执行，对应事件执行时均可能会改变FdContext的事件类型状态，因此为了防止并发导致的数据竞争，每个FdContext还需要一把互斥锁来保证自身状态修改的原子性。最后考虑回调函数，由于我们使用的是协程，所以回调函数可以有两种类型，一种就是函数对象，另一种就是协程本身，当然最后殊途同归都会被封装成为协程，同时为了管理协程，我们还需要个调度器的指针，因此回调函数部分可以单独封装成一个结构体。

```cpp
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
```

触发一个事件

```cpp
void triggerEvent(Event event);
// 这里的设计思路是注册的IO事件是一次性的，事件触发之后就会被删除，如果想持续关注某个fd的读写事件，那么需要在触发之后重新添加
// 重新添加的操作可以考虑实现在回调函数中
```



接下来考虑`IOManager`怎么设计。

首先IO协程是基于`epoll`设计的，自然需要一个**epoll文件句柄**；有些情况下我们需要知道当前还有多少个IO事件执行以便进一步处理(例如结束IO协程调度的时候)，因此我们需要一个整型变量来记录这个值，为了避免资源竞争可以使用原子变量；为了能够调用各个文件描述符的相关事件，需要一个**容器**来保存这些`FdContext`。`IOManager`不仅支持IO事件的调用，还需要提供定时器的支持。epoll_wait会阻塞，这里通过管道来跳出epoll_wait的阻塞状态，因此需要一个**管道**；多线程的情况下，保存FdContext的容器将成为一个临界资源，因此操作该资源时需要上锁，为了实现更好读写访问性能，这里使用了`std::shared_mutex`模拟读写锁的相关功能。

```cpp
private:
    int m_epfd = 0;                                 // epoll 文件句柄
    int m_tickleFds[2];                             // pipe文件句柄，fd[0]读端口，fd[1]写端口，
													// 用于在定时器触发时及时退出epoll_wait
    std::atomic<size_t> m_pendingEventCount {0};    // 当前等待执行的IO事件数量
    RWMutexType m_mutex;                            // IOManager的mutex
    std::vector<FdContext *> m_fdContexts;          // socket事件上下文容器
```

加下来思考一下`IOManager`应该提供哪些方法

首当其冲的必然是构造函数

```cpp
// 构造函数
IOManager(size_t threads = 1, bool use_caller = true, const std::string &name = "IOManager");
// IOManager继承于Scheduler和TimerManager，TimerManager只有默认构造函数，而IOManager构造时不依赖额外参数
// 因此此构造函数的参数全部用来构造Scheduler
// IOManager构造工作主要是：
// 一、初始化epoll资源获取epoll文件描述符
// 二、创建管道，将管道读端口加入epoll监听队列中，并设置管道为非阻塞，对应读端口为ET边缘触发
// 三、预设置FdContexts容器长度，默认为32
// 四、调用Scheduler::start()方法，启动调度器，这样如果采用多线程，就开始可以直接执行任务了
```

然后是析构函数

```cpp
// 析构函数
~IOManager();
// 析构函数的工作就比较简单了
// 一、调用Scheduler::stop()方法来结束协程调度，该函数会阻塞直到所有协程执行完毕
//    而且当我们只开启一个线程时，调用stop方法后才会正式开始协程调度
// 二、释放文件句柄，包括epoll文件句柄，管道资源
// 三、释放所有FdContext
```

添加事件

```cpp
// 添加事件
int addEvent(int fd, Event event, std::function<void()> cb = nullptr);
// 前面说到一个文件事件就是一个三元组，对应添加事件的三个参数
// 添加的事件都保存在IOManager::m_fdContexts中，这是一个vector容器，索引对应着fd，
// 因此增删改查事件时必须先判断容器中是否存在这个元素
// 一、添加事件时，如果fd超过容器长度，则需要容器进行扩容，这也是处理fd的基本操作了
// 二、使用epoll_ctl将新的事件加入epoll_wait队列中，使用epoll_event的私有指针存储FdContext的位置
// 三、更新待执行IO事件数量，加一
// 四、将对应事件的事件上下文相关数据进行赋值，包括回调函数，协程调度器指针
```

删除事件

```cpp
// 删除事件
bool delEvent(int fd, Event event);
// 删除事件只需要对应的fd和事件类型即可，本质上是从epoll等待队列中移除该事件
// 一、首先根据传入的fd找到对应的FdContext，如果不存在直接返回
// 二、判断删除的事件类型是否存在，不存在直接返回
// 三、使用epoll_ctl删除指定类型事件
// 四、更新待执行IO事件数量，减一
// 五、重置fd对应事件类型的上下文，释放一些资源
```

取消事件

```cpp
// 取消事件
bool cancelEvent(int fd, Event event);
// 将事件从epoll等待队列中移除，但是不重置事件上下文，而且取消该事件时自动调用一次
// 一、首先根据传入的fd找到对应的FdContext，如果不存在直接返回
// 二、判断取消的事件类型是否存在，不存在直接返回
// 三、使用epoll_ctl删除指定类型事件
// 四、更新待执行IO事件数量，减一
// 五、主动调用该事件，即调用IOManager::FdContext::triggerEvent方法
```

取消全部事件

```cpp
// 取消所有事件
bool cancalAll(int fd);
// 取消对应fd中的所有事件，操作和cancelEvent很像，取消的事件都会被手动执行一次
```

获取自身指针

```cpp
// 返回当前的IOManager
IOManager *IOManager::GetThis()
{ // 动态类型检查
    return dynamic_cast<IOManager *>(Scheduler::GetThis());
}
// 实际上只有在协程调度才会用到，因此这里采用动态类型转换
```

通知有任务调度

```cpp
// 通知调度器有任务要调度
void tickle() override;
// 主要是让调度协程从idle函数中退出
// 主要使用在定时器管理器在最前方插入新定时器时需要退出epoll_wait从而防止超时，同时调用Scheduler::schedule也会触发该函数
// 一、首先检查是否有IO调度任务，其实也就是判断待执行IO事件数量，如果有事件待执行，那么直接退出，因为此时idle也不会执行
// 二、如果此时确实在idle中，那么想管道中写入数据，这样idle中的epoll_wait就会收到信息进而直接返回，然后idle就会结束执行
```

判断是否能够停止IO协程调度

```cpp
// 判断条件是Scheduler::stopping()外加IOManager的m_pendingEventCount为0，表示没有IO事件可调度了
bool stopping() override;
// 判断是否可以停止，同时获取最近一个定时器的超时时间
bool stopping(std::chrono::milliseconds &timeout);
// 判断的条件是timeout之后没有定时器了，并且此时没有待执行IO事件，同时调度器也可以停止了
```

idle协程，空闲调度

```cpp
void idle() override;
// 每一个线程的调度协程在无任务时会转而阻塞在idle协程上，idle阻塞的来源是epoll_wait函数
// idle主要关心两类事件
// 一种是调度器主动添加事件Scheduler::schedule方法，或者定时器调度器在最前方插入了一个定时器，这些事件回通过tickle来让其打断idle的阻塞过程
// 第二种是当前epoll关注的事件是否有触发，如果触发则调用相应的回调函数
// idle协程可能会反复执行，因此内部是一个死循环
// 一、首先判断IOManager是否停止了，如果停止那么idle协程也可以直接结束了
// 二、获取最近一次定时器超时的时间间隔，并和默认的5秒等待时间进行比较，取最小值
//		epoll_wait每次默认等待5秒，但是如果有定时器在这5秒中触发，则需要提前退出
// 三、epoll_wait退出后我们尝试获取超时定时器执行列表，并将他们仿佛协程调度中
// 四、接下才会处理epoll_wait返回的文件描述符对应的事件
//		对于触发的文件描述符，通过对应epoll_event.data.ptr即可获取对应的EventContext。
//		如果epoll返回错误，那么将执行对应fd的所有事件(如果存在)
//		执行的事件将会从epoll关注队列中删除，将触发的事件对应回调函数加入协程调度中
// 五、处理完事件后，idle退出，因为他也只是将协程添加到调度中去，所以自己yield后去执行调度的协程的任务
```

定时器重新设置最新超时时间

```cpp
void onTimerInsertedAtFront() override;
// 定时器管理器中插入最近时间的定时器后需要及时停止idle的阻塞，防止定时任务不及时执行，内部就是调用了tickle函数
```

需要注意的时，在**idle**中，如果一个epoll关注的事件被执行了，那么它会将该事件从关注列表中移除，因此在执行完回调函数后，需要将该事件重新注册到epoll关注队列中。



## Hook

### 什么是hook，为什么需要hook

hook是对系统调用API进行一次封装，将其封装成为一个与原始系统调用同名的接口。这样调用接口时会先执行一些隐藏的操作，再执行原始的系统调用。

协程的优势在于发生阻塞时可以随时切换，提高处理机资源的利用率，而且协程间切换。因此为了能够使体现的协程库真正体现价值，就需要让其能够实现这一阻塞切换的功能，其中hook就是一种解决方案。

### 如何进行hook

在进行网络编程时，常见的阻塞场景是socket IO相关接口例如read，write等，还有就是用户主动调用sleep相关接口进行睡眠。

**基本思路**：针对socket IO，为了实现非阻塞的使用，需要将socket fd设置为非阻塞，通过对应返回值已经errno确定调用失败的原因，如果是阻塞，则考虑将该协程yield，等到资源可用时再返回继续执行刚刚yield的协程。针对sleep的hook也差不多，不过主要依靠定时器来实现，当某一协程sleep时，设置一个定时器用来在sleep醒来的时机触发，然后协程yield。

**socket IO**

hook的重点是在API底层实现的同时完全模拟其原本的行为，而不让调用方知道，因此处理socket IO相关函数，对于fcntl，iocntl，setsockopt,getsockopt函数也需要hook。

**FdCtx**和**FdManager**

为了方便管理socked fd，即管理fd阻塞超时状态等，我们设计FdCtx类用于记录

```cpp
private:
    bool m_isInit       : 1;                    // 是否初始化
    bool m_isSocket     : 1;                    // 是否为socket
    bool m_sysNoBlock   : 1;                    // 是否hook非阻塞
    bool m_userNoBlock  : 1;                    // 是否用户设置非阻塞
    bool m_isClosed     : 1;                    // 是否关闭
    int m_fd;                                   // 文件句柄
    std::chrono::milliseconds m_recvTimeout;    // 读超时时间毫秒
    std::chrono::milliseconds m_sendTimeout;    // 写超时时间毫秒
```

同时一系列接口用于获取和设置这些成员变量的值。

每一个fd会有自己的FdCtx，但是为了管理FdCtx，需要一个管理类，这就是FdManager，其设计比较简单，主要就是通过一个数组维护FdCtx的指针，同时FdManager被设计为单例对象。

```cpp
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
```

### hook的具体实现

使用线程局部变量用来控制每一个线程是否启用hook，将hook控制的粒度缩小到线程级别。

```cpp
static thread_local bool t_hook_enable = false; // 每一个线程是否开启hook
```

sleep

由于协程工作在线程之上，如果一个协程调用sleep，将导致整个线程睡眠进而无法触发其他协程，这样显然是不合理的。因此我们需要实现对一个协程的sleep功能。这里可以使用定时器，当我们调用sleep时，设置一个睡眠结束时间超时的定时器然后协程yield，这样就可以达到睡眠的效果。并且使用定时器也不会阻塞线程。

write/read/send/recv ....

这些是socket IO函数。hook这些接口的思路是首先判断fd是不是非阻塞，的如果用户自己设置了非阻塞，那么直接执行原来的系统调用即可(因为用户自己设置非阻塞代表用户自己编写了相关代码来处理非阻塞情况)。如果用户没有设置非阻塞，并且该线程也确实开起了hook功能，那么将执行hook操作。对于这些IO接口，其底层处理相似，因此我们实现`do_io`函数来复用这些处理。

```cpp
template <typename OriginFun, typename... Args>
static ssize_t do_io(int fd, OriginFun fun, const char *hook_fun_name, uint32_t event, int timeout_so, Args &&... args);
// 一、不启用hook或者FdManager获取对应FdCtx失败，直接执行原来的系统函数，返回
// 二、此时fd已经被FdManager设置为非阻塞了，因此进行非阻塞的系统调用，调用成功直接返回
// 三、系统调用返回失败并且errno == EAGAIN，表示资源暂时不可用，这种情况需要过段时间重试，其他情况直接返回错误即可
// 四、接下来判断是否传入了有效的超时时间，如果有效，则设置一个条件超时定时器，内部表示超时时间时触发一次event事件
// 五、添加event类型事件，回调函数是当前协程
// 六、yield 让出执行权
// 七、再次被调度，这里有两种情况一是超时触发，而是epoll检测可读/写，如果是因为超时触发，则函数直接返回-1，如果是可读/写触发，则回到二执行
```



使用extern "C" 和 dlsym 进行hook

```cpp
name_f = (name_fun)dlsym(RTLD_NEXT, #name);
```

将hooking系统函数name，获取其地址并赋值给对应函数类型指针，然后重新实现同名的系统函数，内部添加一些操作后再调用底层原来的函数，最后返回。





参考资料

协程的好处有哪些？ - 腾讯技术工程的回答 - 知乎
https://www.zhihu.com/question/20511233/answer/2743607300 

[ucontext_t库原理](https://blog.csdn.net/qq_44443986/article/details/117739157)

[epoll底层](https://mp.weixin.qq.com/s/OmRdUgO1guMX76EdZn11UQ)
