#pragma once
#include <atomic>
#include <cassert>
#include <concepts>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace coro {

class static_thread_pool;

template <class Tp>
class async_awaiter;

template <class Tp>
class parallel_awaiter;

class always_awaiter : public std::suspend_always {
 public:
  always_awaiter() noexcept : scheduler_(nullptr) {}
  explicit always_awaiter(static_thread_pool* pool) noexcept
      : scheduler_(pool) {}

  inline void await_suspend(std::coroutine_handle<>) const;

 private:
  static_thread_pool* scheduler_;
};

template <class Tp>
  requires(!std::is_reference_v<Tp>)
class task;

enum class task_type {
  deferred,
  async,
};

class latch;

namespace this_scheduler {
inline always_awaiter yield;

template <class Tp>
parallel_awaiter<Tp> parallel(task<Tp>) noexcept;
}  // namespace this_scheduler

namespace details_ {

struct cutex_wait_context {
  static_thread_pool* scheduler{nullptr};
  std::deque<std::coroutine_handle<>> wait_ques;
  std::mutex mu;
};

template <class Pred>
  requires requires(Pred f, std::coroutine_handle<> h) {
    { f(h) } -> std::same_as<bool>;
  }
class condition_awaiter : public std::suspend_always {
 public:
  explicit condition_awaiter(Pred f) : f_(f) {}

  bool await_suspend(std::coroutine_handle<> h) const { return f_(h); }

 private:
  Pred f_;
};

template <class Tp>
condition_awaiter(Tp) -> condition_awaiter<Tp>;

template <class Tp>
struct final_awaiter;

template <class Tp>
class task_base;

template <class Tp>
class promise_base : public std::promise<Tp> {
 public:
  struct shared_ctx_t {
    std::mutex mu;
    std::coroutine_handle<> wait_coro;
    bool done{false};
    bool has_scheduled{false};
    bool suspend_on_final{true};
    static_thread_pool* scheduler{nullptr};
  };

  task<Tp> get_return_object() noexcept;

  std::suspend_always initial_suspend() const noexcept { return {}; }

  inline final_awaiter<Tp> final_suspend() noexcept;

  template <class Awaiter>
  void await_transform(Awaiter) = delete;

  inline auto await_transform(latch&) noexcept;

  always_awaiter await_transform(always_awaiter) noexcept {
    return always_awaiter{shared_ctx_->scheduler};
  }

  template <class Up>
  inline async_awaiter<Up> await_transform(task<Up>) noexcept;

  template <class Up>
  inline parallel_awaiter<Up> await_transform(parallel_awaiter<Up>) noexcept;

  void unhandled_exception() noexcept {
    this->set_exception(std::current_exception());
  }

 protected:
  template <class Up>
  friend class promise_base;

  promise_base() : shared_ctx_(std::make_shared<shared_ctx_t>()) {}

  std::shared_ptr<shared_ctx_t> shared_ctx_;

  template <class Up>
  friend struct final_awaiter;
};
}  // namespace details_

template <class Tp>
  requires(!std::is_reference_v<Tp>)
class promise : public details_::promise_base<Tp> {
 public:
  template <class Up>
  void return_value(Up&& value)
    requires(std::is_same_v<Tp, Up> || std::is_same_v<const Tp&, Up> ||
             std::is_constructible_v<Tp, Up>)
  {
    this->set_value(std::forward<Up>(value));
  }

 private:
  template <class Up>
  friend class details_::task_base;

  template <class Up>
  friend class async_awaiter;

  template <class Up>
    requires(!std::is_reference_v<Up>)
  friend struct synced_waker;

  friend class static_thread_pool;
};

template <>
class promise<void> : public details_::promise_base<void> {
 public:
  void return_void() noexcept { this->set_value(); }

 private:
  template <class Up>
  friend class details_::task_base;

  template <class Up>
  friend class async_awaiter;

  friend class static_thread_pool;
};

template <class>
struct is_task : std::false_type {};

template <class Tp>
struct is_task<task<Tp>> : std::true_type {};

template <class Tp>
inline constexpr bool is_task_v = is_task<Tp>::value;

template <class>
struct is_promise : std::false_type {};

template <class Tp>
struct is_promise<promise<Tp>> : std::true_type {};

template <class Tp>
inline constexpr bool is_promise_v = is_promise<Tp>::value;

namespace details_ {
template <class Tp>
class task_base {
 public:
  bool valid() const noexcept { return fu_.valid(); }

  task_type type() const noexcept { return typ_; }

  void wait() const {
    if (typ_ == task_type::deferred) {
      while (!handle_.done()) {
        handle_.resume();
      }
    } else {
      fu_.wait();
    }
  }

  template <typename Rep, typename Period>
  std::future_status wait_for(
      const std::chrono::duration<Rep, Period>& rel) const {
    if (typ_ == task_type::deferred) {
      return std::future_status::deferred;
    }
    return fu_.wait_for(rel);
  }

  Tp get() {
    wait();
    if constexpr (std::is_same_v<Tp, void>) {
      fu_.get();
      // Handle of the non-async task should be destroyed here.
      if (typ_ == task_type::deferred) {
        handle_.destroy();
      }
    } else {
      Tp res = fu_.get();
      if (typ_ == task_type::deferred) {
        handle_.destroy();
      }
      return res;
    }
  }

  std::coroutine_handle<promise<Tp>> release_coroutine_handle() noexcept {
    if (typ_ == task_type::async) {
      return nullptr;
    }
    auto h = handle_;
    typ_ = task_type::async;
    return h;
  }

  ~task_base() {
    if (this->typ_ == task_type::deferred && this->fu_.valid()) {
      get();
    }
  }

 protected:
  task_base(std::coroutine_handle<promise<Tp>> h = nullptr,
            task_type t = task_type::deferred) noexcept
      : handle_(h), typ_(t) {
    if (h) {
      fu_ = h.promise().get_future();
      shared_ctx_ = h.promise().shared_ctx_;
    }
  }

  task_base(task_base&& rhs) noexcept { *this = std::move(rhs); }

  task_base& operator=(task_base&& rhs) noexcept {
    handle_ = rhs.handle_;
    fu_ = std::move(rhs.fu_);
    shared_ctx_ = std::move(rhs.shared_ctx_);
    typ_ = rhs.typ_;
    rhs.handle_ = nullptr;
    return *this;
  }

  promise<Tp>& get_promise() const { return handle_.promise(); }

  std::coroutine_handle<promise<Tp>> handle_;
  std::future<Tp> fu_;
  std::shared_ptr<typename promise<Tp>::shared_ctx_t> shared_ctx_;
  task_type typ_;
};
}  // namespace details_

template <class Tp = void>
  requires(!std::is_reference_v<Tp>)
class task : public details_::task_base<Tp> {
 public:
  using promise_type = promise<Tp>;

  task() noexcept {}
  task(task&& rhs) noexcept = default;
  task& operator=(task&& rhs) noexcept = default;

 protected:
  task(std::coroutine_handle<promise_type> h, task_type t)
      : details_::task_base<Tp>(h, t) {}

  template <class Rp>
  friend class details_::promise_base;

  friend class static_thread_pool;
};

template <class Tp>
task<Tp> details_::promise_base<Tp>::get_return_object() noexcept {
  return task<Tp>(std::coroutine_handle<promise<Tp>>::from_promise(
                      static_cast<promise<Tp>&>(*this)),
                  task_type::deferred);
}

class static_thread_pool {
 public:
  explicit static_thread_pool(size_t n) : exit_(false) {
    for (size_t i = 0; i < n; i++) {
      ths_.push_back(std::thread(&static_thread_pool::worker_routine, this));
    }
  }

  ~static_thread_pool() {
    {
      std::unique_lock l(mu_);
      exit_ = true;
    }
    con_.notify_all();
    for (auto& th : ths_) {
      th.join();
    }
  }

  template <class Tp>
  task<Tp> schedule(task<Tp>&& t) {
    schedule(t.release_coroutine_handle());
    return t;
  }

  template <class Tp>
  task<Tp>& schedule(task<Tp>& t) {
    schedule(t.release_coroutine_handle());
    return t;
  }

 private:
  friend class always_awaiter;

  template <class Tp>
  friend class async_awaiter;

  template <class Tp>
  friend struct details_::final_awaiter;

  friend class latch;

  template <class Tp>
  void schedule(std::coroutine_handle<Tp> handle) {
    if constexpr (is_promise_v<Tp>) {
      handle.promise().shared_ctx_->suspend_on_final = false;
      handle.promise().shared_ctx_->scheduler = this;
    }
    {
      std::unique_lock l(mu_);
      que_.push(handle);
    }
    con_.notify_one();
  }

  void worker_routine() {
    while (true) {
      std::coroutine_handle<> t;
      {
        std::unique_lock l(mu_);
        con_.wait(l, [&] { return exit_ || !que_.empty(); });
        if (que_.empty() && exit_) {
          break;
        }
        t = std::move(que_.front());
        que_.pop();
      }
      t.resume();
    }
  }

  std::queue<std::coroutine_handle<>> que_;
  std::mutex mu_;
  std::condition_variable con_;
  std::vector<std::thread> ths_;
  bool exit_;
};

inline void always_awaiter::await_suspend(std::coroutine_handle<> h) const {
  if (scheduler_) {
    scheduler_->schedule(h);
  }
}

template <class Tp>
class async_awaiter : protected coro::task<Tp> {
 public:
  bool await_ready() const noexcept {
    if (this->typ_ == task_type::deferred) {
      return true;  // jump to call this->get() directly.
    }
    using namespace std::chrono_literals;
    return this->wait_for(0s) == std::future_status::ready;
  }

  bool await_suspend(std::coroutine_handle<> h) {
    // The callee might resume caller in the future, and result in destruction
    // of the caller frame. Which means the awaiter will be destructed too.
    // Therefore, we cannot use `this` next.
    return maybe_suspend(suspend_, this->shared_ctx_, this->handle_, h);
  }

  Tp await_resume() { return this->get(); }

 protected:
  template <class Up>
  friend class details_::promise_base;

  explicit async_awaiter(task<Tp> t, bool suspend = true) noexcept
      : task<Tp>(std::move(t)), suspend_(suspend) {}

  static bool maybe_suspend(
      bool need_suspend,
      std::shared_ptr<typename promise<Tp>::shared_ctx_t> callee_ctx,
      std::coroutine_handle<promise<Tp>> callee,
      std::coroutine_handle<> caller) {
    bool has_scheduled = false;
    bool done = false;
    {
      std::unique_lock l(callee_ctx->mu);
      if (done = callee_ctx->done; !done && need_suspend) {
        callee_ctx->wait_coro = caller;
      }
      has_scheduled = callee_ctx->has_scheduled;
      if (!has_scheduled && callee_ctx->scheduler) {
        callee_ctx->has_scheduled = true;
      }
    }
    if (!has_scheduled && callee_ctx->scheduler) {
      callee_ctx->scheduler->schedule(callee);
    }
    return !done && need_suspend;
  }

  bool suspend_;
};

template <class Tp>
class parallel_awaiter : public async_awaiter<Tp> {
 public:
  task<Tp> await_resume() noexcept {
    return std::move(static_cast<task<Tp>&>(*this));
  }

 private:
  template <class Up>
  friend class details_::promise_base;

  template <class Up>
  friend parallel_awaiter<Up> this_scheduler::parallel(task<Up>) noexcept;

  explicit parallel_awaiter(task<Tp> t) noexcept
      : async_awaiter<Tp>(std::move(t), false /*not suspend*/) {}
};

template <class Tp>
struct details_::final_awaiter {
  constexpr bool await_ready() const noexcept { return false; }

  bool await_suspend(std::coroutine_handle<>) const noexcept {
    auto shared_ctx = std::move(self->shared_ctx_);
    std::unique_lock l(shared_ctx->mu);
    shared_ctx->done = true;
    if (shared_ctx->wait_coro) {
      if (shared_ctx->scheduler) {
        shared_ctx->scheduler->schedule(shared_ctx->wait_coro);
      } else {
        shared_ctx->wait_coro.resume();
      }
    }
    return shared_ctx->suspend_on_final;
  }

  constexpr void await_resume() const noexcept {}

  promise_base<Tp>* self;
};

template <class Tp>
inline details_::final_awaiter<Tp>
details_::promise_base<Tp>::final_suspend() noexcept {
  return {this};
}

class latch {
 public:
  explicit latch(std::ptrdiff_t countdown) : countdown_(countdown) {}
  latch(const latch&) = delete;
  latch& operator=(const latch&) = delete;

  void count_down(std::ptrdiff_t n = 1) {
    auto before = countdown_.fetch_sub(n, std::memory_order_acq_rel);
    if (before > 0 && before - n <= 0) {
      std::unique_lock l(wait_ctx_.mu);
      if (!wait_ctx_.scheduler) return;
      for (auto h : wait_ctx_.wait_ques) {
        wait_ctx_.scheduler->schedule(h);
      }
    }
  }

 private:
  template <class Tp>
  friend class details_::promise_base;

  auto wait_this(static_thread_pool* scheduler) {
    return [scheduler, this](std::coroutine_handle<> h) -> bool {
      std::unique_lock l(wait_ctx_.mu);
      if (countdown_.load(std::memory_order_acquire) <= 0) {
        return false;
      }
      wait_ctx_.scheduler = scheduler;
      wait_ctx_.wait_ques.push_back(h);
      return true;
    };
  }

  std::atomic<ptrdiff_t> countdown_;
  details_::cutex_wait_context wait_ctx_;
};

template <class Tp>
inline auto details_::promise_base<Tp>::await_transform(latch& l) noexcept {
  return condition_awaiter(l.wait_this(shared_ctx_->scheduler));
}

template <class Tp>
template <class Up>
inline async_awaiter<Up> details_::promise_base<Tp>::await_transform(
    task<Up> t) noexcept {
  t.shared_ctx_->scheduler = shared_ctx_->scheduler;
  if (shared_ctx_->scheduler) {
    t.typ_ = task_type::async;
  }
  return async_awaiter<Up>(std::move(t));
}

template <class Tp>
template <class Up>
inline parallel_awaiter<Up> details_::promise_base<Tp>::await_transform(
    parallel_awaiter<Up> awaiter) noexcept {
  awaiter.shared_ctx_->scheduler = shared_ctx_->scheduler;
  if (shared_ctx_->scheduler) {
    awaiter.typ_ = task_type::async;
  }
  return std::move(awaiter);
}

template <class Tp>
parallel_awaiter<Tp> this_scheduler::parallel(task<Tp> t) noexcept {
  return parallel_awaiter<Tp>(std::move(t));
}

}  // namespace coro