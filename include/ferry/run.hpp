// ferry::run — run a coroutine to completion on a fresh event loop.

#pragma once

#include <exception>
#include <type_traits>
#include <utility>

#include "ferry/event_loop.hpp"
#include "ferry/task.hpp"

namespace ferry {

namespace detail {

template <class F>
task<void> run_root_void(F fn, EventLoop& loop, std::exception_ptr& error) {
  try {
    co_await fn();
  } catch (...) {
    error = std::current_exception();
  }
  loop.stop();
}

template <class F>
task<void> run_root_int(F fn, EventLoop& loop, int& status, std::exception_ptr& error) {
  try {
    status = co_await fn();
  } catch (...) {
    error = std::current_exception();
  }
  loop.stop();
}

}  // namespace detail

// Runs the coroutine returned by fn on a new EventLoop. Returns its int result
// (or 0 for a task<void>). Exceptions escaping the root task are rethrown here.
template <class F>
  requires std::is_invocable_v<F&> &&
           std::same_as<std::invoke_result_t<F&>, task<void>>
int run(F&& fn) {
  EventLoop loop;
  std::exception_ptr error;
  loop.post([&] {
    spawn(detail::run_root_void(std::forward<F>(fn), loop, error));
  });
  loop.run();
  if (error) std::rethrow_exception(error);
  return 0;
}

template <class F>
  requires std::is_invocable_v<F&> &&
           std::same_as<std::invoke_result_t<F&>, task<int>>
int run(F&& fn) {
  EventLoop loop;
  std::exception_ptr error;
  int status = 0;
  loop.post([&] {
    spawn(detail::run_root_int(std::forward<F>(fn), loop, status, error));
  });
  loop.run();
  if (error) std::rethrow_exception(error);
  return status;
}

}  // namespace ferry
