#ifndef LOG_H
#define LOG_H

#include <mutex>
#include <string>
#include <thread>
#include <sys/time.h>
#include <string.h>
#include <stdarg.h>           // vastart va_end
#include <assert.h>
#include <sys/stat.h>         //mkdir
#include "blockqueue.hpp"
#include "../buffer/buffer.h"

class Log {
public:
    //日志初始化就做两件事
    //1.如果异步日志，就生成队列和写线程，同步日志做个标记即可
    //2.生成要写入的日志文件
    void Init(int level, const char* path = "./log", 
                const char* suffix =".log",
                int maxQueueCapacity = 1024);
    //获取日志单例
    static Log* Instance();
    //写线程的执行函数
    static void FlushLogThread();
    //这里是生成日志的要写的日志信息，以及如果等于50000行，要把操作的日志文件换了
    //异步日志把缓冲区信息放入阻塞队列，等待线程写
    //如果是同步日志，就直接写生成的缓冲区信息到日志文件中
    void Write(int level, const char *format,...);
    //唤醒列队的pop让写线程继续写，并且刷新缓冲区
    void Flush();
    //获取日志等级
    int GetLevel();
    //设置日志等级
    void SetLevel(int level);
    //判断日志是否初始化
    bool IsOpen() { return isOpen_; }
    
private:
    // 构造和析构都私有化
    Log();
    ~Log();
    //通过信息等级给日志信息加上类别的函数
    void AppendLogLevelTitle_(int level);
    //写线程调用的异步写函数
    void AsyncWrite_();

private:
    static const int LOG_PATH_LEN = 256;//日志路径长度
    static const int LOG_NAME_LEN = 256;//日志名称长度
    static const int MAX_LINES = 50000;//一个日志文件的最大行，如果超过，就要弄第二个文件

    const char* path_;//路径
    const char* suffix_;//前缀


    int lineCount_;//当前日志文件行数

    int toDay_;//记录当前日期

    bool isOpen_;//日志是否初始化
 
    Buffer buff_;//生成的写入日志的信息一开始就放在这里
    int level_;//日志级别
    bool isAsync_;//是否异步

    FILE* fp_;//文件指针，用于操作当前日志文件的
    std::unique_ptr<BlockDeque<std::string>> deque_; //阻塞队列
    std::unique_ptr<std::thread> writeThread_;//写的线程
    std::mutex mtx_;


private:
    static Log* logptr;//单例日志，实例在.cpp文件中生成
};


//这是日志之外的宏函数

//先获取单例的日志
//判断日志是否打开，并且日志等级低于信息的等级，才能进行写，当日志等级大于信息等级时，就不能写
//也就是说日志输出的信息也是有等级的，当输出的信息等级低的时候，只有日志等级也低才能写。否则就不能写。
//通过给定日志的等级，可以让一些日志信息不能输入到日志中，比如日志等级0，就可以输出debug，日志等级1，就不能输出debug，只能info及以上

//加宏函数的一个主要原因就是利用等级隔离一些信息
#define LOG_BASE(level, format, ...)\
    do {\
        Log* log = Log::Instance();\
        if (log->IsOpen()&&log->GetLevel()<= level){\
            log->Write(level,format,##__VA_ARGS__);\
            log->Flush();\
        }\
    } while(0);

//##__VA_ARGS__是一个宏，代表可变参数，用法    LOG_INFO("Client[%d](%s:%d) in, userCount:%d", fd_, GetIP(), GetPort(), (int)userCount);
//format代表字符串的形式，字符串中的一些参数，就在可变参数一种
#define LOG_DEBUG(format, ...) do {LOG_BASE(0, format, ##__VA_ARGS__)} while(0);//debug信息等级就是0
#define LOG_INFO(format, ...) do {LOG_BASE(1, format, ##__VA_ARGS__)} while(0);
#define LOG_WARN(format, ...) do {LOG_BASE(2, format, ##__VA_ARGS__)} while(0);
#define LOG_ERROR(format, ...) do {LOG_BASE(3, format, ##__VA_ARGS__)} while(0);

#endif //LOG_H