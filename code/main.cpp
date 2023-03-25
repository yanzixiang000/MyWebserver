/*


大致结构是这样的

三个主要结构，一个就是主线程，一个是线程池，一个就是任务类

主线程里保存一个线程池，和众多任务，一个连接socket就分配一个任务
然后主线程中对epollwait等待 监听socket的连接请求，以及连接socket的可读和可写，然后都在主线程中完成
读数据是用任务中的读方法读到连接socket的对应任务中，再把任务加入到线程池的请求队列，线程再进行处理读到的数据，并把要写的内容放到一个写缓存中，
写数据也是用的任务的写方法，把任务中的写缓存的数据写出去。


线程池，就是一开始创建线程，每个线程都循环从请求队列中拿任务，没有任务就先阻塞，有任务就被唤醒去拿任务并执行


每个连接socket都有自己的任务类，任务类就是把数据先读到任务中，然后处理（解析，然后决定写什么），再把任务中要写的写出去
读和写都是主线程来操作，处理是加入请求队列后由子线程来进行。


*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>

#include <signal.h>
#include <iostream>

#include <memory>
#include "locker/locker.h"
#include "socket_control/socket_control.h"
#include "threadpool/threadpool.hpp"
#include "http/http_conn.h"
#include "timer/heaptimer.h"
#include "log/log.h"
#include "sqlconnpool/sqlconnpool.h"


#define MAX_FD 65535 //最大的套接字数量
#define MAX_EVENT_NUMBER 50000 //允许同时发生的最大数量
#define OVERTIME_MS 60000 //每个连接的时间，单位ms，如果这么长时间没有读时间发生，就会断开连接，如果有时间发生，在时间结束后会再延长这么久


//添加信号的函数
void Addsig(int sig,void(*handler)(int)){
    struct sigaction sa;
    memset(&sa,'\0',sizeof(sa));
    sa.sa_handler = handler;
    sa.sa_flags = 0|SA_RESTART;
    sigfillset(&sa.sa_mask);
    sigaction(sig,&sa,nullptr);
}



//用于传给定时器的超时回调函数
void TimeCallBack(Http_Conn * user,int fd){
    user[fd].Close_Conn();
}


int main(int argc,char* argv[]) {

    if(argc!=2)
    {
        printf("运行方式 : %s <port>\n" , argv[0]);
        exit(1);//直接退出程序
    }

    //添加对SIGPIPE的信号,如果向封闭管道写入数据，忽略
    //就这里添加了个信号，没有添加其他信号
    Addsig(SIGPIPE,SIG_IGN);

    //日志是单例模式的，不需要new，只需要对日志进行初始化
    Log::Instance()->Init(1,"./log",".log",1024);

    //sql连接池也是单例模式，只需要对其进行一个初始化即可
    SqlConnPool::Instance()->Init("localhost",3306,"debian-sys-maint","mysql","webserver",8);

    //创建一个时间堆
    HeapTimer timeheap;
    
    //创建一个用http状态机这个类处理http协议的线程池，初始化线程池
    std::shared_ptr<ThreadPool<Http_Conn>> pool(new ThreadPool<Http_Conn>);//结束后会自动delete

    //由于http_conn中需要保存套接字，所以干脆套接字信息都保存在http_conn中，所以先为每一个可能存在的套接字都分配一个任务
    Http_Conn *users = new Http_Conn[MAX_FD];

    //创建监听套接字
    int listenfd = socket(PF_INET,SOCK_STREAM,0);
    if(listenfd == -1)//套接字创建失败
    {
        LOG_ERROR("socket() error");
        exit(1);
    }
    //设置一个端口复用
    int reuse =1;
    setsockopt(listenfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    //绑定
    struct sockaddr_in serv_addr;
    memset(&serv_addr,0,sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(atoi(argv[1]));
    if(bind(listenfd,(struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1){
        //perror("bind() error");
        LOG_ERROR("bind() error");
        exit(1);
    }
    //监听
    if(listen(listenfd,100) == -1){
        LOG_ERROR("listen() error");
        //perror("listen() error");
        exit(1);
    }


    //创建epoll对象
    int epfd = epoll_create(100);
    // 将监听的文件描述符相关的检测信息添加到epoll实例中，用一个函数实现
    Addfd(epfd,listenfd,false,false);
    //允许同时发生事件是有上限的
    struct epoll_event epevs[MAX_EVENT_NUMBER];

    Http_Conn::m_epollfd = epfd;

    LOG_INFO("========== Server init ==========");

    while(1) {

        //获取要等待的时间,单位是ms,如果时间堆为空，timeout=-1.
        //获取时间之前会先处理超时的定时器
        int timeout = timeheap.GetNextTick(users,OVERTIME_MS);

        int number = epoll_wait(epfd, epevs, MAX_EVENT_NUMBER, timeout);
        if(number == -1) {//由于信号处理中设置了restart，所以-1绝对是出问题了，而不是信号打断
            LOG_ERROR("epoll_wait() error");
            exit(-1);
        }

        for(int i = 0; i < number; ++i) {

            int curfd = epevs[i].data.fd;

            if(curfd == listenfd) {
                // 监听的文件描述符有数据达到，有客户端连接
                struct sockaddr_in cliaddr;
                socklen_t len = sizeof(cliaddr);
                while(1){//这里添加一个循环，免得每次只取一个，就很麻烦
                    int connfd = accept(listenfd, (struct sockaddr *)&cliaddr, &len);
                    if(connfd<0){
                        if(errno == EAGAIN || errno== EWOULDBLOCK){
                            break;
                        }
                        LOG_ERROR("accept() error");
                        exit(-1);
                    }
                    if(Http_Conn::m_user_count>=MAX_FD){
                        LOG_WARN("Clients is full!");
                        close(connfd);
                        break;
                    }
                    //直接把描述符值当索引，放到对应位置的任务中。//本来是需要在外面添加上epfd的，但是由于任务内部也有epfd，所以在任务内部添加了epfd
                    users[connfd].Init(connfd,cliaddr);
                    //加入与套接字相对应的定时器
                    timeheap.Add(connfd,OVERTIME_MS,TimeCallBack);
                    
                }
            } else if(epevs[i].events & (EPOLLRDHUP | EPOLLRDHUP |EPOLLERR)){//EPOLLERR没注册
                //如果是对面传来的关闭信号，这里就直接关闭
                //由于sock直接存在任务中，直接在任务中写好关闭连接，并进行关闭即可
                users[curfd].Close_Conn();
            }
            else if(epevs[i].events & EPOLLIN){
                //如果是读的事件,直接在主进程读
                if(users[curfd].Read()){
                    //读完后再加入请求队列
                    if(!pool->Append(&users[curfd])){
                        //加入失败也关闭连接
                        users[curfd].Close_Conn();
                        continue;
                    }
                    //如果读成功并且成功加入请求队列，就要标注此事件，后面时间堆处理超时事件时如果有标注就不会清理，而是扩展时间
                    timeheap.Happen(curfd);
                }else{//如果读失败，直接关闭连接
                    users[curfd].Close_Conn();
                }
            }else if(epevs[i].events & EPOLLOUT){//write会一次性写完所有数据，如果写失败了，也要关闭连接
                    if(!users[curfd].Write()){
                        users[curfd].Close_Conn();
                    }
                }

        }
    }

    close(listenfd);
    close(epfd);
    delete[] users;
    //pool用了智能指针，不用手动delelt
    return 0;
}