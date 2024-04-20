#include "Scheduler.h"
#include "Timer.h"
#include "IOManager.h"
// #include "Hook.h"

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <iostream>
#include <stack>
#include <string.h>

static int sock_listen_fd = -1;

void test_accept();
void error(char *msg)
{
    perror(msg);
    printf("erreur....\n");
    exit(1);
}

void watch_io_read()
{
    IOManager::GetThis()->addEvent(sock_listen_fd, IOManager::READ, test_accept);
}

void test_accept()
{ // 回声服务器
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    socklen_t len = sizeof(addr);
    int fd = accept(sock_listen_fd, reinterpret_cast<sockaddr *>(&addr), &len);
    errno;
    if (fd < 0)
    {
        std::cout << "fd ==" << fd << "accept false" << std::endl;
    }
    else
    {
        std::cout<<"accept successful"<<std::endl;
        fcntl(fd, F_SETFL, O_NONBLOCK);
        IOManager::GetThis()->addEvent(fd, IOManager::READ, [fd]()
                                       {
            char buffer[1024];
            memset(buffer, 0, sizeof(buffer));
            for(;;)
            {
                int ret = read(fd, buffer, sizeof(buffer));
                if(ret > 0)
                {
                    ret = write(fd, buffer, sizeof(buffer));
                }
                if(ret <= 0)
                {
                    if(errno == EAGAIN) continue;
                    close(fd);
                    break;
                }
            } });
    }
    IOManager::GetThis()->schedule(watch_io_read);
}

void test_IOmanager()
{
    int portno = 9000;
    sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    sock_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_listen_fd < 0)
    {
        std::cerr << "Error creating socket listen-fd." << std::endl;
        exit(0);
    }
    int yes = 1;
    setsockopt(sock_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    memset(reinterpret_cast<char *>(&server_addr), 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(portno);

    if (bind(sock_listen_fd, reinterpret_cast<sockaddr *>(&server_addr), sizeof(server_addr)) < 0)
    {
        std::cerr << "Error creating socket listen-fd." << std::endl;
        exit(0);
    }
    printf("epoll echo server listening for connetions on port : %d\n", portno);

    if (listen(sock_listen_fd, 1) == -1)
    {
        std::cerr << "Error listen socket listen-fd." << std::endl;
        exit(0);
    }
    fcntl(sock_listen_fd, F_SETFL, O_NONBLOCK);
    IOManager::GetThis()->addEvent(sock_listen_fd, IOManager::READ, test_accept);
}

int main(int argc, char *argv[])
{
    IOManager iom;
    iom.schedule(test_IOmanager);
    for(;;);
    return 0;
}