/*
同步日志就是把要发的日志放入缓冲区，并直接发送
异步日志就是把要发的日志放入一个队列，由写线程来发送
*/

#include "log.h"

using namespace std;

//预先构造一个日志
//并且单例模式的static，又或者其他的static类型的变量，一定要在.cpp文件中构造，否则就算有条件编译，链接时也会出现重复定义
Log* Log::logptr = new Log;

Log::Log() {//构造函数
    lineCount_ = 0;
    isAsync_ = false;
    writeThread_ = nullptr;
    deque_ = nullptr;
    toDay_ = 0;
    fp_ = nullptr;
}

Log::~Log() {//析构函数
    if(writeThread_ && writeThread_->joinable()) {//如果写线程存在并可以由主线程回收资源
        while(!deque_->Empty()) {//只要队列非空，就不停唤醒写线程写入日志
            deque_->Flush();
        };
        deque_->Close();//会把队列再次清空，并让pop返回false，使得写线程结束
        writeThread_->join();//回收写线程资源
    }
    if(fp_) {//如果日志存在
        lock_guard<mutex> locker(mtx_);
        Flush();//刷新缓冲区
        fclose(fp_);//关闭日志
    }
}

int Log::GetLevel() {//获取日志等级
    lock_guard<mutex> locker(mtx_);
    return level_;
}

void Log::SetLevel(int level) {//设置日志等级
    lock_guard<mutex> locker(mtx_);
    level_ = level;
}

//日志初始化就做两件事
//1.如果异步日志，就生成队列和写线程，同步日志做个标记即可
//2.生成要写入的日志文件

void Log::Init(int level = 1, const char* path, const char* suffix, int maxQueueSize) {
    //初始化默认级别为1，路径是放日志的位置，一般是./log，suffix后缀一般是.log，最后一个是异步队列大小，可用于判断同步还是异步，如果为0，是用同步日志
    isOpen_ = true;
    level_ = level;
    if(maxQueueSize > 0) {
        isAsync_ = true;//异步日志就生成阻塞队列和线程
        if(!deque_) {
            unique_ptr<BlockDeque<std::string>> newDeque(new BlockDeque<std::string>(maxQueueSize));
            deque_ = move(newDeque);
            std::unique_ptr<std::thread> NewThread(new thread(FlushLogThread));
            writeThread_ = move(NewThread);
        }
    } else {
        isAsync_ = false;//同步日志做个标记
    }

    lineCount_ = 0;

    //然后创建日志文件夹

    time_t timer = time(nullptr);// 基于当前系统的当前日期/时间，单位是秒
    struct tm *sysTime = localtime(&timer);//返回一个指向表示本地时间的 tm 结构的指针，转换类型
    struct tm t = *sysTime;
    path_ = path;//放日志的路径
    suffix_ = suffix;//文件后缀
    char fileName[LOG_NAME_LEN] = {0};
    snprintf(fileName, LOG_NAME_LEN - 1, "%s/%04d_%02d_%02d%s", 
            path_, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, suffix_);//生成文件的名字信息，路径，年月日，后缀
    toDay_ = t.tm_mday;

    {
        lock_guard<mutex> locker(mtx_);
        buff_.RetrieveAll();//清空放信息的数组
        if(fp_) {//如果文件指针已经存在
            Flush();//刷新文件缓冲区
            fclose(fp_); 
        }
        //以附加的方式打开只写文件，如果文件不存在会建立文件
        fp_ = fopen(fileName, "a");
        if(fp_ == nullptr) {//如果失败，就先生成文件夹，再以附加方式打开只写文件
            mkdir(path_, 0777);
            fp_ = fopen(fileName, "a");
        } 
        assert(fp_ != nullptr);
    }
}

//这里是生成日志的要写的日志信息，以及如果等于50000行，要把操作的日志文件换了
//异步日志把信息放入阻塞队列，等待线程写
//如果是同步日志，就把信息放入缓冲区，还要直接写信息
void Log::Write(int level, const char *format, ...) {

    //获取当前时间
    struct timeval now = {0, 0};
    gettimeofday(&now, nullptr);//得到精确时间，精度可以达到微妙
    time_t tSec = now.tv_sec;//秒数信息
    struct tm *sysTime = localtime(&tSec);//转换为日期和时间信息
    struct tm t = *sysTime;
    va_list vaList;

    /* 日志日期 日志行数 */
    //生成新的文件用来输出日志信息
    //如果现在写的日期和日志日期不同，就要换日志，或者满50000行也换日志

    unique_lock<mutex> locker(mtx_);

    if (toDay_ != t.tm_mday || (lineCount_ && (lineCount_  %  MAX_LINES == 0)))
    {
        
        char newFile[LOG_NAME_LEN];
        char tail[36] = {0};
        snprintf(tail, 36, "%04d_%02d_%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);

        if (toDay_ != t.tm_mday)
        {
            snprintf(newFile, LOG_NAME_LEN - 72, "%s/%s%s", path_, tail, suffix_);//如果只是日期不同，就换个日期即可
            toDay_ = t.tm_mday;//记录日志日期
            lineCount_ = 0;//并把行数归零
        }
        else {
            snprintf(newFile, LOG_NAME_LEN - 72, "%s/%s-%d%s", path_, tail, (lineCount_  / MAX_LINES), suffix_);//如果是日期相同，说明是同日期的装满了，那么生成一个新日志即可
            //日志日期和行数都不用变
        }

        //unique_lock<mutex> locker(mtx_);
        Flush();
        fclose(fp_);
        fp_ = fopen(newFile, "a");//关闭旧文件，打开新文件，如果没有再创建
        assert(fp_ != nullptr);
    }

        
    //生成要输入的日志信息，首先都先弄到信息的缓冲区中，如果异步，再放入队列
    //如果同步，就直接输入日志文件
    {
        //unique_lock<mutex> locker(mtx_);
        lineCount_++;//生成一行信息，行数就++
        int n = snprintf(buff_.BeginWrite(), 128, "%d-%02d-%02d %02d:%02d:%02d.%06ld ",
                    t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                    t.tm_hour, t.tm_min, t.tm_sec, now.tv_usec);//生成日期信息
                    
        buff_.HasWritten(n);
        AppendLogLevelTitle_(level);//放入日志信息的类型

        va_start(vaList, format);//把可变参数取出
        int m = vsnprintf(buff_.BeginWrite(), buff_.WritableBytes(), format, vaList);//放入真正要写的信息
        va_end(vaList);

        buff_.HasWritten(m);
        buff_.Append("\n\0", 2);

        if(isAsync_ && deque_ && !deque_->Full()) {//如果异步，并且队列不满，就往队列里放,如果满了直接舍弃
            deque_->Push_Back(buff_.RetrieveAllToStr());
        } else if(!isAsync_){
            fputs(buff_.Peek(), fp_);
        }
        buff_.RetrieveAll();
    }

}

void Log::AppendLogLevelTitle_(int level) {//在日志中加入信息的类型
    switch(level) {
    case 0:
        buff_.Append("[debug]: ", 9);
        break;
    case 1:
        buff_.Append("[info] : ", 9);
        break;
    case 2:
        buff_.Append("[warn] : ", 9);
        break;
    case 3:
        buff_.Append("[error]: ", 9);
        break;
    default:
        buff_.Append("[info] : ", 9);
        break;
    }
}

void Log::Flush() {
    if(isAsync_) { 
        deque_->Flush(); //如果是异步，就唤醒一个消费者，如果消费者没有了，那么就没有效果
    }
    fflush(fp_);//刷新缓冲区
}

void Log::AsyncWrite_() {//线程执行的异步写操作
    string str = "";
    while(deque_->Pop(str)) {//从队列中取数据，取不到会阻塞
        //lock_guard<mutex> locker(mtx_);//异步写感觉不需要锁
        fputs(str.c_str(), fp_);//写到文件中
        //fflush(fp_);
    }
}

Log* Log::Instance() {//获取日志单例
    return logptr;
}

void Log::FlushLogThread() {//线程的执行函数
    Log::Instance()->AsyncWrite_();
}