
#include "sqlconnpool.h"
using namespace std;


//生成数据库连接池的单例
SqlConnPool* SqlConnPool::sqlpoolptr = new SqlConnPool;


SqlConnPool::SqlConnPool() {
    useCount_ = 0;
    freeCount_ = 0;
}

SqlConnPool* SqlConnPool::Instance() {
    return sqlpoolptr;
}

//之所以不在构造时初始化，是因为这里用了单例模式，一开始就构造了一个，但是不知道这些参数，所以等后面再初始化
void SqlConnPool::Init(const char* host, int port,
            const char* user,const char* pwd, const char* dbName,
            int connSize = 10) {

    assert(connSize > 0);
    for (int i = 0; i < connSize; i++) {
        MYSQL *sql = nullptr;
        sql = mysql_init(sql);//初始化一个mysql结构
        if (!sql) {
            LOG_ERROR("MySql init error!");
            assert(sql);
        }
        sql = mysql_real_connect(sql, host,
                                 user, pwd,
                                 dbName, port, nullptr, 0);//真正连接到MYSQL数据库
        if (!sql) {
            LOG_ERROR("MySql Connect error!");
        }
        connQue_.push(sql);
    }
    MAX_CONN_ = connSize;
    sem_init(&semId_, 0, MAX_CONN_);
}

MYSQL* SqlConnPool::GetConn() {
    MYSQL *sql = nullptr;
    sem_wait(&semId_);//从队列中拿一个，如果拿不到，就阻塞等待
    if(connQue_.empty()){
        LOG_WARN("sem_wait() fake post");
        return nullptr;
    }
    lock_guard<mutex> locker(mtx_);
    sql = connQue_.front();
    connQue_.pop();
    
    return sql;
}

void SqlConnPool::FreeConn(MYSQL* sql) {
    assert(sql);
    lock_guard<mutex> locker(mtx_);
    connQue_.push(sql);
    sem_post(&semId_);
}

void SqlConnPool::ClosePool() {
    lock_guard<mutex> locker(mtx_);
    while(!connQue_.empty()) {
        auto item = connQue_.front();
        connQue_.pop();
        mysql_close(item);
    }
    mysql_library_end();//连接关闭后还需要把整体资源释放掉，避免在使用库完成应用程序后发生内存泄漏
}

int SqlConnPool::GetFreeConnCount() {
    lock_guard<mutex> locker(mtx_);
    return connQue_.size();
}

SqlConnPool::~SqlConnPool() {
    ClosePool();
}

