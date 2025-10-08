# Epoll + Coroutine Minimal

一个使用 **C++20 协程** 的最小化网络服务器示例，基于 `epoll` 实现异步 I/O。  
支持简单的 TCP 连接、协程式读写，并可扩展为 HTTP 或 Echo 服务器。


## 特性

- 使用 **C++20 协程** 实现异步任务。
- 基于 **epoll** 的高效 I/O 多路复用。
- 简单抽象：提供 `read_awaiter`、`write_awaiter` 和 `accept_awaiter`。
- 支持多客户端并发，无需每个连接创建线程。
- 内置示例 HTTP 和 Echo 服务器。
- 非阻塞套接字，协程安全挂起与恢复。


