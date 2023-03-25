#ifndef SOCKETCONTROL_H
#define SOCKETCONTROL_H
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>




void Setnonblocking(int fd);

void Addfd(int epollfd,int fd,bool et,bool one_shot);

void Removefd(int epollfd,int fd);

void Modfd(int epollfd,int fd,int event);

#endif