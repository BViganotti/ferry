// ferry::EventLoop — single-threaded readiness reactor (epoll / kqueue / poll)
// with C++20 coroutine integration and a timer heap.

#pragma once

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "ferry/task.hpp"

namespace ferry {

struct IoResult {
  enum class Status {
    Ok,
    Eof,
    Timeout,
    Cancelled,
    Error,
  };

  Status status = Status::Ok;
  std::size_t n = 0;
  int error = 0;
  std::string text;  // optional human-readable detail (TLS alerts, etc.)

  bool ok() const noexcept { return status == Status::Ok; }
  bool eof() const noexcept { return status == Status::Eof; }
  bool timeout() const noexcept { return status == Status::Timeout; }
  bool cancelled() const noexcept { return status == Status::Cancelled; }
  bool failed() const noexcept { return status == Status::Error; }

  std::string message() const;
};

class Deadline {
 public:
  Deadline() noexcept = default;

  static Deadline infinite() noexcept { return Deadline{}; }

  static Deadline after(std::chrono::milliseconds d) noexcept {
    Deadline deadline;
    deadline.infinite_ = false;
    deadline.time_point_ = std::chrono::steady_clock::now() + d;
    return deadline;
  }

  static Deadline at(std::chrono::steady_clock::time_point tp) noexcept {
    Deadline deadline;
    deadline.infinite_ = false;
    deadline.time_point_ = tp;
    return deadline;
  }

  static Deadline earlier(const Deadline& a, const Deadline& b) noexcept {
    if (a.is_infinite()) return b;
    if (b.is_infinite()) return a;
    return a.time_point_ <= b.time_point_ ? a : b;
  }

  bool is_infinite() const noexcept { return infinite_; }

  std::chrono::steady_clock::time_point time_point() const noexcept {
    return time_point_;
  }

  std::chrono::milliseconds remaining() const noexcept {
    if (infinite_) return std::chrono::milliseconds::max();
    auto now = std::chrono::steady_clock::now();
    if (now >= time_point_) return std::chrono::milliseconds(0);
    return std::chrono::duration_cast<std::chrono::milliseconds>(time_point_ - now);
  }

  bool expired() const noexcept {
    return !infinite_ && std::chrono::steady_clock::now() >= time_point_;
  }

 private:
  std::chrono::steady_clock::time_point time_point_{};
  bool infinite_ = true;
};

class EventLoop;

namespace detail {

struct WaitOp {
  EventLoop* loop = nullptr;
  int fd = -1;
  unsigned events = 0;  // bit 0 = read, bit 1 = write
  std::coroutine_handle<> handle{};
  bool active = false;
  bool ready = false;
  bool timed_out = false;
  bool cancelled = false;
  int error = 0;
  bool has_deadline = false;
  std::chrono::steady_clock::time_point deadline{};
  std::size_t timer_index = static_cast<std::size_t>(-1);
};

}  // namespace detail

// Awaitable that suspends until a descriptor is readable/writable, the deadline
// expires, or the loop cancels it. On success the caller still performs the
// syscall (and handles EAGAIN by awaiting again).
class IoWait {
 public:
  IoWait(EventLoop& loop, int fd, unsigned events, Deadline deadline) noexcept
      : loop_(loop), fd_(fd), events_(events), deadline_(deadline) {}

  bool await_ready() noexcept {
    if (deadline_.expired()) {
      op_.timed_out = true;
      return true;
    }
    return false;
  }

  void await_suspend(std::coroutine_handle<> handle);
  IoResult await_resume();

  IoWait(const IoWait&) = delete;
  IoWait& operator=(const IoWait&) = delete;

 private:
  EventLoop& loop_;
  int fd_;
  unsigned events_;
  Deadline deadline_;
  detail::WaitOp op_{};
};

class EventLoop {
 public:
  enum Events : unsigned {
    Read = 1u << 0,
    Write = 1u << 1,
  };

  EventLoop();
  ~EventLoop();

  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;

  // Runs until stop() is called and all coroutines suspended on this loop have
  // been resumed with IoResult::Cancelled.
  void run();

  // Thread-safe. Requests the loop to exit. Pending waits are cancelled with
  // IoResult::Cancelled before run() returns.
  void stop();
  bool stopped() const noexcept;

  // Thread-safe. Queues a function to run on the loop thread and wakes it.
  void post(std::function<void()> fn);

  // Schedules a callback on the loop thread. Must be called from the loop
  // thread (or before run()).
  void after(std::chrono::milliseconds delay, std::function<void()> fn);

  // Cancels both pending read and write waits for fd. Safe to call from the
  // loop thread.
  void cancel_io(int fd);

  // The loop of the current thread, if any. Set by run().
  static EventLoop* current() noexcept;

  bool in_loop_thread() const noexcept;

  // Internal: used by IoWait.
  void add_wait(detail::WaitOp* op);
  void remove_wait(detail::WaitOp* op);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  friend class IoWait;
};

// Await readiness for reading / writing.
inline IoWait wait_readable(EventLoop& loop, int fd, Deadline deadline) {
  return IoWait(loop, fd, EventLoop::Read, deadline);
}

inline IoWait wait_writable(EventLoop& loop, int fd, Deadline deadline) {
  return IoWait(loop, fd, EventLoop::Write, deadline);
}

// Suspend the current coroutine for the given duration on the current loop.
task<void> sleep_for(std::chrono::milliseconds duration);

// Returns std::span<std::byte> over a mutable char buffer without copying.
inline std::span<std::byte> as_bytes(std::span<char> buffer) noexcept {
  return {reinterpret_cast<std::byte*>(buffer.data()), buffer.size()};
}

inline std::span<const std::byte> as_bytes(std::span<const char> buffer) noexcept {
  return {reinterpret_cast<const std::byte*>(buffer.data()), buffer.size()};
}

inline std::span<const std::byte> as_bytes(std::string_view text) noexcept {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

}  // namespace ferry
