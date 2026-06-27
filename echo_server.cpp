// step1: 单线程 echo 服务器
// 功能：接受一个客户端连接，把收到的数据原样发回去
// 这一步的目的：熟悉 socket / bind / listen / accept / read / write 这几个最基础的系统调用
#include <iostream>
#include <cstring>      // memset
#include <unistd.h>     // close, read, write
#include <arpa/inet.h>  // sockaddr_in, htons, INADDR_ANY
#include <sys/socket.h>
#include <cstdio>

const int PORT = 8080;
const int BUFFER_SIZE = 1024;

int main() {
    // 1. 创建监听socket
    // AF_INET: IPv4   SOCK_STREAM: TCP
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0); //IPV4,TCP,自动选择
    if (listen_fd == -1) {
        perror("socket failed");
        return 1;
    }

    // 设置端口复用：服务器重启后能立刻重新绑定同一个端口，不用等之前的连接超时释放
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 2. 绑定地址和端口
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY; // 监听本机所有网卡
    addr.sin_port = htons(PORT);       // htons: 主机字节序转网络字节序

    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) == -1) {
        perror("bind failed");
        close(listen_fd);
        return 1;
    }

    // 3. 开始监听，backlog设为5（等待accept的连接队列长度）
    if (listen(listen_fd, 5) == -1) {
        perror("listen failed");
        close(listen_fd);
        return 1;
    }

    std::cout << "Echo server listening on port " << PORT << " ..." << std::endl;

    // 4. 主循环：每次接受一个连接，处理完再接受下一个（注意：这是单线程版本，
    //    同一时刻只能服务一个客户端，这正是后面要用epoll/多线程解决的问题）
    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(listen_fd, (sockaddr*)&client_addr, &client_len);
        if (client_fd == -1) {
            perror("accept failed");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        std::cout << "Client connected: " << client_ip << ":" << ntohs(client_addr.sin_port) << std::endl;

        // 5. 循环读取这个客户端发来的数据，原样发回去，直到客户端断开连接
        char buffer[BUFFER_SIZE];
        while (true) {
            memset(buffer, 0, BUFFER_SIZE);
            ssize_t n = read(client_fd, buffer, BUFFER_SIZE - 1);

            if (n > 0) {
                std::cout << "Received: " << buffer;
                write(client_fd, buffer, n); // 原样写回去
            } else if (n == 0) {
                std::cout << "Client disconnected." << std::endl;
                break;
            } else {
                perror("read failed");
                break;
            }
        }

        close(client_fd);
    }

    close(listen_fd);
    return 0;
}