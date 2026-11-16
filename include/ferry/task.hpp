// ferry::task<T> — a lazy, move-only C++20 coroutine task.
//
// Tasks are cold: creating one allocates a frame but does not run it. Awaiting
// starts it; dropping an unawaited task destroys the frame. spawn() turns a
// task into a fire-and-forget background job, reporting escaped exceptions to
// the log handler instead of terminating the process.

#pragma once

#include <coroutine>
#include <cstdio>
#include <exception>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace ferry {

namespace detail {

inline void report_unhandled_exception(std::exception_ptr ep) noexcept {
  if (!ep) return;
  try {
    std::rethrow_exception(ep);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "ferry: unhandled exception in task: %s\n", e.what());
  } catch (...) {
    std::fprintf(stderr, "ferry: unhandled exception in task\n");
  }
}

struct promise_base {
  std::coroutine_handle<> continuation{};
  std::exception_ptr exception{};

  struct final_awaiter {
    bool await_ready() noexcept { return false; }

    template <class Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept {
      auto continuation = h.promise().continuation;
      return continuation ? continuation : std::noop_coroutine();
    }

    void await_resume() noexcept {}
  };

  final_awaiter final_suspend() noexcept { return {}; }
};

}  // namespace detail

template <class T>
class task {
 public:
  struct promise_type : detail::promise_base {
    std::optional<T> value;

    task get_return_object() noexcept {
      return task{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    std::suspend_always initial_suspend() noexcept { return {}; }
    detail::promise_base::final_awaiter final_suspend() noexcept { return {}; }

    void return_value(T v) { value.emplace(std::move(v)); }

    void unhandled_exception() noexcept { exception = std::current_exception(); }
  };

  using handle_type = std::coroutine_handle<promise_type>;

  struct awaiter {
    handle_type handle{};

    awaiter() = default;
    explicit awaiter(handle_type h) noexcept : handle(h) {}

    bool await_ready() const noexcept { return !handle || handle.done(); }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept {
      handle.promise().continuation = continuation;
      return handle;  // symmetric transfer; no extra stack frame
    }

    T await_resume() {
      auto& promise = handle.promise();
      if (promise.exception) {
        auto ep = std::exchange(promise.exception, nullptr);
        std::rethrow_exception(ep);
      }
      return std::move(*promise.value);
    }

    ~awaiter() {
      if (handle) handle.destroy();
    }

    awaiter(const awaiter&) = delete;
    awaiter& operator=(const awaiter&) = delete;

    awaiter(awaiter&& other) noexcept : handle(std::exchange(other.handle, {})) {}

    awaiter& operator=(awaiter&& other) noexcept {
      if (this != &other) {
        if (handle) handle.destroy();
        handle = std::exchange(other.handle, {});
      }
      return *this;
    }
  };

  task() noexcept = default;
  explicit task(handle_type handle) noexcept : handle_(handle) {}

  task(task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}

  task& operator=(task&& other) noexcept {
    if (this != &other) {
      if (handle_) handle_.destroy();
      handle_ = std::exchange(other.handle_, {});
    }
    return *this;
  }

  task(const task&) = delete;
  task& operator=(const task&) = delete;

  ~task() {
    if (handle_) handle_.destroy();
  }

  bool valid() const noexcept { return static_cast<bool>(handle_); }

  handle_type release() noexcept { return std::exchange(handle_, {}); }

  awaiter operator co_await() && noexcept { return awaiter{release()}; }

 private:
  handle_type handle_{};
};

template <>
class task<void> {
 public:
  struct promise_type : detail::promise_base {
    task get_return_object() noexcept {
      return task{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    std::suspend_always initial_suspend() noexcept { return {}; }
    detail::promise_base::final_awaiter final_suspend() noexcept { return {}; }

    void return_void() noexcept {}

    void unhandled_exception() noexcept { exception = std::current_exception(); }
  };

  using handle_type = std::coroutine_handle<promise_type>;

  struct awaiter {
    handle_type handle{};

    awaiter() = default;
    explicit awaiter(handle_type h) noexcept : handle(h) {}

    bool await_ready() const noexcept { return !handle || handle.done(); }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept {
      handle.promise().continuation = continuation;
      return handle;
    }

    void await_resume() {
      auto& promise = handle.promise();
      if (promise.exception) {
        auto ep = std::exchange(promise.exception, nullptr);
        std::rethrow_exception(ep);
      }
    }

    ~awaiter() {
      if (handle) handle.destroy();
    }

    awaiter(const awaiter&) = delete;
    awaiter& operator=(const awaiter&) = delete;

    awaiter(awaiter&& other) noexcept : handle(std::exchange(other.handle, {})) {}

    awaiter& operator=(awaiter&& other) noexcept {
      if (this != &other) {
        if (handle) handle.destroy();
        handle = std::exchange(other.handle, {});
      }
      return *this;
    }
  };

  task() noexcept = default;
  explicit task(handle_type handle) noexcept : handle_(handle) {}

  task(task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}

  task& operator=(task&& other) noexcept {
    if (this != &other) {
      if (handle_) handle_.destroy();
      handle_ = std::exchange(other.handle_, {});
    }
    return *this;
  }

  task(const task&) = delete;
  task& operator=(const task&) = delete;

  ~task() {
    if (handle_) handle_.destroy();
  }

  bool valid() const noexcept { return static_cast<bool>(handle_); }

  handle_type release() noexcept { return std::exchange(handle_, {}); }

  awaiter operator co_await() && noexcept { return awaiter{release()}; }

 private:
  handle_type handle_{};
};

namespace detail {

template <class T>
struct is_task : std::false_type {};

template <class T>
struct is_task<task<T>> : std::true_type {};

struct detached {
  struct promise_type {
    detached get_return_object() noexcept { return {}; }
    std::suspend_never initial_suspend() noexcept { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void return_void() noexcept {}
    void unhandled_exception() noexcept {
      report_unhandled_exception(std::current_exception());
    }
  };
};

template <class T>
detached run_detached(task<T> work) {
  co_await std::move(work);
}

// Coroutine lambdas keep a pointer to their closure object. This wrapper owns
// the callable in a shared_ptr for as long as the coroutine runs, so passing a
// temporary lambda to spawn() is safe.
template <class F>
detached run_detached_callable(std::shared_ptr<std::decay_t<F>> callable) {
  co_await (*callable)();
}

}  // namespace detail

// Start a task in the background. The task runs on the current thread's event
// loop; the caller must therefore be on an event-loop thread.
template <class T>
void spawn(task<T>&& work) {
  detail::run_detached(std::move(work));
}

// Convenience overload for coroutine callables. The callable is copied into a
// heap-owned wrapper, avoiding the classic dangling-closure bug:
//
//   spawn([client]() -> task<void> { co_await client.get(url); });
template <class F>
  requires std::is_invocable_v<F&> && detail::is_task<std::invoke_result_t<F&>>::value
void spawn(F&& callable) {
  auto holder = std::make_shared<std::decay_t<F>>(std::forward<F>(callable));
  detail::run_detached_callable<std::decay_t<F>>(std::move(holder));
}

}  // namespace ferry
