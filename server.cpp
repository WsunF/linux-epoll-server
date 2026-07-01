// step3: 在 epoll 基础上引入线程池
//
// 跟 step2 最大的区别：
//   step2 里，epoll发现某个fd可读之后，直接在主线程里原地read/write处理完，
//          如果这次处理很耗时，会卡住主线程，导致它没法及时去处理其他客户端的事件。
//   step3 里，epoll主线程只负责"发现事件"，把具体的"读数据+处理+写回去"这个任务，
//          打包丢进线程池，由线程池里的某个空闲工作线程去执行，主线程立刻就能
//          回到epoll_wait继续监听其他事件，不会被某一个慢任务拖住。
//
// 新增的关键概念：EPOLLONESHOT
//   如果不用这个机制，可能出现"同一个客户端连接的事件被通知了两次，
//   导致线程池里两个不同的线程同时在读写同一个socket"，引发数据错乱的问题。
//   EPOLLONESHOT保证：一个fd的事件只会被通知一次，处理完之后必须手动"重新武装"它，
//   epoll才会继续通知它后续的事件，从而保证同一个连接的处理永远是串行、不冲突的。

#include <iostream>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include "threadpool.h"   // 引入我们自己写的线程池

const int PORT = 8080;
const int BUFFER_SIZE = 1024;
const int MAX_EVENTS = 1024;
const int THREAD_COUNT = 4;   // 线程池里开几个工作线程，这里先固定写4，实际项目中常用CPU核心数

int epoll_fd; // 设为全局变量，方便在被线程池调用的处理函数里也能访问到同一个epoll实例

void setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// ---------------------------------------------------------------------------
// handleClient: 真正处理"某个客户端fd可读"这件事的函数。
// 这个函数会被打包成任务丢进线程池，由工作线程调用执行（不是在epoll主线程里跑）。
//
// 参数 fd：哪个客户端socket需要被处理
// 返回值：void（没有返回值，处理结果直接通过read/write完成，不需要返回给调用者）
// ---------------------------------------------------------------------------
void handleClient(int fd) {
    char buffer[BUFFER_SIZE];
    bool should_close = false; // 标记这次处理完后，这个连接是否应该被关闭

    memset(buffer, 0, BUFFER_SIZE);
    ssize_t count = read(fd, buffer, BUFFER_SIZE - 1);

    if (count > 0) {
        std::cout << "[线程 " << std::this_thread::get_id() << "] fd=" << fd
                  << " received: " << buffer;
        write(fd, buffer, count); // echo回去
    } else if (count == 0) {
        std::cout << "fd=" << fd << " disconnected." << std::endl;
        should_close = true;
    } else {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("read failed");
            should_close = true;
        }
    }

    if (should_close) {
        // 连接要关闭了：从epoll里彻底摘除这个fd，并真正close它
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
    } else {
        // 连接还要继续用：因为用了EPOLLONESHOT，必须在这里"重新武装"这个fd，
        // 否则epoll以后再也不会通知这个fd的事件了（它处于"一次性触发已消耗"的状态）
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLONESHOT; // 注意这里要再次带上EPOLLONESHOT，不然下一次又变回普通模式
        ev.data.fd = fd;
        // EPOLL_CTL_MOD：修改一个已经注册过的fd的监控设置（不是新增，所以用MOD不是ADD）
        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
    }
}

int main() {
    // ====================== 创建监听socket（跟之前完全一样） ======================
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        perror("socket failed");
        return 1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;           // 地址类型（IPv4 ）
    addr.sin_addr.s_addr = INADDR_ANY;   // IP 地址
    addr.sin_port = htons(PORT);         // 端口号

    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) == -1) {
        perror("bind failed");
        close(listen_fd);
        return 1;
    }

    if (listen(listen_fd, 5) == -1) {
        perror("listen failed");
        close(listen_fd);
        return 1;
    }

    setNonBlocking(listen_fd);

    std::cout << "Epoll + ThreadPool echo server listening on port " << PORT << " ..." << std::endl;

    // ====================== 创建epoll实例 ======================
    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("epoll_create1 failed");
        close(listen_fd);
        return 1;
    }

    epoll_event ev{};
    ev.events = EPOLLIN; // 注意：监听socket本身不需要加EPOLLONESHOT，
                          // 它的"可读"事件代表"有新连接"，这是个持续会发生的事，不需要一次性触发限制
    ev.data.fd = listen_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) == -1) {
        perror("epoll_ctl: listen_fd failed");
        close(listen_fd);
        close(epoll_fd);
        return 1;
    }

    epoll_event events[MAX_EVENTS];

    // ====================== 创建线程池 ======================
    ThreadPool pool(THREAD_COUNT);
    // 这一行执行完，4个工作线程就已经在后台待命了（具体看threadpool.h里的构造函数实现）

    // ====================== 事件循环：主线程只做"发现事件+分发任务"这两件事 ======================
    while (true) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);   //n = 发生事件的数量
        if (n == -1) {
            perror("epoll_wait failed");
            break;
        }

        for (int i = 0; i < n; ++i) {
            int current_fd = events[i].data.fd;

            if (current_fd == listen_fd) {
                // 新连接到来，这部分逻辑很轻量（只是accept+注册），
                // 不涉及耗时操作，所以仍然留在epoll主线程里直接做，不需要丢给线程池
                while (true) {
                    sockaddr_in client_addr{};
                    socklen_t client_len = sizeof(client_addr);
                    //accept接收到的客户端IP和端口填入结构体client_addr，返回client_fd
                    int client_fd = accept(listen_fd, (sockaddr*)&client_addr, &client_len);   

                    if (client_fd == -1) {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) {
                            perror("accept failed");
                        }
                        break;
                    }

                    char client_ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
                    std::cout << "New client connected: " << client_ip << ":"
                              << ntohs(client_addr.sin_port) << " (fd=" << client_fd << ")" << std::endl;

                    setNonBlocking(client_fd);

                    epoll_event client_ev{};
                    // 客户端连接的事件要加上EPOLLONESHOT，理由见文件开头的说明
                    client_ev.events = EPOLLIN | EPOLLONESHOT;
                    client_ev.data.fd = client_fd;
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_ev);
                }
            } else {
                // 客户端fd就绪：不在主线程里直接处理，而是把"处理这个fd"打包成一个任务，
                // 丢进线程池，让某个空闲的工作线程去异步执行，主线程立刻就能回去处理下一个事件
                pool.enqueue(handleClient, current_fd);
                // enqueue的用法回顾：第一个参数是要执行的函数，后面跟的是这个函数需要的参数，
                // 这里等价于线程池内部某个工作线程将来会去调用 handleClient(current_fd);
            }
        }
    }

    close(listen_fd);
    close(epoll_fd);
    return 0;
}