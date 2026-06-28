// step2: 引入 epoll，单线程同时处理多个连接
//
// 跟 step1 最大的区别：
//   step1 用 accept() 卡住等一个客户端，处理完它才能接待下一个客户端
//   step2 用 epoll 同时"盯着"监听socket + 所有已连接的客户端socket，
//         谁有数据来就处理谁，不会被某一个慢客户端卡住整个程序
//
// 注意：这一步还是单线程，所有客户端的读写仍然是依次处理的，
//      只是"等待"这个动作变成了同时等待多个fd，而不是死等accept一个连接。
//      真正的"并行处理"要靠下一步的线程池实现。

#include <iostream>
#include <cstring>      // memset
#include <unistd.h>     // close, read, write
#include <fcntl.h>       // fcntl, 设置非阻塞模式要用到
#include <arpa/inet.h>   // sockaddr_in, htons, inet_ntop
#include <sys/epoll.h>   // epoll_create1, epoll_ctl, epoll_wait
#include <vector>

const int PORT = 8080;
const int BUFFER_SIZE = 1024;
const int MAX_EVENTS = 1024;   // epoll_wait 一次最多返回多少个就绪事件，开大一点没坏处

// ---------------------------------------------------------------------------
// 工具函数：把一个文件描述符(fd)设置成"非阻塞"模式
//
// 为什么需要非阻塞？
//   step1里 read()/accept() 如果没数据/没连接，会一直卡住不返回（"阻塞"）。
//   但在epoll模型里，我们是靠epoll_wait告诉我们"这个fd现在确实有数据可读了"，
//   如果还用阻塞模式，万一判断有误差导致read卡住，会拖死整个单线程的事件循环，
//   后面所有其他客户端都得排队等着。所以epoll搭配的socket，规矩是必须设成非阻塞。
// ---------------------------------------------------------------------------
void setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);      // 先读出这个fd当前的flag设置，fcnt1读取功能
    fcntl(fd, F_SETFL, flags | O_NONBLOCK); // 在原有flag基础上，加上"非阻塞"这一位,fcnt1设置功能
}

int main() {
    // ====================== 第一部分：创建监听socket（跟step1完全一样） ======================
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);//IPV4,TCP
    if (listen_fd == -1) {
        perror("socket failed");
        return 1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));//端口复用

    sockaddr_in addr{};     //端口地址
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

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

    // 新增：监听socket本身也要设成非阻塞
    // 原因：等下accept的时候，我们会"循环accept直到没有新连接为止"（后面会解释为什么要循环），
    //      如果listen_fd是阻塞的，最后一次没有连接可接的accept调用会卡死整个程序。
    setNonBlocking(listen_fd);

    std::cout << "Epoll echo server listening on port " << PORT << " ..." << std::endl;

    // ====================== 第二部分：创建epoll实例 ======================

    // epoll_create1(0): 创建一个epoll实例，返回一个"epoll专用的文件描述符"epoll_fd。
    // 你可以把epoll_fd理解成一个"监控面板"，接下来要监控哪些socket，都注册到这个面板上。
    int epoll_fd = epoll_create1(0);    //传入 0 表示默认行为,成功返回一个全新的非负文件描述符（指向内核的 epoll 实例）；失败返回 -1
    if (epoll_fd == -1) {
        perror("epoll_create1 failed");
        close(listen_fd);
        return 1;
    }

    // epoll_event 就是自定义数据类型（结构体），ev就是单个实例
    //   events 字段：关心的事件类型，比如 EPOLLIN 表示"这个fd上有数据可读了"
    //   data    字段：一个联合体，最常用 data.fd 存"这是哪个fd"，
    //                 这样epoll_wait返回的时候，我们才知道是哪个socket就绪了
    epoll_event ev{};
    ev.events = EPOLLIN;      // EPOLLIN（有数据可读/有新连接）、EPOLLOUT（缓冲区空闲可写）、EPOLLET（边缘触发模式）。
    ev.data.fd = listen_fd;

    // epoll_ctl: 向epoll面板"注册/修改/删除"一个fd的监控
    //   EPOLL_CTL_ADD 表示"新增一个监控对象",EPOLL_CTL_ADD（添加节点）、EPOLL_CTL_MOD（修改节点事件）、EPOLL_CTL_DEL（删除节点）。
    // 这一步把监听socket本身也交给epoll盯着——
    // 当有新客户端发起连接时，listen_fd会变成"可读"，epoll_wait就会通知我们。
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) == -1) {
        perror("epoll_ctl: listen_fd failed");
        close(listen_fd);
        close(epoll_fd);
        return 1;
    }

    // events数组：epoll_wait每次返回时，会把"就绪的fd列表"填到这个数组里，，events就是数组，装的都是ev
    epoll_event events[MAX_EVENTS];

    char buffer[BUFFER_SIZE];

    // ====================== 第三部分：事件循环（整个程序的核心） ======================
    while (true) {
        // epoll_wait: 程序会在这里"睡眠等待"，直到至少有一个被监控的fd就绪
        //  返回值n是"这次有多少个fd同时就绪了，参数-1 表示永久阻塞直到有事件发生；0 表示非阻塞立即返回；>0 表示阻塞等待的毫秒数
        // 这一步是整个epoll模型唯一会阻塞的地方，而且它能同时等待成百上千个fd，
        // 这就是为什么epoll能用一个线程支撑大量并发连接。
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (n == -1) {
            perror("epoll_wait failed");
            break;
        }

        // 遍历这一轮所有就绪的fd，逐个处理
        for (int i = 0; i < n; ++i) {
            int current_fd = events[i].data.fd;

            // ---------- 情况A：是监听socket就绪 → 说明有新客户端要连接进来 ----------
            if (current_fd == listen_fd) 
            {
                while (true) 
                {
                    //有listen_fd触发通过accept分配client_fd
                    sockaddr_in client_addr{};
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = accept(listen_fd, (sockaddr*)&client_addr, &client_len);

                    // EAGAIN / EWOULDBLOCK 表示：当前没有更多待接的连接了，是正常情况，跳出循环即可
                    // 其他错误才需要打印出来排查
                    if (client_fd == -1) {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) {
                            perror("accept failed");
                        }
                        break;
                    }

                    char client_ip[INET_ADDRSTRLEN];//定义一个长度为16 个字节的数组     宏定义：INET_ADDRSTRLEN   16
                    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));    //翻译二进制网络数据
                    std::cout << "New client connected: " << client_ip << ":"
                              << ntohs(client_addr.sin_port) << " (fd=" << client_fd << ")" << std::endl;

                    // 新连接的socket也必须设成非阻塞，理由跟listen_fd一样
                    setNonBlocking(client_fd);

                    // 把这个新客户端的fd也注册进epoll，让epoll以后也帮我们盯着它
                    epoll_event client_ev{};
                    client_ev.events = EPOLLIN;   
                    client_ev.data.fd = client_fd;
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_ev);
                }
            }
            // ---------- 情况B：是某个客户端socket就绪 → 说明它发数据来了，或者它断开了 ----------
            else {
                memset(buffer, 0, BUFFER_SIZE);
                ssize_t count = read(current_fd, buffer, BUFFER_SIZE - 1);  //返回实际读取字节数，0：客户端关闭，-1：错误

                if (count > 0) {
                    // 正常收到数据，原样回写给这个客户端（echo）
                    std::cout << "fd=" << current_fd << " received: " << buffer;
                    write(current_fd, buffer, count);
                } else if (count == 0) {
                    // read返回0，是TCP的约定含义："对方已经正常关闭了连接"
                    std::cout << "fd=" << current_fd << " disconnected." << std::endl;
                    // 必须把这个fd从epoll里摘掉，并且close它，否则会变成"幽灵fd"一直占着资源
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, current_fd, nullptr);
                    close(current_fd);
                } else {
                    // count == -1，说明读取出错（不是EAGAIN这种正常情况，是真的出错了）
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        perror("read failed");
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, current_fd, nullptr);
                        close(current_fd);
                    }
                    // 如果是EAGAIN/EWOULDBLOCK，说明这次其实没真正有数据（LT模式下偶尔会有这种空通知），
                    // 不用管，直接跳过，等下一轮epoll_wait即可
                }
            }
        }
    }

    close(listen_fd);
    close(epoll_fd);
    return 0;
}