// threadpool.h
//
// 线程池：一个可以独立复用的模块，跟"epoll服务器"这个具体业务没有关系，
// 单纯负责"管理一批线程，接收任务，分发给空闲线程去执行"。
//
// 核心设计思路（生产者-消费者模型）：
//   - 有一个"任务队列"（用std::queue实现），存放等待被执行的任务
//   - 主线程（生产者）把任务塞进队列
//   - 线程池里的N个工作线程（消费者）不断地从队列里取任务来执行
//   - 队列空的时候，工作线程不能傻乎乎地一直空转检查（浪费CPU），
//     而是要"睡眠"，等有新任务来了再被"叫醒"——这就是条件变量(condition_variable)的作用

#pragma once  
// #pragma once 是一个"预处理指令"，作用：防止这个头文件被同一个.cpp文件重复包含多次。
// （另一种老写法是用 #ifndef/#define/#endif，效果一样，#pragma once更简洁，现代编译器都支持）

#include <vector>              // std::vector：用来存放所有工作线程对象
#include <queue>               // std::queue：用来实现"任务队列"，先进先出(FIFO)
#include <thread>              // std::thread：C++11标准线程类，一个对象对应一个真实的操作系统线程
#include <mutex>               // std::mutex：互斥锁，保护"任务队列"不被多个线程同时读写而出错
#include <condition_variable>  // std::condition_variable：条件变量，用于线程间的"等待/唤醒"通知机制
#include <functional>          // std::function：可以存放"任何可调用对象"的通用容器，下面会详细解释
#include <future>              // std::future：用于获取异步任务的返回值（这个项目里用得比较简单）

class ThreadPool {
public:
    // 构造函数：创建线程池的时候，立刻启动 thread_count 个工作线程，让它们待命
    // 参数 thread_count：要创建多少个工作线程，通常设置为CPU核心数（可以用 std::thread::hardware_concurrency() 查询）
    explicit ThreadPool(size_t thread_count);    // explicit 关键字：禁止编译器做"隐式类型转换"。

    ~ThreadPool();      // 析构函数：线程池对象被销毁时自动调用

    //禁用拷贝构造和拷贝赋值。线程池管理着底层的系统线程和锁，这些资源是“不可复制”的。强行复制会导致两个线程池对象试图控制同一批物理线程，引发灾难。
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // enqueue: 把一个任务塞进任务队列，让某个工作线程将来去执行它
    //class F:表示"可调用对象"的类型,可以是普通函数、lambda、函数对象等,class... Args:这是可变参数模板,接受任意个数、任意类型的模板参数,比如 0 个、1 个、5 个都行。
    //F&& f:用万能引用(forwarding reference)接收那个可调用对象 f(也就是你要执行的函数),Args&&... args:同样是万能引用,接收对应数量的实参,这些会作为 f 的参数传进去。
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<typename std::result_of<F(Args...)>::type>;

private:
    // worker函数：每个工作线程实际在跑的"主循环"逻辑
    // 注意这不是给外部调用的，是线程池内部自己用的，所以放在private区域
    void workerLoop();

    std::vector<std::thread> workers_;          // 存放所有工作线程对象的容器
    std::queue<std::function<void()>> tasks_;   // 任务队列
    // std::function<void()> 的含义：这是一个"通用函数容器"，可以装下
    // "任何不需要参数、也不需要返回值"的可调用对象（函数指针/lambda/仿函数都行）。
    // 我们把每个任务都统一包装成这种形式存进队列，工作线程取出来直接调用 task() 执行即可。

    std::mutex queue_mutex_;             // 专门保护 tasks_ 这个队列的互斥锁
    std::condition_variable condition_;  // 配合互斥锁使用的条件变量，用于"等待新任务/通知有新任务"
    bool stop_;                          // 标记线程池是否要关闭，true时所有工作线程会陆续退出循环
};

// ---------------------------------------------------------------------------
// 下面是函数的具体实现。C++模板函数的实现习惯上要写在头文件里（不能像普通函数那样
// 声明放.h、实现放.cpp分开），原因是模板要等"真正被使用的时候"编译器才知道具体类型，
// 必须能在用到它的地方看到完整代码，所以连实现一起放在.h文件中。
// ---------------------------------------------------------------------------

// 构造函数实现：创建thread_count个线程，每个线程一启动就跑 workerLoop() 这个循环
inline ThreadPool::ThreadPool(size_t thread_count) : stop_(false)       //: stop_(false) 是成员初始化列表，在对象正式构造的时候,直接给成员变量赋初始值。
{
    // [this] { this->workerLoop(); } 是一个"lambda表达式"（匿名函数）：
    //   [this]  捕获列表，表示这个lambda内部可以访问当前ThreadPool对象的成员（比如workerLoop函数）
    //   {...}   函数体，这里就是去调用 workerLoop() 这个无限循环函数
    // std::thread的构造函数接收一个"可调用对象"作为参数，传进去之后这个线程就会立刻开始执行它
    for (size_t i = 0; i < thread_count; ++i) {
        workers_.emplace_back([this] { this->workerLoop(); });
    }
}

// 工作线程的主循环：每个工作线程从创建开始就一直在执行这个函数，直到stop_变成true且队列为空
inline void ThreadPool::workerLoop() {
    while (true) {
        std::function<void()> task; // 准备一个"容器"，待会儿用来装从队列里取出来的任务

        {
            // std::unique_lock 是一种"会自动管理锁"的封装类：
            //   构造的时候自动加锁(lock)，这个对象生命周期结束（比如离开{}作用域）的时候自动解锁(unlock)
            // 比手动写 lock()/unlock() 更安全，不会因为中途某处忘记解锁、或者抛异常导致锁一直没释放
            std::unique_lock<std::mutex> lock(queue_mutex_);

            // condition_.wait(lock, 判断条件函数):
            // 执行到 wait 这行
            //     ↓
            // 先调用 lambda 检查一次条件
            //     ↓
            // 条件已经满足（队列有任务 or 已经stop）
            //     → 不睡，直接继续往下执行（锁还在手里）
             
            // 条件不满足（队列是空的 且 没有stop）
            //     → 释放锁，睡着
            //     → 等别人调用 notify 把自己敲醒
            //     → 醒来后重新拿锁，再检查一次条件
            //     → 条件满足了 → 继续往下执行
            //     → 条件还不满足（虚假唤醒）→ 再释放锁睡回去
            condition_.wait(lock, [this] { return stop_ || !tasks_.empty(); });

            // 醒过来后，如果是"线程池要关闭 并且 队列已经空了"，这个工作线程就该结束自己的生命了
            if (stop_ && tasks_.empty()) {
                return; // 直接return，跳出整个workerLoop函数，这个线程的任务就算执行完了，线程即将退出
            }

            // 走到这里说明队列里确实有任务，取出队首的任务
            task = std::move(tasks_.front());
            // std::move 把tasks_.front()这个任务"移动"给task（而不是拷贝），
            // 对于std::function这种可能内部持有较多资源的对象，移动比拷贝更高效
            tasks_.pop(); // 从队列里移除刚才取走的这个任务
        }
        // 这个{}作用域结束，lock对象被销毁，自动释放了queue_mutex_这把锁——
        // 这一点很关键：真正执行task()的时候，锁已经被释放了，不会让"执行任务"这个耗时操作
        // 一直占着锁，导致其他线程没法去操作队列，造成不必要的等待。

        task(); // 真正执行这个任务（前面enqueue塞进来的，是什么内容这里完全不关心，调用它就行）
    }
}

// enqueue函数的实现
template<class F, class... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args) -> std::future<typename std::result_of<F(Args...)>::type> {        //传入左值 → F&& 自动变成 F&（左值引用） 传入右值 → F&& 自动变成 F&&（右值引用）
                                                                                                          
    using return_type = typename std::result_of<F(Args...)>::type;      //using xxx = 某个类型 是起别名

    auto task = std::make_shared<std::packaged_task<return_type()>>(      //智能指针
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)       //std::bind 的作用是把函数和它的参数"绑"在一起，生成一个新的可调用对象：    
    );                                                                  // std::forward 就是"保持原来左值/右值属性不变地传递下去"

    std::future<return_type> result = task->get_future();
    // get_future() 拿到一张"提货单"，将来调用 result.get() 就能取到task执行完的返回值

    {
        std::unique_lock<std::mutex> lock(queue_mutex_); // 操作队列前先加锁，防止多线程同时读写队列出错

        if (stop_) {
            // 如果线程池已经在关闭过程中了，不应该再接收新任务，直接抛异常提醒调用者
            throw std::runtime_error("enqueue on stopped ThreadPool");
        }

        // 把task包装成 std::function<void()> 的形式存进队列
        // （因为tasks_队列里存的统一是"无参数无返回值"的形式，具体返回值通过上面的future机制单独获取）
        tasks_.emplace([task] { (*task)(); });
    }

    condition_.notify_one();
    // notify_one()：随机唤醒一个正在 condition_.wait() 处睡眠等待的工作线程，
    // 让它醒过来检查"队列里是不是有新任务了"，然后去把刚塞进去的任务取走执行

    return result; // 把"提货单"返回给调用者
}

// 析构函数实现：线程池对象生命周期结束时（比如main函数返回前），自动调用，负责优雅关闭
inline ThreadPool::~ThreadPool() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        stop_ = true; // 标记"要关闭了"
    }

    condition_.notify_all();
    // notify_all()：唤醒所有正在睡眠等待的工作线程（不止一个），
    // 让它们都醒过来检查条件，发现stop_为true且队列已空，就会各自return退出

    for (std::thread &worker : workers_) {
        worker.join();
        // join()：让"主线程"在这里等待，直到这个工作线程真正执行完毕、彻底退出为止。
        // 必须等所有工作线程都干净地退出后，ThreadPool对象才能真正被销毁，
        // 否则可能出现"线程还在跑，但它依赖的ThreadPool对象已经被释放"的严重错误（悬空引用）。
    }
}