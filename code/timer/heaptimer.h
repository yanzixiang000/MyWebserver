#ifndef HEAP_TIMER_H
#define HEAP_TIMER_H

#include <queue>
#include <unordered_map>
#include <time.h>
#include <algorithm>
#include <arpa/inet.h> 
#include <functional> 
#include <assert.h> 
#include <chrono>
#include "../http/http_conn.h"

//时间节点
struct TimerNode {
    int id;//定时器对应的套接字
    bool isHappened;//用于标注定时器在当前时间段是否发生过事件
    std::chrono::high_resolution_clock::time_point expires;//高精度时间
    void (*TimeoutCallback)(Http_Conn*,int);
    //TimeoutCallBack cb;
    bool operator<(const TimerNode& t) {
        return expires < t.expires;
    }
    bool operator<=(const TimerNode& t) {
        return expires <= t.expires;
    }
};

//时间堆
class HeapTimer {
public:
    HeapTimer() { heap_.reserve(10000); }

    ~HeapTimer() { Clear(); }
    
    void Adjust(int id, int newExpires);

    void Add(int id, int timeOut, void (*TimeoutCallback)(Http_Conn*,int) );

    void DoWork(Http_Conn* user,int id);

    void Clear();

    void Tick(Http_Conn* user,int timeout);

    void Pop();

    int GetNextTick(Http_Conn* user,int timeout);

    void Happen(int fd);

private:
    void Del_(size_t i);
    
    void Siftup_(size_t i);

    bool Siftdown_(size_t index, size_t n);

    void SwapNode_(size_t i, size_t j);

    std::vector<TimerNode> heap_;

    std::unordered_map<int, size_t> ref_;//用于记录每个socket对应的定时器在vector中的位置，以便于能直接找到
};

#endif //HEAP_TIMER_H

/*
改进的时间堆思路

都是从最小的时间算超时时间，给epoll当作超时时间（不用信号了），如果这期间有事件发生，就在对应定时器中记录发生过事件，（之前是发生过事件就直接重置时间并下滤，对于高并发而言不友好）
等事件处理完后，或者epoll_wait超时之后，再次取超时时间之前，需要检查定时器超时的，如果没发生过事件，就去掉，发生过事件，就重置时间和事件，并保留。这里是要清理时再检查需不需要延长时间
然后再次取超时时间，放入epoll_wait ，不断循环

如果定时器还没有到时间时，套接字就被关闭了，时间堆中的定时器先不处理，等到了时间就会自动处理掉，如果处理掉之前就又来一个相同的套接字，那么add对于相同套接字的添加会修改时间

//整个时间堆，被外界调用的就三个函数，第一是获取下一次等待时间（同时清理或处理超时的定时器），第二个是有新的连接到来添加定时器，第三就是如果发生了事件，在定时器进行标注的函数，用来处理超时定时器时扩展时间


//其实也可也用时间链来实现，
//如果就针对断开的非活跃连接，并且每次延长时间相同的话，时间链可以延长时间调整定时器只需要直接放到最后即可。加入时间链也可以直接放到最后。即针对特殊情况时间复杂度都是O1
//但是如果是非特殊情况，那效率和时间堆比就会差很多

*/