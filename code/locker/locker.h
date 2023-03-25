/*
在实现线程池之前，先把锁和信号量，条件变量封装一下

*/
#ifndef LOCKER_H
#define LOCKER_H

#include <pthread.h>
#include <exception>
#include <semaphore.h>

//互斥锁类
class Locker{
private:
    pthread_mutex_t m_mutex;
public:
    Locker();
    ~Locker();
    bool Lock();
    bool unLock();
    pthread_mutex_t*Get();

};

//条件变量类
class Cond{
public:
    Cond();
    ~Cond();
    bool Wait(pthread_mutex_t * mutex);
    bool TimedWait(pthread_mutex_t * mutex,struct timespec t);
    bool Signal();
    bool Broadcast();

private:
pthread_cond_t m_cond;
};

//信号量类
class Sem{
public:
    Sem();
    Sem(int value);
    ~Sem();
    bool Wait();
    bool Post();
    
private:
sem_t m_sem;
};


#endif