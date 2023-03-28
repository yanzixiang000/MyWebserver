#include "locker.h"
//------------------------------------互斥锁---------------
Locker::Locker(){
    if(pthread_mutex_init(&m_mutex, NULL)!= 0){
        throw std::exception();
    }
}

Locker::~Locker(){
    pthread_mutex_destroy(&m_mutex);
}

bool Locker::Lock(){
    return pthread_mutex_lock(&m_mutex) == 0;
};

bool Locker::unLock(){
    return pthread_mutex_unlock(&m_mutex) == 0;
}

pthread_mutex_t* Locker::Get(){
    return &m_mutex;
}

//------------------------------------读写锁---------------
RWlocker::RWlocker(){
    if(pthread_rwlock_init(&m_rwlock, NULL)!= 0){
        throw std::exception();
    }
}
RWlocker::~RWlocker(){
    pthread_rwlock_destroy(&m_rwlock);
}

bool RWlocker::rdLock(){
    return pthread_rwlock_rdlock(&m_rwlock) == 0;
};

bool RWlocker::wrLock(){
    return pthread_rwlock_wrlock(&m_rwlock) == 0;
};


bool RWlocker::unLock(){
    return pthread_rwlock_unlock(&m_rwlock) == 0;
}



//------------------------------------条件变量---------------
Cond::Cond(){
    if(pthread_cond_init(&m_cond, NULL) != 0){
        throw std::exception();
    }
}

Cond::~Cond(){
    pthread_cond_destroy(&m_cond);
}
bool Cond::Wait(pthread_mutex_t * mutex){
    return pthread_cond_wait(&m_cond,mutex)==0;
    
}

bool Cond::TimedWait(pthread_mutex_t * mutex,struct timespec t){
    return pthread_cond_timedwait(&m_cond,mutex,&t)==0;
    
}

bool Cond::Signal(){
    return pthread_cond_signal(&m_cond)==0;
    
}

bool Cond::Broadcast(){
    return pthread_cond_broadcast(&m_cond)==0;
    
}


//------------------------------------信号量-----------------

Sem::Sem(){
    if(sem_init(&m_sem, 0,0) != 0){
        throw std::exception();
    }
}


Sem::Sem(int value){
    if(sem_init(&m_sem, 0,value) != 0){
        throw std::exception();
    }
}


Sem::~Sem(){
    sem_destroy(&m_sem);
}


bool Sem::Wait(){
    return sem_wait(&m_sem)==0;
    
}


bool Sem::Post(){
    return sem_post(&m_sem)==0; 
}