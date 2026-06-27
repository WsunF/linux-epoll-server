# Linux Epoll Server

基于 Linux 系统编程实现的高性能网络服务器，从零开始逐步迭代：从最基础的阻塞式 TCP 服务器，到 I/O 多路复用（epoll），再到多线程并发处理。

这个项目是我系统学习 Linux C++ 服务端开发的实践记录，目标是深入理解高并发服务器底层原理（而不是直接套用现成框架），每一步迭代都经过手动测试验证。

## 技术栈

- **语言**：C++
- **核心技术**：Socket 编程、I/O 多路复用（epoll）、非阻塞 I/O、多线程（开发中）
- **平台**：Linux（WSL2 / Ubuntu）
- **构建工具**：g++

## 项目结构

```
.
├── step1_echo/
│   └── echo_server.cpp      # 单线程阻塞式 TCP echo 服务器
├── step2_epoll/
│   └── epoll_server.cpp     # 引入 epoll，单线程处理多个并发连接
└── README.md
```

## 已完成功能

### Step 1：单线程 TCP Echo 服务器
使用 `socket` / `bind` / `listen` / `accept` / `read` / `write` 实现最基础的回显服务器。

**局限**：同一时刻只能处理一个客户端连接，新连接必须等当前连接结束才能被接受。

### Step 2：引入 epoll，支持并发连接
将 socket 设置为非阻塞模式，使用 `epoll_create1` / `epoll_ctl` / `epoll_wait` 实现 I/O 多路复用，单线程即可同时监控并处理多个客户端连接的读写事件。

**核心改进**：
- 监听 socket 和客户端 socket 均设置为非阻塞模式
- `accept` 采用循环接收，避免漏接同一轮到达的多个新连接
- 正确处理连接断开（`read` 返回 0）及异常情况，避免文件描述符泄漏

**已验证**：多个客户端可同时建立连接、独立收发数据，互不阻塞（对比 Step 1 的阻塞问题）。

## 开发中 / 后续计划

- [ ] 线程池：将 I/O 事件检测（epoll）与具体业务处理解耦，避免单个慢请求拖慢整体响应（Reactor 模式）
- [ ] HTTP 协议解析：支持解析 GET/POST 请求，从 echo 服务器升级为 Web 服务器
- [ ] 定时器：处理超时连接，释放无效资源
- [ ] 日志系统：记录服务器运行状态
- [ ] 性能压测：使用 webbench/ab 进行并发压力测试，给出具体 QPS 数据

## 如何编译运行

每个步骤目录下都是独立可编译的单文件程序，例如运行 Step 2：

```bash
cd step2_epoll
g++ -Wall -o epoll_server epoll_server.cpp
./epoll_server
```

服务器默认监听 `8080` 端口，可用 `nc 127.0.0.1 8080` 或 `telnet 127.0.0.1 8080` 进行连接测试。

## 学习参考

- 游双《Linux高性能服务器编程》
- man 7 epoll