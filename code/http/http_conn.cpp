#include "http_conn.h"

int Http_Conn::m_epollfd = -1;
int Http_Conn::m_user_count = 0;


// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "400,Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "403,You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "404,The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "500,There was an unusual problem serving the requested file.\n";


//解析出一行时需要用到的字符串
const char CRLF[] = "\r\n";

//---------------------------------------

//主线程调用
void Http_Conn::Init(int sockfd,const sockaddr_in& addr){
    m_sockfd = sockfd;
    m_address = addr;
    //设置一个端口复用，调试的时候用，实际使用不需要用
    int reuse =1;
    setsockopt(sockfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    //在任务内部里把sockfd加入epollfd中
    Addfd(m_epollfd,sockfd,true,true);
    mutex.Lock();
    ++m_user_count;
    mutex.unLock();

    m_read_buffer.RetrieveAll();
    //读缓存只用初始化即可，为了解决粘包问题，不需要读完后清空，其实写缓存也不需要，但为了实现简单，就清理了

    //除了上面的套接字的初始化，还有任务类内部的初始化,这个初始化分开写的原因在于，后面可能还需要用到
    Clean();
    LOG_INFO("Client[%d] in!", m_sockfd);

}
void Http_Conn::Clean(){

    m_check_state = CHECK_STATE_REQUESTLINE;

    m_url.clear();
    m_version.clear();
    m_content_type.clear();
    m_boundary.clear();
    m_mehtod = GET;
    m_content_length = 0; 
    m_linger =false;
    m_bytes_to_send=0;
    m_bytes_have_send =0;
    m_write_buffer.RetrieveAll();
    memset(m_real_file,'\0',FILENAME_LEN);
    m_isdownload = false;
    
}


void Http_Conn::Close_Conn(){
    if(m_sockfd!=-1){
        LOG_INFO("Client[%d] quit!", m_sockfd);
        Removefd(m_epollfd,m_sockfd);
        close(m_sockfd);
        m_sockfd=-1;
        mutex.Lock();
        --m_user_count;//总的连接数量减一
        mutex.unLock();
    }
}

//循环读取客户内容，直到无可读，或者对方关闭连接
bool Http_Conn::Read(){
    int saveErrno =0;
    while(1){
        ssize_t bytes_read = m_read_buffer.ReadFd(m_sockfd,&saveErrno);
        if(bytes_read< 0){
            if(saveErrno==EAGAIN || saveErrno == EWOULDBLOCK){
                break;//代表ET情况下读完了，就停止读取
            }
            LOG_ERROR("read() error");
            return false;
        }else if(bytes_read == 0){
            return false;//读到关闭连接，直接return false,主线程也会关闭连接
        }
    }
    return true;
}

//写函数，由主线程调用，当process_write生成响应完成后，主线程调用write写出去
bool Http_Conn::Write(){//返回true就不关闭连接，返回false关闭连接
    if ( m_bytes_to_send == 0 ) {
        // 将要发送的字节为0，这一次响应结束。
        Modfd( m_epollfd, m_sockfd, EPOLLIN ); 
        Clean();
        return true;
    }
    while(1) {
        // 一起写
        int temp = writev(m_sockfd, m_iv, m_iv_count);
        if ( temp <= -1 ) {
            // EAGAIN 或 EWOULDBLOCK，表示缓冲区已满
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if( errno == EAGAIN ) {
                Modfd( m_epollfd, m_sockfd, EPOLLOUT );
                return true;
            }
            LOG_ERROR("writev() error");
            unMap();
            return false;
        }
        //如果写成功一部分，记录还需要写多少
        m_bytes_to_send -= temp;
        m_bytes_have_send += temp;
        //如果写完了
        if ( m_bytes_to_send <= 0 ) {
            // 发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否立即关闭连接
            unMap();
            if(m_linger) {//如果要求继续连接
                Clean();
                Modfd( m_epollfd, m_sockfd, EPOLLIN );
                return true;
            } else {
                Modfd( m_epollfd, m_sockfd, EPOLLIN );
                return false;
            } 
        }
        //注意因为用的不是写缓存中的发送，所以写缓存中的读指针始终不变，而写指针因为已经不再往写缓存里写，所以也位置不变
        //如果没写完,循环下次发送的时候，需要把发送内容修改一下，已发过的就不要再发了
        if(m_bytes_have_send>=m_write_buffer.ReadableBytes())
        //但是写缓冲区写完了,总体没写完但写缓冲区写完说明一定发送的文件，就需要把文件已发送的部分也去掉
        {
            m_iv[0].iov_len=0;
            m_iv[1].iov_base = m_file_address + (m_bytes_have_send - m_write_buffer.ReadableBytes());
            m_iv[1].iov_len = m_bytes_to_send;
        }else{//如果写缓冲区没写完，那就不好判断到底有没有文件，不过也不需要涉及到文件了，只用修改第一个元素即可
            m_iv[0].iov_base = m_write_buffer.Peek()+ m_bytes_have_send;
            m_iv[0].iov_len=m_iv[0].iov_len - temp;  
        }
    }
}




//------------------------------------------------------------------------------

//子线程调用的任务
void Http_Conn::Process(){//proactor模式下，是把任务中读到的数据进行解析，然后决定发送什么数据，并注册可写，等可写时主线程就会写出去
    //把读缓冲区的东西拿出来，解析http请求,解析结束后会有一个返回值，是解析后的结果
    HTTP_CODE read_ret =  Process_Read();
    if(read_ret == NO_REQUEST){//说明不完整，需要继续读，而继续读需要重新oneshot
        Modfd(m_epollfd,m_sockfd,EPOLLIN);
        return;
    }
    //如果完整，就需要响应，通过返回的解析结果判断是回复正确信息还是回复错误信息
    bool write_ret = Process_Write(read_ret);//要返回生成的响应是否成功
    if(!write_ret){//如果不成功，就关闭连接
        LOG_ERROR("process_write() error");
        Close_Conn();
        return;
    }
    //如果生成响应成功，就需要写
    Modfd(m_epollfd,m_sockfd,EPOLLOUT);
    return;
    
}



//------------------------------------------------------------------------------
//任务里的解析函数
Http_Conn::HTTP_CODE Http_Conn::Process_Read(){
    LINE_STATUS line_status = LINE_OK;
    char* lineEnd =nullptr;
    
    while( ((m_check_state==CHECK_STATE_CONTENT)&& line_status ==LINE_OK )  || ((line_status = Parse_Line(lineEnd))==LINE_OK)   )
    //不是请求体时必须要有一行完整数据才能去解析数据，否则结束循环，会返回NO_REQUEST
    //如果需要判断请求体，请求体中不再是一行行的，所以如果状态为请求体，并且请求体之前判断的line_status=LINE_OK，也就能继续进行
    {
        //解析到完整的一行,从peek开始，不用取，直接用
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
            {
                HTTP_CODE RET = Process_Request_Line(m_read_buffer.Peek());
                if(RET==BAD_REQUEST){//如果语法错误，就结束解析，否则其他都是no——request，要继续解析
                    return BAD_REQUEST;
                }
                break;
            }
        case CHECK_STATE_HEADER:
            {
                HTTP_CODE RET = Process_Headers(m_read_buffer.Peek());
                if(RET == BAD_REQUEST){
                    return BAD_REQUEST;
                }else if(RET == GET_REQUEST){
                    //GET_REQUEST表示获得了一个完成的客户请求，在里面已经确认没有消息体
                    //当遇到空行并且没有消息体时进入这里，会处理后返回，如果不移动buffer的指针，就会出问题
                    m_read_buffer.RetrieveUntil(lineEnd + 2);//使读指针指向下一个请求行的头部。
                    return Do_Request();
                    //do_request是针对命令做一些事情的函数
                }
                break;
            }
        case CHECK_STATE_CONTENT:
            {
                HTTP_CODE RET = Process_Connect(m_read_buffer.Peek());
                if(RET== GET_REQUEST){//这里是代表有消息体的http请求完全获得了
                    return Do_Request();
                    //从这里调用的一定是POST的处理
                }
                line_status = LINE_OPEN;//如果RET不是GET_REQUEST，就说明没有完全获得，就说明没读完，以结束当前循环，就改状态，并返回NO_REQUEST
                break;
            }
        default:{
            return INTERNAL_ERROR;
            }
        }
        if(line_status == LINE_OPEN){

            break;
        }//发现消息体不完全，就不执行下面这个，而是继续去读

        //一个完整行解析结束后，需要调整缓冲区的起始位置
        //空指针但有消息体时，进入这里，使读指针指向消息体的第一个字符
        m_read_buffer.RetrieveUntil(lineEnd + 2);

    }
    return NO_REQUEST;//即信息不完整
}


//解析分几部分，要能解析是否存在一个完整行，然后解析请求行，解析头部信息，解析请求体
//解析结束后还有针对解析的结构进行一小部分回应，真正具体写操作在后面的函数
Http_Conn::LINE_STATUS Http_Conn::Parse_Line(char* & lineEnd)//注意这里必须是指针引用才行，否则没法把指针的值传出去
//解析是否存在一个完整行,因为用的search
//在消息体中结束不是\r\n,所以这个函数不能用在消息体中
{   //找\r\n
    lineEnd = std::search(m_read_buffer.Peek(), m_read_buffer.BeginWrite(), CRLF, CRLF + 2);
    if(lineEnd == m_read_buffer.BeginWrite()){
        return LINE_OPEN;
    }
    //否则说明找到了\r\n，修改一下
    *lineEnd = '\0';
    *(lineEnd+1) = '\0';
    return LINE_OK;
    
}
//解析请求行，就三个内容，花去请求方法，目标URL，以及HTTP版本号
Http_Conn::HTTP_CODE Http_Conn::Process_Request_Line(char* text)
{
    //GET /index.html HTTP/1.1
    //依次检验字符串 str1 中的字符，当被检验字符在字符串 str2 中也包含时，则停止检验，并返回该字符位置,这里检查的是空格和tab符
    char* url_start = strpbrk(text," \t");
    if(!url_start){
        //为了让后面比较版本的时候不出现段错误，所以要给版本赋个值
        m_version = std::string("HTTP/1.1");
        return BAD_REQUEST;
    }
    *url_start++='\0';
    //strcasecmp用忽略大小写比较字符串
    if(strcasecmp(text,"GET") == 0 ){//text加了空字符，所以只剩命令
        m_mehtod = GET;
    }
    else if(strcasecmp(text,"POST") == 0 ){
        m_mehtod = POST;
    }
    else{
        //否则就是不支持的命令，但是为了让后面比较版本的时候不出现段错误，所以要给版本赋个值
        m_version = std::string("HTTP/1.1");
        return BAD_REQUEST;
    }

    char* version_start = strpbrk(url_start," \t");
    if(!version_start){
        //为了让后面比较版本的时候不出现段错误，所以要给版本赋个值
        m_version = std::string("HTTP/1.1");
        return BAD_REQUEST;
    }

    *version_start++='\0';//m_version直接指向写缓存里的地址
    m_version = std::string(version_start);
    if(strcasecmp(m_version.c_str(),"HTTP/1.1") != 0 && strcasecmp(m_version.c_str(),"HTTP/1.0") != 0 ){
        return BAD_REQUEST;
    }

    m_url = std::string(url_start);
    //因为有的可能是http://192.168.xxx.xxx/index.html,如果前面是http的形式
    if(strncasecmp(m_url.c_str(),"http://",7) ==0){
        m_url = std::string(m_url.c_str()+7);
        //在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置
        //m_url也直接指向读缓存的某个地址,是文件路径的第一个/号
        m_url = std::string(strchr(m_url.c_str(),'/'));
    }
    //如果就是普通情况，那么m_url就应该指向了文件路径的第一个/号，
    //如果不是正常的文件路径，m_url可能空或者不是\，也是BAD_REQUEST
    if(m_url.size()==0 || m_url[0] !='/'){
        return BAD_REQUEST;
    }
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析头部信息
Http_Conn::HTTP_CODE Http_Conn::Process_Headers(char* text)
{
    // 遇到空行，表示头部字段解析完毕，\r被换成了'\0'
    if( text[0] == '\0' ) {
        // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，
        // 状态机转移到CHECK_STATE_CONTENT状态
        if ( m_content_length != 0 ) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    } else if ( strncasecmp( text, "Connection:", 11 ) == 0 ) {
        // 处理Connection 头部字段  Connection: keep-alive
        text += 11;
        text += strspn( text, " \t" );//检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标。
        if ( strcasecmp( text, "keep-alive" ) == 0 ) {
            m_linger = true;
        }
    } else if ( strncasecmp( text, "Content-Length:", 15 ) == 0 ) {
        // 处理Content-Length头部字段
        text += 15;
        text += strspn( text, " \t" );
        m_content_length = atol(text);
    }else if( strncasecmp( text, "Content-Type:", 13 ) == 0){
        //发现只有POST才有Content-Type
        text += 13;
        text += strspn( text, " \t" );
        char* boundary = strchr(text,';');
        if(boundary==nullptr){//没有分号说明是登陆注册的内容
            m_content_type = std::string(text);
        }else{
            *boundary='\0';
            m_content_type = std::string(text);
            boundary+=11;
            m_boundary = std::string(boundary);
        }
    }else {
        //如果是其他头部字段，先不管
    }
    return NO_REQUEST;
}
//解析请求体,这里并没有真正解析，只是判断是否完整读入，真正的解析再do_request中
Http_Conn::HTTP_CODE Http_Conn::Process_Connect(char* text)
{
    if ( m_read_buffer.BeginWrite() >= ( m_content_length + m_read_buffer.Peek() ))
    //因为到了内容的时候已经不是一行解析一次了，Peek还停留在上一行末尾的下一个字节，即内容体的第一个字节
    //其实BeginWrite如果大于，就说明出现粘包问题，所以这里不会全部清空，而是读指针移动到下一个内容的初始位置，让后面会在写指针的位置继续写，注意，如果需要扩展
    //首选是会考虑把读指针到写指针的那部分全部移动到头部，所以也就是说，读缓存根本不用清空，会自动调整
    {
        //读完整了就可以处理了
        return GET_REQUEST;
    }
    //没读完整需要再读
    return NO_REQUEST;
}


//针对解析结果进行回应，但只是一部分，需要进行文件内存映射的都用这个函数处理内存映射，剩余的在process_write里放到写缓存中
//文件这个不需要放到写缓存中，分开发送
Http_Conn::HTTP_CODE Http_Conn::Do_Request(){
    if(m_mehtod==GET){
        if(strncasecmp( m_url.c_str(), "/download_", 10 ) == 0){//如果是下载
            m_url[9]='/';//先把_换成/
            m_isdownload = true;//下载标志设为真
            int utf8start = m_url.find('%');
            //有%就认为有汉字的编码,就要把utf8编码转换为汉字
            if(utf8start != -1){
                std::string message = std::string("./filedir") +m_url.substr(9,utf8start-9);
                std::vector<unsigned char> utf8str;
                int n = m_url.size();
                for(int i=utf8start;i<n;++i){//把两个字符一组按照16进制解析成一个long型，再变成unsigned char，然后每三个unsigned char就得到了字符代表的汉字
                    if(m_url[i]=='%'){//代表接下来是%EA这种编码,但注意空格特殊，是%20，只有一个编码，汉字都是三个编码的组合
                        if(m_url[i+1]=='2' && m_url[i+2]=='0'){
                            message = message +' ';
                            i+=2;
                            continue;
                        }
                        utf8str.push_back((unsigned char)strtol(std::string(m_url.c_str()+i+1,m_url.c_str()+i+3).c_str(),nullptr,16));
                        if(utf8str.size()==3){
                            utf8str.push_back('\0');
                            message = message + (char*)&utf8str[0];
                            utf8str.clear();
                        }
                        i+=2;
                    }else{
                        message = message + m_url[i];
                    }
                }
                return Map(const_cast<char*>(message.c_str()));
            }else{//没有汉字，就正常给文件即可
                std::string message = "./filedir" + m_url.substr(9,m_url.size()-9);
                return Map(const_cast<char*>(message.c_str()));
            }
        }else if(strncasecmp( m_url.c_str(), "/delete_", 8 ) == 0){//如果是删除
            m_url[7] = '/';//先把_换成/
            //也要关注有汉字或者没有汉字
            int utf8start = m_url.find('%');
            if(utf8start != -1){
                //有%就认为有汉字的编码,就要把utf8编码转换为汉字
                std::string message = std::string("./filedir") +m_url.substr(7,utf8start-7);
                std::vector<unsigned char> utf8str;
                int n = m_url.size();
                for(int i=utf8start;i<n;++i){//把两个字符一组按照16进制解析成一个long型，再变成unsigned char，然后每三个unsigned char就得到了字符代表的汉字
                    if(m_url[i]=='%'){//代表接下来是%EA这种编码
                        if(m_url[i+1]=='2' && m_url[i+2]=='0'){
                            message = message +' ';
                            i+=2;
                            continue;
                        }
                        utf8str.push_back((unsigned char)strtol(std::string(m_url.c_str()+i+1,m_url.c_str()+i+3).c_str(),nullptr,16));
                        if(utf8str.size()==3){
                            utf8str.push_back('\0');
                            message = message + (char*)&utf8str[0];
                            utf8str.clear();
                        }
                        i+=2;
                    }else{
                        message = message + m_url[i];
                    }
                }
                remove(message.c_str());//删除文件
                //返回剩余文件组成的文件列表网页
                GetFileHtmlPage();
                char megret[] = {"./resources/file.html"};
                return Map(megret);
            }else{//没有汉字，就正常删除文件即可
                std::string message = "./filedir" + m_url.substr(7,m_url.size()-7);
                remove(message.c_str());//删除文件
                //返回剩余文件组成的文件列表网页
                GetFileHtmlPage();
                char megret[] = {"./resources/file.html"};
                return Map(megret);
            }
        }else{//是正常的网页申请
            //直接请求的error.html不允许
            if(strcasecmp(m_url.c_str(),"/error.html")==0){
                return NO_RESOURCE;
            }
            //并且不能直接进入文件页面，必须先登陆
            if(strcasecmp(m_url.c_str(),"/")==0 || strcasecmp(m_url.c_str(),"/filelist.html")==0  || 
            strcasecmp(m_url.c_str(),"/file.html")==0 || strcasecmp(m_url.c_str(),"/fileitem.html")==0){//如果是根目录，也返回登陆页面
                char message[] = {"./resources/login.html"};
                return Map(message);//就返回登陆页面
            }
            //正常的登陆或者注册页面的申请
            std::string message = "./resources" + m_url;
            return Map(const_cast<char*>(message.c_str()));

        }
    }else if(m_mehtod==POST){
        if(strcasecmp(m_url.c_str(),"/upload")!=0){//如果不是上传，必然是登陆或者注册
            ParseFromUrlencoded_();//解析用户名和密码
            //解析完消息体之后需要把读指针挪到下一个请求行的头部
            m_read_buffer.RetrieveUntil(m_content_length + m_read_buffer.Peek());
            bool islogin = true;//默认为登陆
            if(strcasecmp(m_url.c_str(),"/register.html") == 0)//再一次比较文件名，判断是登陆还是注册
            {
                islogin = false;
            }
            //根据用户名，密码，去登陆或者注册mysql
            if(UserVerify(post_["username"],post_["password"], islogin)){
                //如果成功
                if(islogin){//登陆成功
                    //生成要返回的真正文件,并进行内存映射
                    GetFileHtmlPage();
                    char message[] = {"./resources/file.html"};
                    return Map(message);
                }else{//注册成功
                    char message[] = {"./resources/login.html"};
                    return Map(message);//就返回登陆页面
                }
            }else{
                char message[] = {"./resources/error.html"};
                return Map(message);//两种失败都返回错误界面
            }
        }else{//否则说明是文件上传
            //保存文件
            Process_File();
            //解析完文件后要到下个请求行的的头部
            m_read_buffer.RetrieveUntil(m_content_length + m_read_buffer.Peek());
            //生成要返回的真正文件,并进行内存映射
            GetFileHtmlPage();
            char message[] = {"./resources/file.html"};
            return Map(message);
        }
    }else{
        return  BAD_REQUEST;
    }
}
//对登陆和注册的用户名密码进行解析
void Http_Conn::ParseFromUrlencoded_() {
    std::string key, value;
    int n = m_content_length;
    int i = 0, j = 0;
    char* bodystart = m_read_buffer.Peek();

    for(; i < n; i++) {
        char ch = bodystart[i];
        switch (ch) {
        case '=':
            key = std::string(bodystart+j,bodystart+i);
            j = i + 1;
            break;
        case '+':
            bodystart[i] = ' ';
            break;
        case '&':
            value = std::string(bodystart+j,bodystart+i);
            j = i + 1;
            post_[key] = value;
            LOG_DEBUG("%s = %s", key.c_str(), value.c_str());
            break;
        default:
            break;
        }
    }
    assert(j <= i);
    if(post_.count(key) == 0 && j < i) {//前面&那个是放用户名，这里是放入密码
        value = std::string(bodystart+j,bodystart+i);
        post_[key] = value;
    }
}
//对登陆和注册在一个函数中操作，返回成功与否
bool Http_Conn::UserVerify(const std::string &name, const std::string &pwd, bool isLogin) {
    if(name == "" || pwd == "") { return false;}
    LOG_INFO("Verify name:%s pwd:%s", name.c_str(), pwd.c_str());
    MYSQL* sql;
    SqlConnRAII raii(&sql,  SqlConnPool::Instance());//从sql连接池中拿出来一个sql连接使用,析构后会自动放回去
    assert(sql);
    
    bool flag = false;
    char order[256] = { 0 };
    MYSQL_RES *res = nullptr;
    //如果是注册，先标志为true
    if(!isLogin) { flag = true; }
    //先生成查询语句，根据用户名去查询对应的用户名和密码
    snprintf(order, 256, "SELECT username, password FROM user WHERE username='%s' LIMIT 1", name.c_str());//order 及查询语句
    LOG_DEBUG("%s", order);
    //mysql_query函数查询，如果查询失败，返回false，找到值和没找到值都算查询成功
    if(mysql_query(sql, order)) {
        mysql_free_result(res);
        return false; 
    }
    res = mysql_store_result(sql);//再把结束放入res
    //从结果集中取得一行数据返回
    //进入循环说明存在对应的用户名，如果是登陆，密码正确就返回true，否则返回false，如果是注册，就不能注册了，也要返回false
    //取不出来返回null，说明不存在这样的用户，如果是登陆，跳过循环以及注册，直接会返回false，如果是注册就要去注册，成功返回true，失败返回false;
    while(MYSQL_ROW row = mysql_fetch_row(res)) {
        LOG_DEBUG("MYSQL ROW: %s %s", row[0], row[1]);
        std::string password(row[1]);
        if(isLogin) {//如果是登陆，判断密码是否正确
            if(pwd == password) { flag = true; }
            else {
                flag = false;
                LOG_DEBUG("pwd error!");
            }
        } 
        else { 
            flag = false;//说明要注册但是已经有用户了，所以是false 
            LOG_DEBUG("user used!");
        }
    }
    mysql_free_result(res);
    /* 注册行为 且 用户名未被使用*/
    //之前只有用户名找不到，无法进入循环，才能进入这里，否则进不去，也就注册不了
    if(!isLogin && flag == true) {
        LOG_DEBUG("regirster!");
        bzero(order, 256);
        snprintf(order, 256,"INSERT INTO user(username, password) VALUES('%s','%s')", name.c_str(), pwd.c_str());
        LOG_DEBUG( "%s", order);
        if(mysql_query(sql, order)) { 
            LOG_DEBUG( "Insert error!");
            flag = false; //注册失败
        }
        flag = true;//注册成功
    }
    LOG_DEBUG( "UserVerify success!!");
    return flag;
}

//解析消息体中的文件，并保存文件
void Http_Conn::Process_File(){
    //跳过文件的开始行
    char* body = m_read_buffer.Peek();
    body = strchr(body,'\r');
    if(!body)return;
    body+=2;//跳过文件的开始行，指向文件属性第一行
    //从属性行里找到文件名
    while(1){
        body = strchr(body,';');
        if(!body)break;
        body+=2;
        if(strncasecmp(body,"filename",8)==0 ){
            break;
        }
    }
    if(!body)return;//如果是空，说明有问题，直接返回即可
    body = strchr(body,'\"');
    if(!body)return;
    ++body;
    char* name_end = strchr(body,'\"');
    if(!name_end)return;
    *name_end = '\0';
    std::string file_name  = std::string(body);//复制出文件名
    
    if(file_name=="")return;
    //如果文件名为空，直接return
    body = name_end+3;//指向文件属性的第二行
    body = strchr(body,'\r');
    if(!body)return;
    body+=4;//跳过空行，指向文件正文的第一个
    if(*body =='\r')return;
    //打开文件
    std::ofstream ofs("./filedir/" + file_name, std::ios::out  | std::ios::binary);//不要加 std::ios::app，追加模式如果多次传同一个文件，会被保存在同一个文件中
    //知道文件内容结束后增加的长度
    long end_length = m_boundary.size()+8;//文件结束行长度再加上上一行的\r\n,这个文件内容最后一行的\r\n是自动添加的，不能算文件内容
    //找到文件结束的下一个字符
    char* body_end = m_read_buffer.Peek()+m_content_length-end_length;//指向文件结束后自动添加的\r
    long segment_length = body_end-body;
    ofs.write(body,segment_length);
    ofs.close();
}

// 以下两个函数用来构建文件列表的页面，最终结果生成file文件保存
void Http_Conn::GetFileHtmlPage(){

    // 将指定目录内的所有文件保存到 fileVec 中
    std::vector<std::string> fileVec;
    GetFileVec("./filedir", fileVec);
    std::ifstream fileListStream(std::string("./resources")+"/filelist.html", std::ios::in);
    std::ofstream ofs(std::string("./resources")+"/file.html", std::ios::out  | std::ios::binary);
    std::string tempLine;
    // 首先读取文件列表的 <!--filelist_label--> 注释前的语句,放入新的文件中
    while(1){
        getline(fileListStream, tempLine);
        if(tempLine == "<!--filelist_label-->"){
            break;
        }
        ofs << tempLine << "\n";
    }
    // 根据如下标签，将将文件夹中的所有文件项添加到返回页面中
    //             <tr><td class="col1">filename</td> <td class="col2"><a href="download_filename">下载</a></td> <td class="col3"><a href="delete_filename">删除</a></td></tr>
    for(const auto &filename : fileVec){
        ofs << "            <tr><td class=\"col1\">" + filename +
                    "</td> <td class=\"col2\"><a href=\"download_" + filename +
                    "\">下载</a></td> <td class=\"col3\"><a href=\"delete_" + filename +
                    "\" onclick=\"return confirmDelete();\">删除</a></td></tr>" + "\n";
    }
    // 将文件列表注释后的语句加入后面
    while(getline(fileListStream, tempLine)){
        ofs << tempLine + "\n";
    }
    
}
void Http_Conn::GetFileVec(const std::string dirName, std::vector<std::string> &resVec){
    // 使用 dirent 获取文件目录下的所有文件
    DIR *dir;   // 目录指针
    dir = opendir(dirName.c_str());
    struct dirent *stdinfo;
    while (1)
    {
        // 获取文件夹中的一个文件
        stdinfo = readdir(dir);
        if (stdinfo == nullptr){
            break;
        }
        if(strcasecmp(stdinfo->d_name,".") != 0  && strcasecmp(stdinfo->d_name,"..") != 0 )resVec.push_back(stdinfo->d_name);
    }
}

//把指定的文件夹里的文件进行内存映射
Http_Conn::HTTP_CODE Http_Conn::Map(char* file){
        strcpy( m_real_file, file );

        // 获取m_real_file文件的相关的状态信息，-1失败，0成功
        if ( stat( m_real_file, &m_file_stat ) < 0 ) {
            return NO_RESOURCE;
        }
        // 判断访问权限
        if ( ! ( m_file_stat.st_mode & S_IROTH ) ) {
            return FORBIDDEN_REQUEST;
        }
        // 判断是否是目录
        if ( S_ISDIR( m_file_stat.st_mode ) ) {
            return BAD_REQUEST;
        }

        // 以只读方式打开文件
        int fd = open( m_real_file, O_RDONLY );
        // 创建内存映射
        m_file_address = ( char* )mmap( NULL, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
        close( fd );
        return FILE_REQUEST;
}

// 对内存映射区执行munmap操作
void Http_Conn::unMap() {
    if( m_file_address )
    {
        munmap( m_file_address, m_file_stat.st_size );
        m_file_address = 0;
    }
}



//-------------------------------------------------------------------------------------
//以上对http请求的解析以及一小部分响应已经做完
//接下来该根据解析的结果，决定写什么，并放入写缓存，然后等待可写，然后发送出去
//根据请求结果以及一小部分的响应，去做真正的生成响应，
//生成响应其实就是往写缓冲里写入响应行和响应头部，至于响应体，就是之前的一小部分的响应来生成的
bool Http_Conn::Process_Write(HTTP_CODE ret){
    switch (ret)
    {
        case INTERNAL_ERROR:
            Add_Status_Line( 500, error_500_title );
            Add_Headers( strlen( error_500_form ));
            if ( ! Add_Content( error_500_form ) ) {
                return false;
            }
            m_bytes_to_send = m_write_buffer.ReadableBytes();//因为响应体不是文件而是字符串时，也在写缓存中
            break;
        case BAD_REQUEST:
            Add_Status_Line( 400, error_400_title );
            Add_Headers( strlen( error_400_form ));
            if ( ! Add_Content( error_400_form ) ) {
                return false;
            }
            m_bytes_to_send = m_write_buffer.ReadableBytes();
            break;
        case NO_RESOURCE:
            Add_Status_Line( 404, error_404_title );
            Add_Headers( strlen( error_404_form ));
            if ( ! Add_Content( error_404_form ) ) {
                return false;
            }
            m_bytes_to_send = m_write_buffer.ReadableBytes();
            break;
        case FORBIDDEN_REQUEST:
            Add_Status_Line( 403, error_403_title );
            Add_Headers(strlen( error_403_form));
            if ( ! Add_Content( error_403_form ) ) {
                return false;
            }
            m_bytes_to_send = m_write_buffer.ReadableBytes();
            break;
        case FILE_REQUEST:
            Add_Status_Line(200, ok_200_title );
            Add_Headers(m_file_stat.st_size);
            m_iv[ 0 ].iov_base = m_write_buffer.Peek();
            m_iv[ 0 ].iov_len = m_write_buffer.ReadableBytes();
            m_iv[ 1 ].iov_base = m_file_address;
            m_iv[ 1 ].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            //需要发送的总字节数
            m_bytes_to_send = m_write_buffer.ReadableBytes() + m_file_stat.st_size;
            return true;
        default:
            return false;
    }

    m_iv[ 0 ].iov_base = m_write_buffer.Peek();
    m_iv[ 0 ].iov_len = m_write_buffer.ReadableBytes();
    m_iv_count = 1;
    return true;
}
//生成响应需要调用的函数
bool Http_Conn::Add_Response(const char* format,...)//往写缓冲中写入数据
//因为用了C语言的可变参数，format是可变参数的格式，后面是可变参数
{
    char write[1024];
    va_list arg_list;
    va_start( arg_list, format );//把format格式的参数都放入arg_list
    int len = vsnprintf( write, 1024, format, arg_list );//再把这些放入临时缓存
    if( len > 1024) {
        return false;
    }
    //再放入写缓存
    m_write_buffer.Append(write,strlen(write));//就加入strlen(write)长度的即可，不要加\0
    va_end( arg_list );
    return true;
}

bool Http_Conn::Add_Status_Line(int status,const char* title)//写入响应行
{
    if(strcasecmp(m_version.c_str(),"HTTP/1.0") == 0){
        return Add_Response( "%s %d %s\r\n", "HTTP/1.0", status, title );
    }
    return Add_Response( "%s %d %s\r\n", "HTTP/1.1", status, title );//默认为1.1
    

}
bool Http_Conn::Add_Headers(int content_len)//写入响应头
{
    if(m_isdownload){
        Add_Response( "Content-Disposition: attachment\r\n");//添加这个字段，可以决定客户端的下载还是直接显示
    }
    //注意如果文件类型和真正类型不一致，会导致客户端的错误
    return Add_Content_Length(content_len) && Add_Linger() &&  Add_Blank_Line();

}
bool Http_Conn::Add_Content_Length(int content_len)//响应头中需要写入的响应体长度
{
    return Add_Response( "Content-Length: %d\r\n", content_len );
}
bool Http_Conn::Add_Linger()//响应头中写入是否保持连接
{
    return Add_Response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}

bool Http_Conn::Add_Blank_Line()//写入空行
{
    return Add_Response( "%s", "\r\n" );
}
bool Http_Conn::Add_Content(const char* content)//除了文件以外的如果需要写入其他字符型响应体，用这个函数加到写缓存中
{
    return Add_Response( "%s", content );
}


//-----------------------------以上生成响应完成