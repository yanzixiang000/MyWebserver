#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <cstdarg>
#include <string.h>
#include <sys/types.h>
#include <dirent.h>

#include <iostream>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <fstream>
#include <locale.h>

#include "../locker/locker.h"
#include "../socket_control/socket_control.h"
#include "../buffer/buffer.h"
#include "../log/log.h"
#include "../sqlconnpool/sqlconnpool.h"
#include "../sqlconnpool/sqlconnRAII.h"


class Http_Conn{
public:
//用状态机来实现，以下是状态

// HTTP请求方法，这里只支持GET
enum METHOD {GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT};
/*
    解析客户端请求时，主状态机的状态
    CHECK_STATE_REQUESTLINE:当前正在分析请求行
    CHECK_STATE_HEADER:当前正在分析头部字段
    CHECK_STATE_CONTENT:当前正在解析请求体
*/
enum CHECK_STATE { CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT };
/*
    服务器处理HTTP请求的可能结果，报文解析的结果
    NO_REQUEST          :   请求不完整，需要继续读取客户数据
    GET_REQUEST         :   表示获得了一个完成的客户请求
    BAD_REQUEST         :   表示客户请求语法错误
    NO_RESOURCE         :   表示服务器没有资源
    FORBIDDEN_REQUEST   :   表示客户对资源没有足够的访问权限
    FILE_REQUEST        :   文件请求,获取文件成功
    INTERNAL_ERROR      :   表示服务器内部错误
    CLOSED_CONNECTION   :   表示客户端已经关闭连接了
*/
enum HTTP_CODE { NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION };
// 从状态机的三种可能状态，即行的读取状态，分别表示
// 1.读取到一个完整的行 2.行出错 3.行数据尚且不完整
enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };

public:
    //由于都共享一个epollfd，所以先弄成静态的
    static int m_epollfd;//所有socket上的事件都被注册到同一个epoll上
    static int m_user_count;//用户数量,用在了监听套接字有连接请求时，判断如果连接过多，就不要了
    //文件名最大长度
    static const int FILENAME_LEN = 1024;
public:
    Http_Conn(){//所有的都默认初始化

    };
    ~Http_Conn(){
    };


public://这些是主线程或者子线程调用的函数，接口函数
    void Process();//这个是任务类中的处理事件，由子线程调用。
    void Init(int sockfd,const sockaddr_in&addr);//虽然创建好了对象，但是里面的sock之类的只有真的有值了才能赋值，所以有个初始化函数
    void Close_Conn();//关闭连接，因为用户数量也要变，所以干脆写在http_conn中,注意在主线程中关闭，所以不需要保护，如果是子线程自己关，需要进行保护
    bool Read(); //非阻塞的读
    bool Write(); //非阻塞的写

private://以下是由外部接口函数调用的函数

    //一些成员变量的初始化，由init调用
    void Clean();

    //解析HTTP请求
    HTTP_CODE Process_Read();
    //以下是该函数需要用到的函数
    //解析分几部分，要能解析是否存在一个完整行，然后解析请求行，解析头部信息，解析请求体
    HTTP_CODE Process_Request_Line(char* text);//解析请求行
    HTTP_CODE Process_Headers(char* text);//解析头部信息
    HTTP_CODE Process_Connect(char* text);//解析请求体
    LINE_STATUS Parse_Line(char* &lineEnd);//解析是否存在一个完整行
    HTTP_CODE Do_Request();//根据获取指令进行对应的操作
    void ParseFromUrlencoded_();//解析登陆和注册输入的消息体的内容
    bool UserVerify(const std::string &name, const std::string &pwd, bool isLogin); //对登陆和注册在一个函数中操作MYSQL，返回成功与否
    void Process_File();//解析文件并保存文件
    void GetFileHtmlPage();
    void GetFileVec(const std::string dirName, std::vector<std::string> &resVec);//获取所有上传到服务器进行保存的文件名
    HTTP_CODE Map(char* file); //把指定的文件进行内存映射
    void unMap();//取消内存映射



    //根据请求结果以及一小部分的响应，去做真正的生成响应，
    //生成响应其实就是往写缓冲里写入响应行和响应头部，至于响应体，就是之前的一小部分的响应来生成的
    bool Process_Write(HTTP_CODE ret);
    //生成响应需要调用的函数
    bool Add_Response(const char* format,...);//往写缓冲中写入数据
    bool Add_Status_Line(int status,const char* title);//写入响应行
    bool Add_Headers(int content_len);//写入响应头
    bool Add_Content_Length(int content_len);//响应头中需要写入的响应体长度
    bool Add_Linger();//响应头中写入是否保持连接
    bool Add_Blank_Line();//写入空行
    bool Add_Content(const char* content);//除了文件以外的如果需要写入其他响应体，用这个函数






private:
    int m_sockfd;//这个任务对应的套接字
    sockaddr_in m_address;//通信的socket地址


    //新的读缓冲区和写缓冲区
    Buffer m_read_buffer;
    Buffer m_write_buffer;

    
    std::string m_url; //请求目标文件的文件名
    char m_real_file[FILENAME_LEN];//真正的要发送的文件路径
    std::string m_version; //协议版本，支持HTTP1.1和1.0
    METHOD m_mehtod; //请求方法

    long m_content_length; //记录消息体长度，http的头部信息中应该要有
    bool m_linger; //判断是否保持连接
    std::string m_content_type;//内容类型
    std::string m_boundary;//post文件时的边界

    //主状态机当前所处的状态
    CHECK_STATE m_check_state;


    struct stat m_file_stat;//客户要获取的文件的状态，用stat查看，并保存在这里
    char* m_file_address;//客户请求的目标文件被mmap到内存中的位置
    
    //writev去发送数据，在需要发送文件的时候，写缓冲中放的响应行和响应头，放在第一个元素中，文件就放在第二个元素中
    //如果不需要发送文件，只有第一个元素放写缓冲中的响应行和响应头，第二个元素用不上
    struct iovec m_iv[2];
    int m_iv_count;


    //因为close时，除了主线程的close，其他情况下线程也会close，为了防止静态变量被多次不正确改变，所以需要用互斥锁
    Locker mutex;
    //因为用了mysql，防止幻读，加个读写锁
    RWlocker rwlock;
    
    int m_bytes_to_send;// 需要发送的字节个数,上限代表是几个G，所以没必要再用long了
    int m_bytes_have_send;    // 已经发送的字节

    bool m_isdownload;//因为发送文件回去时浏览器默认是打开而不是下载，需要添加一个消息头来说明是下载，isdownload为true就添加下载消息头

    std::unordered_map<std::string, std::string> post_;//因为登陆和注册都需要输入用户和密码，就先保存在哈希表中
    


};


#endif