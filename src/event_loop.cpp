#include "ferry/event_loop.hpp"

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <unistd.h>

#if defined(__linux__)
#include <sys/epoll.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <sys/event.h>
#include <sys/time.h>
#else
#include <poll.h>
#endif

namespace ferry {

namespace {

thread_local EventLoop* tls_current_loop = nullptr;

void set_nonblocking(int fd) {
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  ::fcntl(fd, F_SETFD, FD_CLOEXEC);
}

struct Timer {
  std::chrono::steady_clock::time_point when{};
  detail::WaitOp* op = nullptr;
  std::function<void()> fn;
  std::size_t index = 0;
};

}  // namespace

struct EventLoop::Impl {
  struct FdState {
    detail::WaitOp* reader = nullptr;
    detail::WaitOp* writer = nullptr;
    bool registered = false;
    bool read_registered = false;
    bool write_registered = false;

    bool empty() const noexcept { return reader == nullptr && writer == nullptr; }
  };

  std::atomic<bool> stop_requested{false};
  bool running = false;
  std::thread::id thread_id{};

  // Wakeup pipe.
  int wake_read = -1;
  int wake_write = -1;

  // Timers, kept as an indexed min-heap so a completed wait can remove its
  // timeout in O(log n) instead of leaving dangling entries behind.
  std::vector<Timer> timers;

  // Posted work.
  std::mutex post_mutex;
  std::vector<std::function<void()>> posts;

  std::unordered_map<int, FdState> fds;

#if defined(__linux__)
  int epoll_fd = -1;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
  int kqueue_fd = -1;
#endif

  Impl() {
    int pipe_fds[2];
#if defined(__linux__)
    if (::pipe2(pipe_fds, O_NONBLOCK | O_CLOEXEC) != 0) {
      pipe_fds[0] = pipe_fds[1] = -1;
    }
#else
    if (::pipe(pipe_fds) != 0) {
      pipe_fds[0] = pipe_fds[1] = -1;
    } else {
      set_nonblocking(pipe_fds[0]);
      set_nonblocking(pipe_fds[1]);
    }
#endif
    wake_read = pipe_fds[0];
    wake_write = pipe_fds[1];

#if defined(__linux__)
    epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    kqueue_fd = ::kqueue();
    if (kqueue_fd >= 0) ::fcntl(kqueue_fd, F_SETFD, FD_CLOEXEC);
#endif
    wakeup();
  }

  ~Impl() {
    if (wake_read >= 0) ::close(wake_read);
    if (wake_write >= 0) ::close(wake_write);
#if defined(__linux__)
    if (epoll_fd >= 0) ::close(epoll_fd);
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    if (kqueue_fd >= 0) ::close(kqueue_fd);
#endif
  }

  void wakeup() {
    if (wake_write < 0) return;
    const char byte = 1;
    ssize_t n = ::write(wake_write, &byte, 1);
    (void)n;
  }

  void drain_wakeup() {
    if (wake_read < 0) return;
    char buffer[256];
    for (;;) {
      ssize_t n = ::read(wake_read, buffer, sizeof buffer);
      if (n > 0) continue;
      break;
    }
  }

  // ---- timer heap -------------------------------------------------------

  void timer_swap(std::size_t a, std::size_t b) {
    std::swap(timers[a], timers[b]);
    timers[a].index = a;
    timers[b].index = b;
    if (timers[a].op) timers[a].op->timer_index = a;
    if (timers[b].op) timers[b].op->timer_index = b;
  }

  void timer_sift_up(std::size_t index) {
    while (index > 0) {
      std::size_t parent = (index - 1) / 2;
      if (timers[parent].when <= timers[index].when) break;
      timer_swap(parent, index);
      index = parent;
    }
  }

  void timer_sift_down(std::size_t index) {
    const std::size_t size = timers.size();
    for (;;) {
      std::size_t left = index * 2 + 1;
      if (left >= size) break;
      std::size_t right = left + 1;
      std::size_t smallest = left;
      if (right < size && timers[right].when < timers[left].when) smallest = right;
      if (timers[index].when <= timers[smallest].when) break;
      timer_swap(index, smallest);
      index = smallest;
    }
  }

  void timer_push(Timer timer) {
    timer.index = timers.size();
    if (timer.op) timer.op->timer_index = timer.index;
    timers.push_back(std::move(timer));
    timer_sift_up(timers.size() - 1);
  }

  void timer_erase(std::size_t index) {
    if (index >= timers.size()) return;
    Timer removed = std::move(timers[index]);
    const std::size_t last = timers.size() - 1;
    if (index != last) {
      timers[index] = std::move(timers[last]);
      timers[index].index = index;
      if (timers[index].op) timers[index].op->timer_index = index;
    }
    timers.pop_back();
    if (removed.op) removed.op->timer_index = static_cast<std::size_t>(-1);
    if (index != last) {
      timer_sift_down(index);
      timer_sift_up(index);
    }
  }

  void remove_op_timer(detail::WaitOp* op) {
    if (op->timer_index != static_cast<std::size_t>(-1)) {
      timer_erase(op->timer_index);
      op->timer_index = static_cast<std::size_t>(-1);
    }
  }

  // ---- descriptor registration -----------------------------------------

#if defined(__linux__)
  void apply_epoll(int fd, FdState& state) {
    uint32_t mask = 0;
    if (state.reader) mask |= EPOLLIN | EPOLLRDHUP;
    if (state.writer) mask |= EPOLLOUT;

    if (mask == 0) {
      if (state.registered) {
        ::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
        state.registered = false;
      }
      return;
    }

    epoll_event event{};
    event.events = mask;
    event.data.fd = fd;
    if (!state.registered) {
      if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) == 0) {
        state.registered = true;
      }
    } else {
      ::epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event);
    }
  }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
  void kqueue_change(int fd, int16_t filter, uint16_t flags) {
    if (kqueue_fd < 0) return;
    struct kevent change{};
    EV_SET(&change, fd, filter, flags, 0, 0, nullptr);
    struct timespec zero{0, 0};
    ::kevent(kqueue_fd, &change, 1, nullptr, 0, &zero);
  }

  void apply_kqueue(int fd, FdState& state) {
    bool want_read = state.reader != nullptr;
    bool want_write = state.writer != nullptr;
    if (want_read != state.read_registered) {
      kqueue_change(fd, EVFILT_READ, want_read ? (EV_ADD | EV_ONESHOT) : EV_DELETE);
      state.read_registered = want_read;
    }
    if (want_write != state.write_registered) {
      kqueue_change(fd, EVFILT_WRITE, want_write ? (EV_ADD | EV_ONESHOT) : EV_DELETE);
      state.write_registered = want_write;
    }
  }
#endif

  void fd_add(detail::WaitOp* op) {
    if (op->fd < 0) return;
    FdState& state = fds[op->fd];
    if (op->events & EventLoop::Read) state.reader = op;
    if (op->events & EventLoop::Write) state.writer = op;
#if defined(__linux__)
    apply_epoll(op->fd, state);
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    apply_kqueue(op->fd, state);
#endif
  }

  void fd_remove(detail::WaitOp* op) {
    if (op->fd < 0) return;
    auto it = fds.find(op->fd);
    if (it == fds.end()) return;
    FdState& state = it->second;
    if ((op->events & EventLoop::Read) && state.reader == op) state.reader = nullptr;
    if ((op->events & EventLoop::Write) && state.writer == op) state.writer = nullptr;
#if defined(__linux__)
    apply_epoll(op->fd, state);
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    apply_kqueue(op->fd, state);
#endif
    if (state.empty()) fds.erase(it);
  }

  // ---- completion -------------------------------------------------------

  void complete(detail::WaitOp* op, IoResult::Status status, int error) {
    if (!op->active) return;
    op->active = false;
    op->ready = true;
    op->timed_out = status == IoResult::Status::Timeout;
    op->cancelled = status == IoResult::Status::Cancelled;
    op->error = error;
    remove_op_timer(op);
    fd_remove(op);
    auto handle = op->handle;
    if (handle) handle.resume();
  }

  void add_wait(detail::WaitOp* op) {
    op->loop = nullptr;
    op->active = true;
    op->ready = false;
    op->timed_out = false;
    op->cancelled = false;
    op->error = 0;

    op->timer_index = static_cast<std::size_t>(-1);
    if (op->has_deadline) {
      Timer timer;
      timer.when = op->deadline;
      timer.op = op;
      timer_push(std::move(timer));
    }

    if (op->fd >= 0) fd_add(op);
  }

  void cancel_op(detail::WaitOp* op) {
    complete(op, IoResult::Status::Cancelled, ECANCELED);
  }

  void cancel_fd(int fd) {
    auto it = fds.find(fd);
    if (it == fds.end()) return;
    auto* reader = it->second.reader;
    auto* writer = it->second.writer;
    if (reader) cancel_op(reader);
    if (writer && writer->active) cancel_op(writer);
  }

  void cancel_all() {
    // Resumed continuations may register new waits while unwinding. Give them
    // a few passes to finish, then abandon the rest.
    for (int pass = 0; pass < 8 && !fds.empty(); ++pass) {
      std::vector<detail::WaitOp*> ops;
      ops.reserve(fds.size() * 2);
      for (auto& [fd, state] : fds) {
        if (state.reader) ops.push_back(state.reader);
        if (state.writer) ops.push_back(state.writer);
      }
      for (auto* op : ops) {
        if (op->active) cancel_op(op);
      }
    }
    timers.clear();
  }

  void run_posts() {
    std::vector<std::function<void()>> pending;
    {
      std::lock_guard<std::mutex> lock(post_mutex);
      pending.swap(posts);
    }
    for (auto& fn : pending) {
      if (!fn) continue;
      try {
        fn();
      } catch (...) {
        detail::report_unhandled_exception(std::current_exception());
      }
    }
  }

  int compute_timeout_ms() const {
    if (timers.empty()) return -1;
    auto now = std::chrono::steady_clock::now();
    if (timers.front().when <= now) return 0;
    auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(timers.front().when - now);
    if (delta.count() > 60000) return 60000;
    return static_cast<int>(delta.count());
  }

  void expire_timers() {
    auto now = std::chrono::steady_clock::now();
    while (!timers.empty() && timers.front().when <= now) {
      Timer timer = std::move(timers.front());
      timer_erase(0);
      if (timer.op) {
        if (timer.op->active) {
          complete(timer.op, IoResult::Status::Timeout, ETIMEDOUT);
        }
      } else if (timer.fn) {
        auto fn = std::move(timer.fn);
        try {
          fn();
        } catch (...) {
          detail::report_unhandled_exception(std::current_exception());
        }
      }
    }
  }

  void handle_fd(int fd, bool readable, bool writable, bool hangup) {
    auto it = fds.find(fd);
    if (it == fds.end()) return;
    detail::WaitOp* reader = it->second.reader;
    detail::WaitOp* writer = it->second.writer;
    // Do not dereference the reader after resuming it: its coroutine may
    // complete and destroy the frame. Capture both pointers, check active.
    if ((readable || hangup) && reader && reader->active) {
      complete(reader, IoResult::Status::Ok, 0);
    }
    if ((writable || hangup) && writer && writer->active) {
      complete(writer, IoResult::Status::Ok, 0);
    }
  }

  void poll_once() {
#if defined(__linux__)
    epoll_event events[256];
    int timeout = compute_timeout_ms();
    int count = ::epoll_wait(epoll_fd, events, 256, timeout);
    if (count <= 0) return;
    for (int i = 0; i < count; ++i) {
      uint32_t mask = events[i].events;
      int fd = events[i].data.fd;
      if (fd == wake_read) {
        drain_wakeup();
        continue;
      }
      bool hangup = (mask & (EPOLLHUP | EPOLLERR)) != 0;
      handle_fd(fd, (mask & (EPOLLIN | EPOLLRDHUP)) != 0, (mask & EPOLLOUT) != 0, hangup);
    }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    struct kevent events[256];
    int timeout = compute_timeout_ms();
    struct timespec ts{};
    struct timespec* ts_ptr = nullptr;
    if (timeout >= 0) {
      ts.tv_sec = timeout / 1000;
      ts.tv_nsec = (static_cast<long>(timeout) % 1000) * 1000000L;
      ts_ptr = &ts;
    }
    int count = ::kevent(kqueue_fd, nullptr, 0, events, 256, ts_ptr);
    if (count <= 0) return;
    for (int i = 0; i < count; ++i) {
      int fd = static_cast<int>(events[i].ident);
      if (fd == wake_read) {
        drain_wakeup();
        continue;
      }
      bool readable = events[i].filter == EVFILT_READ;
      bool writable = events[i].filter == EVFILT_WRITE;
      bool hangup = (events[i].flags & EV_EOF) != 0;
      auto state_it = fds.find(fd);
      if (state_it != fds.end()) {
        if (readable) state_it->second.read_registered = false;
        if (writable) state_it->second.write_registered = false;
      }
      handle_fd(fd, readable, writable, hangup);
    }
#else
    std::vector<pollfd> pfds;
    std::vector<int> pfds_map;
    pfds.reserve(fds.size() + 1);
    pfds_map.reserve(fds.size() + 1);
    if (wake_read >= 0) {
      pfds.push_back(pollfd{wake_read, POLLIN, 0});
      pfds_map.push_back(-1);
    }
    for (auto& [fd, state] : fds) {
      short events = 0;
      if (state.reader) events |= POLLIN;
      if (state.writer) events |= POLLOUT;
      pfds.push_back(pollfd{fd, events, 0});
      pfds_map.push_back(fd);
    }
    int timeout = compute_timeout_ms();
    int count = ::poll(pfds.data(), pfds.size(), timeout);
    if (count <= 0) return;
    for (std::size_t i = 0; i < pfds.size(); ++i) {
      if (pfds[i].revents == 0) continue;
      if (pfds_map[i] == -1) {
        drain_wakeup();
        continue;
      }
      handle_fd(pfds_map[i], (pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) != 0,
                (pfds[i].revents & POLLOUT) != 0, (pfds[i].revents & (POLLHUP | POLLERR)) != 0);
    }
#endif
  }
};

EventLoop::EventLoop() : impl_(std::make_unique<Impl>()) {
#if defined(__linux__)
  if (impl_->epoll_fd < 0) {
    throw std::runtime_error("ferry: epoll_create1 failed");
  }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
  if (impl_->kqueue_fd < 0) {
    throw std::runtime_error("ferry: kqueue failed");
  }
#endif
  if (impl_->wake_read < 0 || impl_->wake_write < 0) {
    throw std::runtime_error("ferry: failed to create event-loop wakeup pipe");
  }
#if defined(__linux__)
  // The wakeup pipe is monitored permanently, not per wait.
  epoll_event wake_event{};
  wake_event.events = EPOLLIN;
  wake_event.data.fd = impl_->wake_read;
  ::epoll_ctl(impl_->epoll_fd, EPOLL_CTL_ADD, impl_->wake_read, &wake_event);
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
  // The wakeup pipe is monitored permanently, not per wait.
  impl_->kqueue_change(impl_->wake_read, EVFILT_READ, EV_ADD | EV_ENABLE);
#endif
}

EventLoop::~EventLoop() = default;

void EventLoop::add_wait(detail::WaitOp* op) {
  impl_->add_wait(op);
}

void EventLoop::remove_wait(detail::WaitOp* op) {
  if (!op->active) return;
  op->active = false;
  impl_->remove_op_timer(op);
  impl_->fd_remove(op);
}

void EventLoop::run() {
  if (impl_->running) {
    throw std::runtime_error("ferry: EventLoop::run is not reentrant");
  }
  impl_->running = true;
  impl_->thread_id = std::this_thread::get_id();

  EventLoop* previous = tls_current_loop;
  tls_current_loop = this;

  impl_->run_posts();

  while (!impl_->stop_requested.load(std::memory_order_relaxed)) {
    impl_->poll_once();
    impl_->expire_timers();
    impl_->run_posts();
  }

  impl_->cancel_all();
  impl_->run_posts();

  tls_current_loop = previous;
  impl_->running = false;
  impl_->stop_requested.store(false, std::memory_order_relaxed);
}

void EventLoop::stop() {
  impl_->stop_requested.store(true, std::memory_order_relaxed);
  impl_->wakeup();
}

bool EventLoop::stopped() const noexcept {
  return impl_->stop_requested.load(std::memory_order_relaxed);
}

void EventLoop::post(std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lock(impl_->post_mutex);
    impl_->posts.push_back(std::move(fn));
  }
  impl_->wakeup();
}

void EventLoop::after(std::chrono::milliseconds delay, std::function<void()> fn) {
  Timer timer;
  timer.when = std::chrono::steady_clock::now() + delay;
  timer.fn = std::move(fn);
  impl_->timer_push(std::move(timer));
}

void EventLoop::cancel_io(int fd) {
  impl_->cancel_fd(fd);
}

EventLoop* EventLoop::current() noexcept {
  return tls_current_loop;
}

bool EventLoop::in_loop_thread() const noexcept {
  return std::this_thread::get_id() == impl_->thread_id;
}

void IoWait::await_suspend(std::coroutine_handle<> handle) {
  op_.loop = &loop_;
  op_.fd = fd_;
  op_.events = events_;
  op_.handle = handle;
  op_.has_deadline = !deadline_.is_infinite();
  op_.deadline = deadline_.time_point();
  loop_.add_wait(&op_);
}

IoResult IoWait::await_resume() {
  IoResult result;
  if (op_.timed_out) {
    result.status = IoResult::Status::Timeout;
    result.error = ETIMEDOUT;
  } else if (op_.cancelled) {
    result.status = IoResult::Status::Cancelled;
    result.error = ECANCELED;
  } else if (op_.error != 0) {
    result.status = IoResult::Status::Error;
    result.error = op_.error;
  }
  return result;
}

std::string IoResult::message() const {
  if (!text.empty()) return text;
  switch (status) {
    case Status::Ok:
      return "ok";
    case Status::Eof:
      return "end of stream";
    case Status::Timeout:
      return "operation timed out";
    case Status::Cancelled:
      return "operation cancelled";
    case Status::Error:
      return error != 0 ? std::strerror(error) : std::string("I/O error");
  }
  return "unknown";
}

task<void> sleep_for(std::chrono::milliseconds duration) {
  EventLoop* loop = EventLoop::current();
  if (!loop) {
    throw std::runtime_error("ferry::sleep_for requires an event loop");
  }
  co_await IoWait(*loop, -1, 0, Deadline::after(duration));
}

}  // namespace ferry
