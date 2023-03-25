#include "heaptimer.h"

//上滤操作
void HeapTimer::Siftup_(size_t i) {
    assert(i >= 0 && i < heap_.size());
    size_t j = (i - 1) / 2;
    while(j >= 0) {
        if(heap_[j] <= heap_[i]) { break; }
        SwapNode_(i, j);
        i = j;
        j = (i - 1) / 2;
    }
}

void HeapTimer::SwapNode_(size_t i, size_t j) {
    assert(i >= 0 && i < heap_.size());
    assert(j >= 0 && j < heap_.size());
    std::swap(heap_[i], heap_[j]);
    ref_[heap_[i].id] = i;//i里面放的已经是j的socket了，但是还是对应的j的位置，所以改为i的位置
    ref_[heap_[j].id] = j;
} 

//下滤操作
bool HeapTimer::Siftdown_(size_t index, size_t n) {
    assert(index >= 0 && index < heap_.size());
    assert(n >= 0 && n <= heap_.size());
    size_t i = index;
    size_t j = i * 2 + 1;
    while(j < n) {
        if(j + 1 < n && heap_[j + 1] < heap_[j]) j++;//找两个子节点中的更小值进行下滤
        if(heap_[i] < heap_[j]) break;
        SwapNode_(i, j);
        i = j;
        j = i * 2 + 1;
    }
    return i > index;
}


void HeapTimer::Add(int id, int timeout, void (*TimeoutCallback)(Http_Conn*,int) ) {
    assert(id >= 0);
    size_t i;
    if(ref_.count(id) == 0) {
        /* 新节点：堆尾插入，调整堆 */
        i = heap_.size();
        ref_[id] = i;
        heap_.push_back({id,false, std::chrono::high_resolution_clock::now() + std::chrono::milliseconds(timeout), TimeoutCallback});
        Siftup_(i);
    } 
    else {
        /* 已有结点：调整堆 */
        i = ref_[id];
        heap_[i].expires = std::chrono::high_resolution_clock::now() + std::chrono::milliseconds(timeout);
        heap_[i].TimeoutCallback = TimeoutCallback;
        heap_[i].isHappened = false;
        if(!Siftdown_(i, heap_.size())) {
            Siftup_(i);
        }
    }
}

void HeapTimer::DoWork(Http_Conn* user,int id) {
    /* 删除指定id结点，并触发回调函数 */
    if(heap_.empty() || ref_.count(id) == 0) {
        return;
    }
    size_t i = ref_[id];
    TimerNode node = heap_[i];
    node.TimeoutCallback(user,id);
    Del_(i);
}

void HeapTimer::Del_(size_t index) {
    /* 删除指定位置的结点 */
    assert(!heap_.empty() && index >= 0 && index < heap_.size());
    /* 将要删除的结点换到队尾，然后调整堆 */
    size_t i = index;
    size_t n = heap_.size() - 1;
    assert(i <= n);
    if(i < n) {
        //先交换
        SwapNode_(i, n);
        ///再删除
        ref_.erase(heap_.back().id);
        heap_.pop_back();

        if(!Siftdown_(i, n)) {//再对之前的进行下滤
            Siftup_(i);
        }
    }else{
        //直接删除
        ref_.erase(heap_.back().id);
        heap_.pop_back();
    }

}

void HeapTimer::Adjust(int id, int timeout) {
    /* 调整指定id的结点 */
    assert(!heap_.empty() && ref_.count(id) > 0);
    heap_[ref_[id]].expires = std::chrono::high_resolution_clock::now() + std::chrono::milliseconds(timeout);
    Siftdown_(ref_[id], heap_.size());
} 

void HeapTimer::Tick(Http_Conn* user,int timeout) {
    /* 清除超时结点 */
    if(heap_.empty()) {
        return;
    }
    while(!heap_.empty()) {

        TimerNode node = heap_.front();

        if(std::chrono::duration_cast<std::chrono::milliseconds>(node.expires - std::chrono::high_resolution_clock::now()).count() > 0) { 
            break; //顶点没超时，那么后面的都不会超时
        }

        //如果是超时的定时器
        //判断之前是否发生过事件
        if(node.isHappened == true){//注意node只是一个局部变量，复制品
            Adjust(node.id,timeout);//把套接字对应的定时器加上时间，执行了下滤操作，此时heap_.front已经不是node了
            heap_[ref_[node.id]].isHappened = false;//每次扩展时间之后，都需要重新发生事件，才能再次扩展事件
            continue;
        }

        node.TimeoutCallback(user,node.id);
        Pop();
    }
}

void HeapTimer::Pop() {
    assert(!heap_.empty());
    Del_(0);
}

void HeapTimer::Clear() {
    ref_.clear();
    heap_.clear();
}


int HeapTimer::GetNextTick(Http_Conn* user,int timeout) {
    Tick(user,timeout);//先处理超时的定时器
    size_t res = -1;
    if(!heap_.empty()) {
        res = std::chrono::duration_cast<std::chrono::milliseconds>(heap_.front().expires - std::chrono::high_resolution_clock::now()).count();
        if(res < 0) { res = 0; }//如果因为运行代码导致本来不超时的超时了，先不处理，超时时间为0，就是不阻塞，检查一次epoll_wait就立马返回，让epoll后下一次来处理
    }
    //如果为空，返回-1，epoll_wait会一直等待到有链接，就会有定时器再次加入
    return res;
}


void HeapTimer::Happen(int fd){
    int i = ref_[fd];//先得到发生事件的定时器的位置
    heap_[i].isHappened = true;//标志改为1

}

