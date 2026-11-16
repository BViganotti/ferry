#include <condition_variable>
#include <coroutine>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ferry/net.hpp"

namespace ferry {

namespace {

struct ResolveState {
  std::mutex mutex;
  EventLoop* loop = nullptr;
  std::coroutine_handle<> handle{};
  std::string host;
  std::uint16_t port = 0;
  bool done = false;
  bool timed_out = false;
  ResolveResult result;
};

class Resolver {
 public:
  static Resolver& instance() {
    static Resolver resolver;
    return resolver;
  }

  ~Resolver() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    condition_.notify_all();
    for (auto& thread : threads_) {
      if (thread.joinable()) thread.join();
    }
  }

  void submit(std::shared_ptr<ResolveState> state) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      queue_.push_back(std::move(state));
    }
    condition_.notify_one();
  }

 private:
  Resolver() {
    unsigned count = std::thread::hardware_concurrency();
    if (count < 2) count = 2;
    if (count > 4) count = 4;
    for (unsigned i = 0; i < count; ++i) {
      threads_.emplace_back([this] { worker(); });
    }
  }

  void worker() {
    for (;;) {
      std::shared_ptr<ResolveState> state;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (stopping_ && queue_.empty()) return;
        state = std::move(queue_.front());
        queue_.pop_front();
      }

      ResolveResult result;
      bool should_resume = false;
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->done) {
          result.addresses = Address::resolve(state->host, state->port);
          if (result.addresses.empty()) {
            result.error = "cannot resolve host '" + state->host + "'";
          }
          state->result = std::move(result);
          state->done = true;
          should_resume = true;
        }
      }

      if (should_resume) {
        auto loop = state->loop;
        loop->post([state] { state->handle.resume(); });
      }
    }
  }

  std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<std::shared_ptr<ResolveState>> queue_;
  std::vector<std::thread> threads_;
  bool stopping_ = false;
};

class ResolveAwaiter {
 public:
  ResolveAwaiter(std::shared_ptr<ResolveState> state, Deadline deadline)
      : state_(std::move(state)), deadline_(deadline) {}

  bool await_ready() noexcept {
    if (deadline_.expired()) {
      state_->done = true;
      state_->timed_out = true;
      state_->result.error = "DNS resolution timed out";
      return true;
    }
    return false;
  }

  void await_suspend(std::coroutine_handle<> handle) {
    state_->handle = handle;

    if (!deadline_.is_infinite()) {
      auto remaining = deadline_.remaining();
      if (remaining.count() <= 0) {
        state_->result.error = "DNS resolution timed out";
        state_->timed_out = true;
        state_->done = true;
        auto state = state_;
        state_->loop->post([state] { state->handle.resume(); });
        return;
      }
      auto state = state_;
      state_->loop->after(remaining, [state] {
        bool resume = false;
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          if (!state->done) {
            state->done = true;
            state->timed_out = true;
            state->result.error = "DNS resolution timed out";
            resume = true;
          }
        }
        if (resume) state->handle.resume();
      });
    }

    Resolver::instance().submit(state_);
  }

  ResolveResult await_resume() {
    auto result = std::move(state_->result);
    if (state_->timed_out && result.error.empty()) result.error = "DNS resolution timed out";
    return result;
  }

 private:
  std::shared_ptr<ResolveState> state_;
  Deadline deadline_;
};

}  // namespace

task<ResolveResult> resolve_async(std::string host, uint16_t port, Deadline deadline) {
  if (auto numeric = Address::parse(host, port)) {
    ResolveResult result;
    result.addresses.push_back(*numeric);
    co_return result;
  }

  EventLoop* loop = EventLoop::current();
  if (!loop) {
    ResolveResult result;
    result.error = "resolve_async requires an event loop";
    co_return result;
  }

  auto state = std::make_shared<ResolveState>();
  state->loop = loop;
  state->host = std::move(host);
  state->port = port;
  co_return co_await ResolveAwaiter(std::move(state), deadline);
}

}  // namespace ferry
