#include <arpa/inet.h>
#include <array>
#include <bitset>
#include <cassert>
#include <cerrno>
#include <concepts>
#include <coroutine>
#include <csignal>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <source_location>
#include <span>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <variant>
void assert_error(bool condition,
                  std::source_location const location = std::source_location::current());

static void log(std::source_location const location = std::source_location::current()) {
    if (errno == 0) {
        std::cout << "[INFO] " << location.file_name() << ":" << location.line() << " ("
                  << location.function_name() << "): " << std::endl;

        return;
    }
    std::cerr << "Error at " << location.file_name() << ":" << location.line() << " in function "
              << location.function_name() << std::endl;
    std::cerr << "Error: " << strerror(errno) << " errno: " << errno << std::endl;
}

void make_socket_non_blocking(int sfd);

bool g_stop = false;
void sig_ctrl_c(int);

void sig_ctrl_c(int) {
    g_stop = true;
}

int create_listen_socket(unsigned short port = 8080);

int create_epoll(int listen_fd);

void handle_new_connection(int epoll_fd, int listen_fd);

void handle_client_io(int fd);

void event_loop(int epoll_fd, int listen_fd);

struct final_awaiter {
    bool await_ready() const noexcept {
        return false;
    }

    template <typename Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> coro) noexcept {
        if (coro.promise().caller_) {
            return coro.promise().caller_;
        }
        return std::noop_coroutine();
    }

    void await_resume() noexcept {}
};

template <class T>
struct task_coro;

template <class T>
struct promise_base {
    T value_;
    std::exception_ptr exception_;
    std::coroutine_handle<> caller_ = nullptr;
    task_coro<T> get_return_object();

    std::suspend_always initial_suspend() {
        return {};
    }

    final_awaiter final_suspend() noexcept {
        return {};
    }

    void unhandled_exception() {
        exception_ = std::current_exception();
    }

    template <typename U>
        requires std::convertible_to<U, T>
    void return_value(U &&value) {
        if (exception_) {
            std::cerr << "Exception is not expected here!" << __LINE__ << std::endl;
            std::rethrow_exception(exception_);
        }
        value_ = std::forward<U>(value);
    }
};

template <class T>
struct task_coro {
    using promise_type = promise_base<T>;
    using handle_type = std::coroutine_handle<promise_type>;
    handle_type coro_;

    task_coro(handle_type h) : coro_(h) {}

    ~task_coro() {
        if (coro_) {
            coro_.destroy();
        }
    }

    auto operator co_await() {
        struct save_caller_awaiter : std::suspend_always {
            promise_type &promise_;

            save_caller_awaiter(promise_type &p) : promise_(p) {}

            std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
                promise_.caller_ = caller;
                return std::coroutine_handle<promise_type>::from_promise(promise_);
            }

            auto await_resume() {
                if (promise_.exception_) {
                    std::rethrow_exception(promise_.exception_);
                }
                return promise_.value_;
            }
        };

        return save_caller_awaiter{coro_.promise()};
    }

    T get() {
        if (coro_.promise().exception_) {
            std::rethrow_exception(coro_.promise().exception_);
        }
        return coro_.promise().value_;
    }
};

template <class T>
task_coro<T> promise_base<T>::get_return_object() {
    return task_coro<T>{std::coroutine_handle<promise_base>::from_promise(*this)};
}

task_coro<int> example_coro() {
    std::cout << "In coroutine, before co_await\n";
    std::cout << "In coroutine, after co_await\n";
    co_return 42;
}

task_coro<int> hello_coro() {
    std::cout << "Hello from coroutine!" << std::endl;
    auto r = co_await example_coro();
    std::cout << "Back to hello_coro: " << r << std::endl;
    co_return 0;
}

void test_base_task() {
    auto r = hello_coro();
    if (!r.coro_.done()) {
        r.coro_.resume();
    }
    while (!g_stop) {
        // epoll runtime
    }
    std::cout << "Coroutine returned: " << r.get() << std::endl;
}

struct AcceptData {
    int fd;
    std::expected<int, std::error_code> &ret; // 使用 std::error_code
};

struct ReadData {
    int fd;
    std::span<char> buffer;
    std::expected<size_t, std::error_code> &ret; // 使用 std::error_code
};

struct WriteData {
    int fd;
    std::span<char const> buffer;
    std::expected<size_t, std::error_code> &ret; // 使用 std::error_code
};

using ResumeVariant = std::variant<AcceptData, ReadData, WriteData>;

struct EpollRuntime {
private:
    int epoll_fd = -1;

    EpollRuntime() {
        epoll_fd = epoll_create1(0);
        assert_error(epoll_fd != -1);
    }

public:
    static auto &ins() {
        static EpollRuntime e;
        return e;
    }

    struct resume_data {
        ResumeVariant var;
        std::coroutine_handle<> handle;
    };

    static constexpr int MAX_FDS = 65536; // 假设 fd 不超过这个数
    std::bitset<MAX_FDS> fd_registered;
    std::array<uint32_t, MAX_FDS> fd_events{0};

    void add_watch(ResumeVariant var, std::coroutine_handle<> handle) {
        int fd = std::visit([](auto &v) { return v.fd; }, var);
        auto *p = new resume_data{std::move(var), handle};

        epoll_event ev{};
        ev.data.ptr = p;

        uint32_t new_events = 0;
        std::visit(
            [&](auto &v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, AcceptData> || std::is_same_v<T, ReadData>) {
                    new_events = EPOLLIN | EPOLLRDHUP;
                } else if constexpr (std::is_same_v<T, WriteData>) {
                    new_events = EPOLLOUT | EPOLLRDHUP;
                }
            },
            p->var);

        if (fd_registered.test(fd)) {
            // 已注册，MOD 时保留之前的事件
            ev.events = fd_events[fd] | new_events;
            if (::epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) == -1) {
                perror("epoll_ctl MOD failed");
            } else {
                fd_events[fd] = ev.events;
            }
        } else {
            ev.events = new_events;
            if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
                perror("epoll_ctl ADD failed");
            } else {
                fd_registered.set(fd);
                fd_events[fd] = ev.events;
            }
        }
    }

    static int const MAX_EVENTS = 10;

    static inline epoll_event events[MAX_EVENTS]{};

    void do_once() {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, 10000);
        if (n == -1) {
            if (errno == EINTR) {
                // 被信号打断，继续循环即可
                log();
                return;
            }
            perror("epoll_wait failed");
            throw std::runtime_error("epoll_wait error");
        }

        if (n == 0) {
            // 超时，没有事件发生
            log();
            std::cout << "Epoll wait timed out" << std::endl;
            return;
        }

        for (int i = 0; i < n; i++) {
            auto *p = static_cast<resume_data *>(events[i].data.ptr);
            bool ready_to_resume = false;

            std::visit(
                [&](auto &data) {
                    using T = std::decay_t<decltype(data)>;
                    int fd = data.fd;

                    if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                        std::cerr << "fd=" << fd << " peer closed / error\n";
                        if constexpr (std::is_same_v<T, AcceptData>) {
                            data.ret = std::unexpected(
                                std::make_error_code(std::errc::connection_aborted));
                        } else {
                            data.ret =
                                std::unexpected(std::make_error_code(std::errc::connection_reset));
                        }
                        close(fd);
                        ready_to_resume = true; // error -> resume
                        return;
                    }

                    if constexpr (std::is_same_v<T, ReadData>) {
                        ssize_t nread = ::read(fd, data.buffer.data(), data.buffer.size());
                        if (nread == -1) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                // 数据还没到，不 resume，下次 epoll 再触发
                                return;
                            }
                            data.ret =
                                std::unexpected(std::error_code(errno, std::generic_category()));
                            ready_to_resume = true;
                            return;
                        }
                        data.ret = static_cast<size_t>(nread);
                        ready_to_resume = true;
                    } else if constexpr (std::is_same_v<T, WriteData>) {
                        ssize_t nwritten = ::write(fd, data.buffer.data(), data.buffer.size());
                        if (nwritten == -1) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                // 写缓冲区满，下次 epoll 再触发
                                return;
                            }
                            data.ret =
                                std::unexpected(std::error_code(errno, std::generic_category()));
                            ready_to_resume = true;
                            return;
                        }
                        data.ret = static_cast<size_t>(nwritten);
                        ready_to_resume = true;
                    } else if constexpr (std::is_same_v<T, AcceptData>) {
                        sockaddr_in in_addr{};
                        socklen_t in_len = sizeof(in_addr);
                        int new_fd = ::accept(fd, (sockaddr *)&in_addr, &in_len);
                        if (new_fd == -1) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                return;
                            }
                            data.ret =
                                std::unexpected(std::error_code(errno, std::generic_category()));
                            ready_to_resume = true;
                            return;
                        }
                        // make_socket_non_blocking(new_fd);
                        data.ret = new_fd;
                        ready_to_resume = true;
                    }
                },
                p->var);

            // 仅在真正准备好或者发生错误时 resume 并删除
            if (ready_to_resume) {
                p->handle.resume();
                delete p;
            }
        }
    }
};

struct accept_awaiter {
    int listen_fd_;
    std::expected<int, std::error_code> ret_{}; // 结果

    explicit accept_awaiter(int fd) : listen_fd_(fd) {}

    bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        EpollRuntime::ins().add_watch(AcceptData{listen_fd_, ret_}, h);
    }

    std::expected<int, std::error_code> await_resume() noexcept {
        return std::move(ret_);
    }
};

struct read_awaiter {
    int fd_;
    std::span<char> buf_;
    std::expected<size_t, std::error_code> ret_{}; // 结果

    read_awaiter(int fd, std::span<char> buf) : fd_(fd), buf_(buf) {}

    bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        EpollRuntime::ins().add_watch(ReadData{fd_, buf_, ret_}, h);
    }

    std::expected<size_t, std::error_code> await_resume() noexcept {
        return std::move(ret_);
    }
};

struct write_awaiter {
    int fd_;
    std::span<char const> buf_;
    std::expected<size_t, std::error_code> ret_{}; // 结果

    write_awaiter(int fd, std::span<char const> buf) : fd_(fd), buf_(buf) {}

    bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        EpollRuntime::ins().add_watch(WriteData{fd_, buf_, ret_}, h);
    }

    std::expected<size_t, std::error_code> await_resume() noexcept {
        return std::move(ret_);
    }
};

task_coro<int> async_echo_client(int client_fd) {
    try {
        while (!g_stop) {
            std::array<char, 1024> buf{};
            // read_awaiter 返回 std::expected<size_t, error_code>
            auto r = co_await read_awaiter(client_fd, buf);
            if (!r) {
                std::cerr << "read failed on fd=" << client_fd << ": " << r.error().message()
                          << std::endl;

                break;
            }
            size_t n = *r;
            if (n == 0) {
                // 对端关闭
                std::cout << "client fd=" << client_fd << " closed connection\n";
                break;
            }

            log();
            std::cout << "Received " << n << " bytes from fd=" << client_fd << ": "
                      << std::string_view(buf.data(), n) << std::endl;

            // 写回客户端
            auto w = co_await write_awaiter(client_fd, std::span(buf.data(), n));
            if (!w) {
                std::cerr << "write failed on fd=" << client_fd << ": " << w.error().message()
                          << std::endl;
                break;
            }
            size_t written = *w;
            if (written != n) {
                std::cerr << "partial write on fd=" << client_fd << ": " << written << "/" << n
                          << std::endl;
            }
        }
    } catch (...) {
        std::cerr << "client fd=" << client_fd << " disconnected with exception\n";
    }

    close(client_fd);
    co_return 0;
}

task_coro<int> async_accept_loop(int listen_fd) {
    while (!g_stop) {
        auto r = co_await accept_awaiter(listen_fd);
        if (!r) {
            std::cerr << "accept failed: " << r.error().message() << std::endl;
            continue; // 再次等待 accept
        }
        int client_fd = *r;
        std::cout << "accept fd: " << client_fd << std::endl;
        co_await async_echo_client(client_fd);
    }
    co_return 0;
}

void test_epoll_task() {
    std::signal(SIGINT, sig_ctrl_c);
    std::cout.sync_with_stdio(false);
    int listen_fd = create_listen_socket(8080);
    auto r = async_accept_loop(listen_fd);
    r.coro_.resume();
    while (!g_stop) {
        std::cout << "EpollRuntime tick..." << std::endl;
        EpollRuntime::ins().do_once();
    }

    close(listen_fd);
    std::cout << "Coroutine returned: " << r.get() << std::endl;
}

int main() {
    // test_base_task();
    test_epoll_task();
    return 0;
    std::signal(SIGINT, sig_ctrl_c);
    std::cout.sync_with_stdio(false);
    int listen_fd = create_listen_socket(8080);
    int epoll_fd = create_epoll(listen_fd);

    // RAII 管理文件描述符
    struct fd_guard {
        int fd;

        ~fd_guard() {
            std ::cout << "Closing fd " << fd << std::endl;
            close(fd);
        }
    } listen_guard{listen_fd}, fd_guard{epoll_fd};

    std::cout << "Server running on port 8080..." << std::endl;
    event_loop(epoll_fd, listen_fd);

    std::cout << "Exiting..." << std::endl;
}

void event_loop(int epoll_fd, int listen_fd) {
    int const MAX_EVENTS = 10;
    epoll_event events[MAX_EVENTS];

    while (!g_stop) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000);
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("epoll_wait");
            throw;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                std::cerr << "epoll error on fd " << events[i].data.fd << "\n";
                close(events[i].data.fd);
                continue;
            }

            if (events[i].data.fd == listen_fd) {
                handle_new_connection(epoll_fd, listen_fd);
            } else {
                handle_client_io(events[i].data.fd);
            }
        }
    }
}

void handle_client_io(int fd) {
    char buf[512];
    while (true) {
        ssize_t count = read(fd, buf, sizeof(buf));
        if (count == -1) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("read");
                assert_error(false);
            }
            break;
        } else if (count == 0) {
            close(fd);
            std::cout << "Closed connection on fd " << fd << std::endl;
            break;
        }

        ssize_t written = 0;
        while (written < count) {
            ssize_t w = write(fd, buf + written, count - written);
            if (w == -1) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("write");
                    assert_error(false);
                }
                break;
            }
            written += w;
        }
    }
}

void handle_new_connection(int epoll_fd, int listen_fd) {
    sockaddr_in in_addr{};
    socklen_t in_len = sizeof(in_addr);
    int infd = accept(listen_fd, (sockaddr *)&in_addr, &in_len);
    if (infd == -1) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("accept");
            throw;
        }
        return;
    }

    make_socket_non_blocking(infd);
    epoll_event event{};
    event.data.fd = infd;
    event.events = EPOLLIN;
    assert_error(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, infd, &event) != -1);
}

void assert_error(bool condition, std::source_location const location) {
    if (!condition) {
        std::cerr << "Error at " << location.file_name() << ":" << location.line()
                  << " in function " << location.function_name() << std::endl;
        std::cerr << "Error: " << strerror(errno) << " errno: " << errno << std::endl;
        throw;
    }
}

int create_epoll(int listen_fd) {
    int epoll_fd = epoll_create1(0);
    assert_error(epoll_fd != -1);

    epoll_event event;
    event.data.fd = listen_fd;
    event.events = EPOLLIN;

    assert_error(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &event) != -1);
    return epoll_fd;
}

int create_listen_socket(unsigned short port) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    std::cout.sync_with_stdio(false);
    assert_error(listen_fd != -1);
    // 设置 SO_REUSEADDR
    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR failed");
        close(listen_fd);
        return -1;
    }

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8080);
    assert_error(bind(listen_fd, (sockaddr *)&addr, sizeof(addr)) != -1);
    assert_error(listen(listen_fd, SOMAXCONN) != -1);
    make_socket_non_blocking(listen_fd);
    return listen_fd;
}

void make_socket_non_blocking(int sfd) {
    int flags = fcntl(sfd, F_GETFL, 0);
    if (flags == -1) {
        std::cerr << "fcntl F_GETFL failed" << std::endl;
        throw;
    }

    flags |= O_NONBLOCK;
    if (fcntl(sfd, F_SETFL, flags) == -1) {
        std::cerr << "fcntl F_SETFL failed" << std::endl;
        throw;
    }
}
