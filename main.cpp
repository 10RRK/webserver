#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include "threadpool.h"
#include "http_conn.h"

#define MAX_FD 65536   // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000  // 监听的最大的事件数量

extern void addfd(int epollfd, int fd, bool one_shot);  // 向epoll中添加需要监听的文件描述符
extern void removefd(int epollfd, int fd);      // 从epoll中移除监听的文件描述符

// 设置信号的处理函数
void addsig(int sig, void(handler)(int))
{
    struct sigaction sa;    // 指定信号的处理方式（sigaction函数的参数）
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;    // 指定信号处理函数
    sigfillset(&sa.sa_mask);    // 设置进程的信号掩码
    assert(sigaction(sig, &sa, NULL) != -1);    // sig为要捕获的信号类型
}

int main(int argc, char* argv[]) 
{
    if(argc <= 1)     // 提示需要输入端口号参数
    {
        printf("usage: %s port_number\n", basename(argv[0]));  // 第一个数组元素argv[0]是程序名称，并且包含程序所在的完整路径
        return 1;
    }

    int port = atoi(argv[1]);   // 将输入的端口号字符串转换成整数
    addsig(SIGPIPE, SIG_IGN);   // 忽略SIGPIPE信号（SIGPIPE：往读端被关闭的管道或者socket连接中写数据）

    // 创建线程池
    threadPool<http_conn>* pool = NULL;
    try 
    {
        pool = new threadPool<http_conn>;
    } 
    catch( ... ) 
    {
        return 1;
    }

    http_conn* users = new http_conn[MAX_FD];   // 预先为每个可能的客户连接分配一个http_conn对象

    int listenfd = socket(PF_INET, SOCK_STREAM, 0); // 创建socket，TCP/IP协议族，流服务（TCP），默认协议

    struct sockaddr_in address;     // TCP/IP协议族 IPv4 socket地址结构体
    address.sin_addr.s_addr = INADDR_ANY;   // IPv4地址结构体，s_addr为IPv4地址，要用网络字节序表示;服务端可以用INADDR_ANY，客户端不行
    address.sin_family = AF_INET;      // 地址族（IPv4）
    address.sin_port = htons(port);   // 端口号，要用网络字节序表示; htons表示host to network short

    // 端口复用
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));  // 设置socket文件描述符属性（这里是设置端口复用）

    int ret = 0;
    ret = bind(listenfd, (struct sockaddr*)&address, sizeof(address));  // 命名socket(将一个socket与socket地址绑定)
    ret = listen(listenfd, 5);  // 监听socket，内核监听队列的最大长度（典型值为5）

    // 创建epoll对象，和事件数组，添加
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    // 将listenfd上的注册事件添加到epoll对象中（epoll事件表中）
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;

    while(true) 
    {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);     // 成功时返回就绪的文件描述符的个数
        
        if((number < 0) && (errno != EINTR))    // EINTR为被中断，这种情况不是epoll调用失败
        {
            printf("epoll failure\n");
            break;
        }
        // 循环遍历事件数组
        for(int i = 0; i < number; i++) 
        {
            int sockfd = events[i].data.fd;     
            if(sockfd == listenfd)      // 有客户端连接进来
            {
                struct sockaddr_in client_address;  // 用于获取被接受连接的远端socket地址
                socklen_t client_addrlength = sizeof(client_address);   // 客户端socket地址的长度
                while(1) 
                {    
                    int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlength);   // 从listen监听队列中接受一个连接;成功时返回一个新的连接socket
                    if (connfd < 0)     
                    {
                        if(!(errno == EAGAIN || errno == EWOULDBLOCK))  // 没有数据。对于非阻塞IO，下面的条件成立表示数据已经全部读取完毕
                            printf("errno is: %d\n", errno);
                        break;
                    }
                    if(http_conn::m_user_count >= MAX_FD)   // 目前支持的连接数满了
                    {
                        close(connfd);
                        break;
                    }
                    users[connfd].init(connfd, client_address); // 初始化客户连接（包含向epoll添加connfd文件描述符的操作，成员变量的初始化等）
                }
            } 
            else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))  // TCP连接被对方关闭或对方关闭了写操作，挂起，错误
            {
                users[sockfd].close_conn();     // 如果有异常，直接关闭客户连接
            } 
            else if(events[i].events & EPOLLIN) 
            {
                if(users[sockfd].read())    // 根据读的结果，决定是将任务添加到线程池，还是关闭连接
                    pool->addTask(users+sockfd);
                else
                    users[sockfd].close_conn();
            }  
            else if(events[i].events & EPOLLOUT) 
            {
                if(!users[sockfd].write())  // 根据写的结果，决定是否关闭连接
                    users[sockfd].close_conn();
            }
        }
    }
    
    close(epollfd);
    close(listenfd);
    delete[] users;
    delete pool;
    return 0;
}