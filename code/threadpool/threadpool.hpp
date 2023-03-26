/*
线程池类
当前主机1cpu，8核，16线程

*/

#ifndef THREADPOLL_H
#define THREADPOLL_H

#include <pthread.h>
#include <list>
#include <exception>
#include <iostream>
#include <unordered_map>
#include "../locker/locker.h"
#include "../log/log.h"


//定义为模板类，T为线程需要执行的任务类，这样本次虽然任务类是解析HTTP，但可以添加其他任务类以完成其他任务
template<typename T>
class ThreadPool{

public:
    ThreadPool(int thread_number = 6 ,int  max_requests = 10000);
    ~ThreadPool();
    bool Append(T* request);

private:
    static void* Worker(void* arg);//静态函数，只能访问静态成员
    void Run();


private:
    //线程池中的线程数量
    int m_thread_number;
    //线程池数组，动态创建，大小为m_thread_number
    pthread_t * m_threads;
    //请求队列数组,为每一个线程都生成一个请求队列
    std::list<T*>* m_workqueues;
    //每一个请求队列中允许的最大请求数
    int m_max_requests;
    // //保护请求队列的互斥锁
    // locker m_queuelocker;
    //每一个请求队列都对应一个信号量，平时把线程阻塞，对应的任务队列中有任务时把线程唤醒
    Sem* m_queuestats;

    //是否结束线程，因为所有线程都共用一个线程池，所以m_stop=true会结束所有准备开始下一轮的线程
    bool m_stop;

    //用于轮询，知道该放入第几个线程的请求队列中
    int m_number;
    //一个哈希表，需要告诉每个线程自己的id号对应的是第几个线程
    std::unordered_map<pthread_t,int> hash;

};

//构造函数
template<typename T>
ThreadPool<T>::ThreadPool(int thread_number ,int  max_requests):
    m_thread_number(thread_number),m_threads(nullptr),m_workqueues(nullptr)
    ,m_max_requests(max_requests),m_queuestats(nullptr),m_stop(false),m_number(0){

    if((thread_number<=0) || (max_requests<=0)){
        LOG_ERROR("thread_number<=0 || max_requests<=0");
        throw std::exception();
    }
    //这些必须先创建，因为第一个线程创建后就会去使用，没有就会出现段错误
    //为每一个线程创建请求队列
    m_workqueues = new std::list<T*>[m_thread_number];
    //每个请求队列一个信号量
    m_queuestats = new Sem[m_thread_number];

    //创建线程数组
    m_threads= new pthread_t[m_thread_number];

    if(!m_threads){
        LOG_ERROR("new pthread_t[] error");
        throw std::exception();
    }

    //创建m_thread_number个线程,并设置为线程脱离，并告诉线程tid对应的序号放在哈希表中
    for(int i=0;i<m_thread_number;++i){
        std::cout<<"create the "<<i<<"th thread"<<std::endl;
        if(pthread_create(m_threads+i,NULL,Worker,this)!=0){//worker是子线程执行的代码，在C++中必须是静态的
            LOG_ERROR("pthread_create() error");
            delete[] m_threads;
            throw std::exception();
        }
        if(pthread_detach(m_threads[i])!=0){
            LOG_ERROR("pthread_detach() error");
            delete[] m_threads;
            throw std::exception();
        }
        hash.insert(std::pair<pthread_t,int>(m_threads[i],i));
    }

    
}


//析构函数
template<typename T>
ThreadPool<T>::~ThreadPool(){
    m_stop = true;
    //析构的时候设置了true，但是阻塞状态根本没办法结束，所以需要全部唤醒
    for(int i=0;i<m_thread_number;++i){
        m_queuestats[i].Post();
    }
    
    delete [] m_workqueues;
    delete [] m_queuestats;
    delete [] m_threads;
}

//把任务指针加入队列，轮询放入
template<typename T>
bool ThreadPool<T>::Append(T* request){
    int start = m_number;
    while(m_workqueues[m_number].size()>=m_max_requests){//先找一个不满的请求队列
        ++m_number;
        if(m_number>=m_thread_number){
            m_number=0;
        }
        if(m_number==start){//如果再次回到原地，就放弃
            return false;
        }
    }
    m_workqueues[m_number].push_back(request);
    m_queuestats[m_number].Post();
    ++m_number;
    if(m_number>=m_thread_number){
        m_number=0;
    }


    return true;
}

//worker是子线程要运行的程序，但由于不能访问非静态成员，是因为没有this指针。
//所以把this当作变量进行传递，就可以在worker中调用this中的非静态成员了
template<typename T>
void* ThreadPool<T>::Worker(void* arg){
    ThreadPool * pool = (ThreadPool*)arg;
    pool->Run();
    return pool;
}

//run函数就是循环的从工作队列中取任务并执行
template<typename T>
void ThreadPool<T>::Run(){
    int number=hash[pthread_self()];
    while(!m_stop){
        m_queuestats[number].Wait();//要从线程对应的工作队列中取,就要用对应的信号量
        if(m_workqueues[number].empty()){//其实就一个线程对应一个队列，如果要析构，唤醒后是可能为空的，continue后再次判断就能停下来
            continue;
        }
        T* request = m_workqueues[number].front();
        m_workqueues[number].pop_front();
        if(!request){//如果为空，就再跳过
            continue;
        }
        request->Process();//线程去执行任务中的process类，任务类中一定要有这个函数
    }

}

#endif