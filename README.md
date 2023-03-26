# MyWebserver
基于Linux下C++实现的高性能、高并发的WEB文件管理服务器，通过浏览器发送HTTP请求管理服务器指定文件夹下的所有文件，经过webbenchh压力测试可以实现上万的QPS

## 功能
- 可以通过注册登陆进入文件管理界面
- 以HTML页面形式显示可以操作的所有文件
- 可以选择本地文件上传到服务器
- 可以对列表中的文件执行下载操作
- 可以删除服务器中的指定文件

## 技术架构
* 采用**模拟Proactor事件处理模型**，主线程利用Epoll边缘触发的IO复用技术进行监听和输入输出，工作线程负责执行业务逻辑，比Reactor事件处理模型**QPS提升50%**
* 实现**线程池**预先创建线程，减少频繁创建和销毁线程的开销，使用**轮询算法**将任务派发给线程的工作队列，实现负载均衡
* 实现**数据库连接池**，减少数据库连接建立与关闭的开销，采取**RAII机制**实现数据库连接池资源的获取和释放，实现了用户**注册登录**功能
* 利用**有限状态机**解析HTTP请求报文，实现处理静态资源的请求，支持**GET、POST请求**，实现**文件的上传，下载，删除**操作
* 实现基于小根堆的**改进时间堆**，解决高并发下频繁调整定时器导致的效率下降，用于关闭超时的非活动连接
* 实现**同步/异步日志系统**，利用单例模式生成日志系统，记录服务器运行状态
* 利用标准库容器封装char，实现**自动增长的缓冲区**

## 环境要求
* Linux
* C++
* MySql

## 项目启动
需要先配置好对应的数据库
```bash
// 建立yourdb库
create database yourdb;

// 创建user表
USE yourdb;
CREATE TABLE user(
    username varchar(50) NOT NULL,
    password varchar(50) NOT NULL
)ENGINE=InnoDB;

// 添加数据
INSERT INTO user(username, password) VALUES('name', 'password');
```

```bash
//编译
make
//执行
./bin/webserver port
```

## 压力测试
```bash
./webbench-1.5/webbench -c 10000 -t 5 http://ip:port/
```

## TODO
* main.cpp采用面向对象
* 自动增长的缓冲区内部实现改用string
* 读取配置文件

