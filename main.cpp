#include <arpa/inet.h>
#include <cassert>
#include <csignal>
#include <exception>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <coroutine>
#include <cstring>
#include <iostream>
#include <source_location>
void assert_error(bool condition, const std::source_location location =
                                      std::source_location::current());

void make_socket_non_blocking(int sfd);

bool g_stop = false;
void sig_ctrl_c(int);
void sig_ctrl_c(int) { g_stop = true; }
int create_listen_socket(unsigned short port = 8080);

int create_epoll(int listen_fd);

void handle_new_connection(int epoll_fd, int listen_fd);

void handle_client_io(int fd);

void event_loop(int epoll_fd, int listen_fd);

struct final_awaiter {
  bool await_ready() const noexcept { return false; }

  template <typename Promise>
  std::coroutine_handle<>
  await_suspend(std::coroutine_handle<Promise> coro) noexcept {
    if (coro.promise().caller_)
      return coro.promise().caller_;
    return std::noop_coroutine();
  }

  void await_resume() noexcept {}
};

template <class T> struct task_coro;
template <class T> struct promise_base {
  T value_;
  std::exception_ptr exception_;
  std::coroutine_handle<> caller_ = nullptr;
  task_coro<T> get_return_object();

  std::suspend_always initial_suspend() { return {}; }
  final_awaiter final_suspend() noexcept { return {}; }

  void unhandled_exception() { exception_ = std::current_exception(); }

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
template <class T> struct task_coro {
  using promise_type = promise_base<T>;
  using handle_type = std::coroutine_handle<promise_type>;
  handle_type coro_;

  task_coro(handle_type h) : coro_(h) {}
  ~task_coro() {
    if (coro_)
      coro_.destroy();
  }

  auto operator co_await() {
    struct save_caller_awaiter : std::suspend_always {
      promise_type &promise_;
      save_caller_awaiter(promise_type &p) : promise_(p) {}
      std::coroutine_handle<>
      await_suspend(std::coroutine_handle<> caller) noexcept {
        promise_.caller_ = caller;
        return std::coroutine_handle<promise_type>::from_promise(promise_);
      }
      auto await_resume() {
        if (promise_.exception_)
          std::rethrow_exception(promise_.exception_);
        return promise_.value_;
      }
    };

    return save_caller_awaiter{coro_.promise()};
  }

  T get() {
    if (coro_.promise().exception_)
      std::rethrow_exception(coro_.promise().exception_);
    return coro_.promise().value_;
  }
};
template <class T> task_coro<T> promise_base<T>::get_return_object() {
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
  r.coro_.resume();
  while (!g_stop) {
    // epoll runtime
  }
  std::cout << "Coroutine returned: " << r.get() << std::endl;
}
enum Event { ListenE };
struct EpollRuntime {
private:
  int epoll_fd;
  int listen_fd;
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
    int fd;
    std::coroutine_handle<> handle;
    int &cli_fd;
  };
  void add_watch(Event e, int fd, std::coroutine_handle<> resume_h,
                 int &client_fd) {
    if (e == ListenE) {
      resume_data *p = new resume_data{fd, resume_h, client_fd};
      listen_fd = fd;
      epoll_event event;
      event.data.ptr = p;
      event.events = EPOLLIN;
      assert_error(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) != -1);
      return;
    }
    throw;
  }
  static const int MAX_EVENTS = 10;
  static inline epoll_event events[MAX_EVENTS]{};
  void do_once() {
    int n = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000);
    if (n == -1) {
      if (errno == EINTR)
        return;
      perror("epoll_wait");
      throw;
    }

    for (int i = 0; i < n; i++) {
      if (events[i].events & (EPOLLERR | EPOLLHUP)) {
        std::cerr << "epoll error on fd " << events[i].data.fd << "\n";
        close(events[i].data.fd);
        continue;
      }
      auto p = static_cast<resume_data *>(events[i].data.ptr);
      auto fd = p->fd;
      auto hanlde = p->handle;

      if (fd == listen_fd) {
        sockaddr_in in_addr{};
        socklen_t in_len = sizeof(in_addr);
        p->cli_fd = accept(listen_fd, (sockaddr *)&in_addr, &in_len);
        if (p->cli_fd == -1) {
          if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("accept");
            throw;
          }
          return;
        }
        p->handle.resume();
        delete p;
      } else {
        std::cerr << "unimpl!\n";
      }
    }
  }
};
struct accept_awaiter : std::suspend_always {
  int listen_fd_;
  int client_fd_ = -1;
  accept_awaiter(int fd) : listen_fd_(fd) {};
  void await_suspend(std::coroutine_handle<> resume_h)  noexcept {
    auto &e = EpollRuntime::ins();
    e.add_watch(ListenE, listen_fd_, resume_h, client_fd_);
  }

  int await_resume() const noexcept { return client_fd_; }
};
task_coro<int> async_accept(int listen_fd) {
  int fd = co_await accept_awaiter(listen_fd);
  std::cout << "accept fd: " << fd << std::endl;
  co_return 0;
}
void test_epoll_task() {
  std::signal(SIGINT, sig_ctrl_c);
  std::cout.sync_with_stdio(false);
  int listen_fd = create_listen_socket(8080);
  auto r = async_accept(listen_fd);
  r.coro_.resume();
  while (!g_stop) {
    const int MAX_EVENTS = 10;
    epoll_event events[MAX_EVENTS];

    while (!g_stop) {
      EpollRuntime::ins().do_once();
    }
  }
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
  const int MAX_EVENTS = 10;
  epoll_event events[MAX_EVENTS];

  while (!g_stop) {
    int n = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000);
    if (n == -1) {
      if (errno == EINTR)
        continue;
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
void assert_error(bool condition, const std::source_location location) {
  if (!condition) {
    std::cerr << "Error at " << location.file_name() << ":" << location.line()
              << " in function " << location.function_name() << std::endl;
    std::cerr << "Error: " << strerror(errno) << " errno: " << errno
              << std::endl;
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