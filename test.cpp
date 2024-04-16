#include <ucontext.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <fcntl.h>      // fcntl()
#include <thread>
#include "Scheduler.h"
#include "Timer.h"
#include "IOManager.h"
#include "Hook.h"

using namespace std;


void func1(void * arg)
{
    puts("1");
    puts("11");
    puts("111");
    puts("1111");

}

// =======================test fiber/scheduler=========================
void context_test()
{
    char stack[1024*128];
    ucontext_t child,main;

    getcontext(&child); //获取当前上下文
    child.uc_stack.ss_sp = stack;//指定栈空间
    child.uc_stack.ss_size = sizeof(stack);//指定栈空间大小
    child.uc_stack.ss_flags = 0;
    child.uc_link = NULL;//设置后继上下文

    makecontext(&child,(void (*)(void))func1,0);//修改上下文指向func1函数

    swapcontext(&main,&child);//切换到child上下文，保存当前上下文到main
    puts("main");//如果设置了后继上下文，func1函数指向完后会返回此处
}

void test_fiber1();
void test_fiber2();
void test_fiber3();
void test_fiber4();
void test_fiber5();


void testScheduler()
{
    Scheduler sc(3,false);
    sc.schedule(test_fiber1);
    sc.schedule(test_fiber2);

    Fiber::ptr fiber(new Fiber(&test_fiber3));
    sc.schedule(fiber);
    
    sc.start();

    sc.schedule(test_fiber4);

    // 在stop之前都可以一直添加任务
    sc.stop();
}

// ================test timer/IOManager=======================
// !!! 测试该函数时请将 void IOManager::idle() 中epoll_wait 的死循环中注释掉
void testTimer()
{
    static int timeout = 100;
    static Timer::ptr s_timer;

    auto timer_callBack = [&]()
    {
        std::cout<<"timer callback, timeout = "<<timeout<<std::endl;
        timeout += 100;
        if(timeout < 500) s_timer->reset(std::chrono::milliseconds(timeout), true);
        else s_timer->cancel();
    };

    IOManager iom(3); // 参数为启用的线程数目
    // 循环定时器
    s_timer = iom.addTimer(std::chrono::milliseconds(100), timer_callBack, true);
    // 单次定时器
    iom.addTimer(std::chrono::milliseconds(50), []{std::cout<<"50ms timeout"<<std::endl;});
    iom.addTimer(std::chrono::milliseconds(2000), []{std::cout<<"2000ms timeout"<<std::endl;});
}

int sockfd;

void watch_io_read();
// 写事件回调，只执行一次，用于判断非阻塞套接字connect成功
void do_io_write();
// 读事件回调，每次读取之后如果套接字未关闭，需要重新添加
void do_io_read();
void test_io();

void testIOManager()
{
    IOManager iom;
    iom.schedule(test_io);
}

// ==============hook===============

void test_sleep()
{
    IOManager iom;

    iom.schedule([](){
        int t = 2000;
        //std::this_thread::sleep_for(std::chrono::milliseconds(t));
        sleep(t/1000);
        std::cout<<t<<std::endl;
    });

    iom.schedule([](){
        int t = 3000;
        //std::this_thread::sleep_for(std::chrono::milliseconds(t));
        sleep(t/1000);
        std::cout<<t<<std::endl;
    });
}

// 测试连接Redis_Learn

// 命令编号
enum CMD_FLAG {
    CMD_SET = 0,
    CMD_GET,
    CMD_BGSAVE,
    CMD_SYNC,
    CMD_AOF_REWRIIE,
    CMD_SHUTDOWN
};
std::string sendCmd(int sock, CMD_FLAG flag, std::string key = std::string(), std::string value = std::string());
size_t getBinaryCmd(char *buff, CMD_FLAG cmd_flag, std::string key, std::string value = std::string());

void test_sock(IOManager &iom)
{
    int sock = socket(PF_INET, SOCK_STREAM, 0);
    sockaddr_in master_adr;
    memset(&master_adr, 0, sizeof(master_adr));
    master_adr.sin_family = AF_INET;
    master_adr.sin_addr.s_addr = inet_addr("127.0.0.1");
    master_adr.sin_port = htons(9000);

    int optval = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    if(connect(sock, reinterpret_cast<const sockaddr *>(&master_adr), sizeof(master_adr)))
    {
        std::cerr<<"error"<<std::endl;
        return;
    }
    //std::cout<<"connect success!"<<endl;
    
    

    sendCmd(sock, CMD_SET, "hello","world");
    sendCmd(sock, CMD_SET, "myc","66666");
    sendCmd(sock, CMD_SET, "zsx","world");
    sendCmd(sock, CMD_SET, "mkx","world");
    sendCmd(sock, CMD_SET, "whb","world");
    sendCmd(sock, CMD_SET, "abc","world");
    sendCmd(sock, CMD_SET, "ddd","aaa");
    sendCmd(sock,CMD_GET, "ddd");
    sendCmd(sock,CMD_GET, "myc");
    //sendCmd(sock,CMD_SHUTDOWN);

    return;
}

void test_hook()
{
    //test_sleep();

    IOManager iom(4);
    
    iom.schedule(std::bind(test_sock, std::ref(iom)));
    return;
}
// ============== main ================

int main()
{
    // context_test();

    // testScheduler();

    // testTimer();

    // testIOManager();

    // test_sleep();
    

    test_hook();


    return 0;
}


// =================================================
/**
 * @brief 演示协程主动yield情况下应该如何操作
 */
void test_fiber1() {
    /**
     * 协程主动让出执行权，在yield之前，协程必须再次将自己添加到调度器任务队列中，
     * 否则yield之后没人管，协程会处理未执行完的逃逸状态，测试时可以将下面这行注释掉以观察效果
     */
    Scheduler::GetThis()->schedule(Fiber::GetThis());
    Fiber::GetThis()->yield();
    std::cout<<"test_fiber1 finished\n";
}

/**
 * @brief 演示协程睡眠对主程序的影响
 */
void test_fiber2() {
    /**
     * 一个线程同一时间只能有一个协程在运行，线程调度协程的本质就是按顺序执行任务队列里的协程
     * 由于必须等一个协程执行完后才能执行下一个协程，所以任何一个协程的阻塞都会影响整个线程的协程调度，这里
     * 睡眠的3秒钟之内调度器不会调度新的协程，对sleep函数进行hook之后可以改变这种情况
     */
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::cout<<"test_fiber2 finished\n";

}

void test_fiber3() {
    std::cout<<"test_fiber3 finished\n";
}

void test_fiber5() {
    static int count = 0;
    std::cout<<count<<std::endl;
    count++;
}

/**
 * @brief 演示指定执行线程的情况
 */
void test_fiber4() {
    
    for (int i = 0; i < 3; i++) {
        Scheduler::GetThis()->schedule(test_fiber5, std::this_thread::get_id());
    }
    std::cout<<"test_fiber4 finished\n";
}

void do_io_write()
{
    std::cout << "write callback"<<std::endl;
    int so_err;
    socklen_t len = sizeof(so_err);
    getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &so_err, &len);
    if(so_err) {
        std::cout << "connect fail"<<std::endl;
        return;
    } 
    std::cout << "connect success"<<std::endl;
}

void do_io_read()
{
    std::cout << "read callback"<<std::endl;
    char buf[1024] = {0};
    int readlen = 0;
    readlen = read(sockfd, buf, sizeof(buf));
    if(readlen > 0) {
        buf[readlen] = '\0';
        std::cout << "read " << readlen << " bytes, read: " << buf<<std::endl;
    } else if(readlen == 0) {
        std::cout << "peer closed";
        close(sockfd);
        return;
    } else {
        std::cout << "err, errno=" << errno << ", errstr=" << strerror(errno)<<std::endl;
        close(sockfd);
        return;
    }
    // read之后重新添加读事件回调，这里不能直接调用addEvent，因为在当前位置fd的读事件上下文还是有效的，直接调用addEvent相当于重复添加相同事件
    IOManager::GetThis()->schedule(watch_io_read);
}

void watch_io_read()
{
    std::cout<<"watch_io_read"<<std::endl;
    IOManager::GetThis()->addEvent(sockfd, IOManager::READ, do_io_read);
}

void test_io()
{
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    assert(sockfd > 0);
    fcntl(sockfd, F_SETFL, O_NONBLOCK); // 非阻塞socket
    sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(9000);

    int rt = connect(sockfd, (const sockaddr*)&servaddr, sizeof(servaddr));
    if(rt != 0) 
    {
        if(errno == EINPROGRESS) {
            std::cout << "EINPROGRESS"<<std::endl;
            // 注册写事件回调，只用于判断connect是否成功
            // 非阻塞的TCP套接字connect一般无法立即建立连接，要通过套接字可写来判断connect是否已经成功
            IOManager::GetThis()->addEvent(sockfd, IOManager::WRITE, do_io_write);
            // 注册读事件回调，注意事件是一次性的
            IOManager::GetThis()->addEvent(sockfd, IOManager::READ, do_io_read);
        } else {
            std::cout << "connect error, errno:" << errno << ", errstr:" << strerror(errno)<<std::endl;
        }
    } 
    else
    {
        std::cout << "else, errno:" << errno << ", errstr:" << strerror(errno)<<std::endl;
    }
}



size_t getBinaryCmd(char *buff, CMD_FLAG cmd_flag, std::string key, std::string value)
{
    char *p = buff;

    *(CMD_FLAG*)p =  cmd_flag;
    p = p + sizeof(CMD_FLAG);

    int keyLen = key.length();
    *(size_t*)p = keyLen+1;
    p = p + sizeof(size_t);

    memcpy(p, key.c_str(), keyLen);
    p = p + keyLen;
    *p = '\0';
    ++p;

    int valLen = value.length();
    *(size_t*)(p) = valLen+1;
    p = p + sizeof(size_t);

    if(valLen != 0) memcpy(p,value.c_str(),valLen);
    p += valLen;
    *p = '\0';

    return sizeof(CMD_FLAG) + 2 * sizeof(size_t) + keyLen + valLen + 2;
}

std::string sendCmd(int sock, CMD_FLAG flag, std::string key, std::string value)
{
    char buff[1024];
    size_t len = getBinaryCmd(buff, flag, key, value);
    write(sock,&len, sizeof(size_t));

    write(sock, buff, len);
    //for(;;);
    int rrlen = 0;
    rrlen = read(sock,&len, sizeof(size_t));
    rrlen = read(sock,buff,len);
    //std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    buff[len]='\0';
    cout<<buff<<endl;
    return string(buff);

}