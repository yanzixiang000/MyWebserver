#include "socket_control.h"

void Setnonblocking(int fd){
    int flag = fcntl(fd,F_GETFL);
    flag |=O_NONBLOCK;
    fcntl(fd,F_SETFL,flag);

}
void Addfd(int epollfd,int fd,bool et,bool one_shot){
    struct epoll_event epev;
    epev.events = EPOLLIN  | EPOLLRDHUP |EPOLLHUP;
    if(et){
        epev.events|= EPOLLET;
    }
    if(one_shot){
        epev.events |= EPOLLONESHOT;
    }
    epev.data.fd = fd;
    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&epev);
    Setnonblocking(fd);
}

void Removefd(int epollfd,int fd){
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
}

void Modfd(int epollfd,int fd,int event){
    struct epoll_event epev;
    epev.events = event | EPOLLET | EPOLLRDHUP |EPOLLHUP| EPOLLONESHOT;
    epev.data.fd = fd;
    epoll_ctl(epollfd,EPOLL_CTL_MOD,fd,&epev);
}