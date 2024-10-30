#pragma once
#include <cassert>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace coro {

template <class Tp>
struct async_awaiter;

struct suspend_always : std::suspend_always {};

class static_thread_pool;

template <class Tp>
  requires(!std::is_reference_v<Tp>)
class task;

enum class task_type {
  deferred,
  async,
};

namespace this_scheduler {
inline suspend_always yield;
}  // namespace this_scheduler

namespace details_ {
struct coro_context {
  std::queue<std::coroutine_handle<>> ctx_wait_que_;
  std::mutex ctx_mu_;
};

template <class Tp>
class task_base;

template <class Tp>
struct final_suspender;

template <class Tp>
struct promise_base : std::promise<Tp>, protected coro_context {
  task<Tp> get_return_object() noexcept;

  std::suspend_always initial_suspend() const noexcept { return {}; }

  inline final_suspender<Tp> final_suspend() noexcept;

  template <class Awaiter>
  void await_transform(Awaiter) = delete;

  suspend_always await_transform(suspend_always) { return suspend_always{}; }

  template <class Up>
  inline async_awaiter<Up> await_transform(task<Up>) noexcept;

  void unhandled_exception() noexcept {
    this->set_exception(std::current_exception());
  }

 protected:
  template <class Up>
  friend struct promise_base;

  promise_base() : scheduler_(nullptr), suspend_on_final_(true) {}

  static_thread_pool* scheduler_;
  bool suspend_on_final_;

  template <class Up>
  friend struct final_suspender;
};
}  // namespace details_

template <class Tp>
  requires(!std::is_reference_v<Tp>)
class promise : public details_::promise_base<Tp> {
 public:
  template <class Up>
  void return_value(Up&& value)
    requires(std::is_same_v<Tp, Up> || std::is_same_v<const Tp&, Up>)
  {
    this->set_value(std::forward<Up>(value));
  }

 private:
  template <class Up>
  friend class details_::task_base;

  template <class Up>
  friend struct async_awaiter;

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
  friend struct async_awaiter;

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
      while (!done()) {
        resume();
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

  void resume() const { handle_.resume(); }

  bool done() const noexcept { return handle_.done(); }

  void destroy() const { handle_.destroy(); }

  std::coroutine_handle<promise<Tp>> release_coroutine_handle() noexcept {
    auto h = handle_;
    handle_ = nullptr;
    typ_ = task_type::async;
    return h;
  }

 protected:
  task_base(std::coroutine_handle<promise<Tp>> h = nullptr,
            task_type t = task_type::deferred) noexcept
      : handle_(h), typ_(t) {
    if (h) {
      fu_ = h.promise().get_future();
    }
  }

  task_base(task_base&& rhs) noexcept { *this = std::move(rhs); }

  task_base& operator=(task_base&& rhs) noexcept {
    handle_ = rhs.handle_;
    fu_ = std::move(rhs.fu_);
    typ_ = rhs.typ_;
    rhs.handle_ = nullptr;
    return *this;
  }

  promise<Tp>& get_promise() const { return handle_.promise(); }

  std::coroutine_handle<promise<Tp>> handle_;
  std::future<Tp> fu_;
  task_type typ_;
};
}  // namespace details_

template <class Tp>
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
  friend struct details_::promise_base;

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
  template <class Tp>
  friend struct details_::final_suspender;

  template <class Tp>
  void schedule(std::coroutine_handle<Tp> handle) {
    if constexpr (is_promise_v<Tp>) {
      handle.promise().suspend_on_final_ = false;
      handle.promise().scheduler_ = this;
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

template <class Tp>
struct async_awaiter : public coro::task<Tp> {
  bool await_ready() const noexcept {
    using namespace std::chrono_literals;
    return this->wait_for(0s) == std::future_status::ready;
  }

  void await_suspend(std::coroutine_handle<> h) {
    assert(this->typ_ == task_type::async);
    promise<Tp>& promise = this->get_promise();
    {
      std::unique_lock l(promise.ctx_mu_);
      promise.ctx_wait_que_.push(h);
    }
    if (promise.scheduler_) {
      promise.scheduler_->schedule(static_cast<task<Tp>&>(*this));
    } else {
      while (!this->done()) {
        this->resume();
      }
      this->destroy();
    }
  }

  Tp await_resume() { return this->get(); }
};

template <class Tp>
struct details_::final_suspender {
  constexpr bool await_ready() const noexcept { return false; }

  bool await_suspend(std::coroutine_handle<>) const noexcept {
    std::unique_lock l(self->ctx_mu_);
    auto wait_que = std::move(self->ctx_wait_que_);
    auto sch = self->scheduler_;
    bool suspend_on_final = self->suspend_on_final_;
    // Current coroutine might be destroyed by the coroutines resumed in this
    // function. Promise object (which is stored in coroutine frame) is not
    // allowed to use.
    while (!wait_que.empty()) {
      auto handle = wait_que.front();
      wait_que.pop();
      if (sch) {
        sch->schedule(handle);
      } else {
        handle.resume();
      }
    }
    return suspend_on_final;
  }

  constexpr void await_resume() const noexcept {}

  promise_base<Tp>* self;
};

template <class Tp>
template <class Up>
inline async_awaiter<Up> details_::promise_base<Tp>::await_transform(
    task<Up> t) noexcept {
  t.get_promise().scheduler_ = scheduler_;
  t.typ_ = task_type::async;
  return async_awaiter<Up>{std::move(t)};
}

template <class Tp>
inline details_::final_suspender<Tp>
details_::promise_base<Tp>::final_suspend() noexcept {
  return {this};
}

}  // namespace coro