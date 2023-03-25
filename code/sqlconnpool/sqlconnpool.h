#ifndef SQLCONNPOOL_H
#define SQLCONNPOOL_H

#include <mysql/mysql.h>//这是C的mysql库
#include <string>
#include <queue>
#include <mutex>
#include <semaphore.h>
#include <thread>

#include "../log/log.h"


//数据库连接池，初始化时先创建好mysql的多个连接，放入队列
//有要使用的就从队列中拿，释放时再放回队列中
//数据连接池还有一个真正释放连接的函数

class SqlConnPool {
public:
    static SqlConnPool* Instance();

    //从队列中获取一个数据库连接
    MYSQL *GetConn();
    //使用完连接后放回队列
    void FreeConn(MYSQL * conn);
    //获取队列中剩余的未使用连接数量
    int GetFreeConnCount();
    //产生mysql连接的函数并放入队列
    //主机名，MYSQL的端口号，用户名，密码，数据库名，mysql连接数量
    void Init(const char* host, int port,
              const char* user,const char* pwd, 
              const char* dbName, int connSize);
    //释放掉队列中所有的连接
    void ClosePool();

private:
    SqlConnPool();
    ~SqlConnPool();

    int MAX_CONN_;//最大连接数
    int useCount_;//当前连接数
    int freeCount_;//剩余连接数

    std::queue<MYSQL *> connQue_;//队列，用于保存mysql指针，这个指针是MYSQL库中的，用于操作mysql数据库
    std::mutex mtx_;//互斥锁
    sem_t semId_;//信号量

private:
    static SqlConnPool* sqlpoolptr;
};


#endif // SQLCONNPOOL_H