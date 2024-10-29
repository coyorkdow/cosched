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
#include <vector>

namespace coro {

template <class Tp>
  requires(!std::is_reference_v<Tp>)
struct run_to_done_awaiter;

class static_thread_pool;

struct suspend_t : std::suspend_always {};

namespace this_scheduler {
inline suspend_t suspend;
}  // namespace this_scheduler

template <class Tp>
  requires(!std::is_reference_v<Tp>)
class task;

enum class task_type {
  deferred,
  async,
  async_runner,
};

class task_error : public std::logic_error {
 public:
  const char* what() const noexcept override {
    return std::logic_error::what();
  }
  explicit task_error(const std::string& msg)
      : std::logic_error("coro::task_error: " + msg) {}
};

namespace details_ {
template <class Tp>
class task_base;

template <class Tp>
struct promise_base : std::promise<Tp> {
  std::suspend_always initial_suspend() const noexcept { return {}; }
  std::suspend_always final_suspend() const noexcept { return {}; }

  template <class Awaiter>
  void await_transform(Awaiter) = delete;

  suspend_t await_transform(suspend_t) { return this_scheduler::suspend; }

  template <class R>
  inline run_to_done_awaiter<R> await_transform(task<R>) noexcept;

  void unhandled_exception() noexcept {
    this->set_exception(std::current_exception());
  }

  promise_base() : fu_(this->get_future()) {}

  std::future<Tp> fu_;
};
}  // namespace details_

template <class Tp>
  requires(!std::is_reference_v<Tp>)
class promise : public details_::promise_base<Tp> {
 public:
  task<Tp> get_return_object() noexcept;

  void return_value(const Tp& value) noexcept(
      std::is_nothrow_copy_constructible_v<Tp>) {
    this->set_value(value);
  }

  void return_value(Tp&& value) noexcept(
      std::is_nothrow_move_constructible_v<Tp>) {
    this->set_value(std::move(value));
  }

 private:
  template <class Rp>
  friend class details_::task_base;

  using details_::promise_base<Tp>::fu_;
};

template <>
class promise<void> : public details_::promise_base<void> {
 public:
  inline task<void> get_return_object() noexcept;

  void return_void() noexcept { this->set_value(); }

 private:
  template <class Rp>
  friend class details_::task_base;

  using details_::promise_base<void>::fu_;
};

namespace details_ {

template <class Tp>
class task_base {
 public:
  bool valid() const noexcept { return handle_ && get_future().valid(); }
  void wait() const {
    if (typ_ == task_type::deferred) {
      run_to_done();
    } else {
      get_future().wait();
    }
  }

  template <typename Rep, typename Period>
  std::future_status wait_for(
      const std::chrono::duration<Rep, Period>& rel) const {
    if (typ_ == task_type::deferred) {
      return std::future_status::deferred;
    }
    return get_future().wait_for(rel);
  }

  Tp get() const {
    if (typ_ == task_type::async_runner) {
      throw task_error("get value from an async runner task is not allowed");
    }
    wait();
    if constexpr (std::is_same_v<Tp, void>) {
      get_future().get();
      handle_.destroy();
    } else {
      Tp res = get_future().get();
      handle_.destroy();
      return res;
    }
  }

  void resume() const { handle_.resume(); }
  bool done() const noexcept { return handle_.done(); }

 protected:
  std::future<Tp>& get_future() const { return handle_.promise().fu_; }

  void run_to_done() const {
    while (!done()) {
      this->handle_.resume();
    }
  }

  task_base(std::coroutine_handle<promise<Tp>> h = nullptr,
            task_type t = task_type::deferred) noexcept
      : handle_(h), typ_(t) {}

  std::coroutine_handle<promise<Tp>> handle_;
  task_type typ_;
};

}  // namespace details_

template <class Tp>
  requires(!std::is_reference_v<Tp>)
class task : public details_::task_base<Tp> {
 public:
  using promise_type = promise<Tp>;

  task() noexcept {}
  task(task&& rhs) noexcept { *this = std::move(rhs); }
  task& operator=(task&& rhs) noexcept {
    this->handle_ = rhs.handle_;
    this->typ_ = rhs.typ_;
    rhs.handle_ = nullptr;
    return *this;
  }

 protected:
  task(std::coroutine_handle<promise_type> h, task_type t)
      : details_::task_base<Tp>(h, t) {}

  template <class Rp>
    requires(!std::is_reference_v<Rp>)
  friend class promise;

  template <class Rp>
  friend task<Rp> make_async_task(task<Rp>&) noexcept;
};

template <>
class task<void> : public details_::task_base<void> {
 public:
  using promise_type = promise<void>;

  task() noexcept {}
  task(task&& rhs) noexcept { *this = std::move(rhs); }
  task& operator=(task&& rhs) noexcept {
    this->handle_ = rhs.handle_;
    this->typ_ = rhs.typ_;
    rhs.handle_ = nullptr;
    return *this;
  }

 protected:
  task(std::coroutine_handle<promise_type> h, task_type t)
      : details_::task_base<void>(h, t) {}

  template <class Rp>
    requires(!std::is_reference_v<Rp>)
  friend class promise;

  template <class Rp>
  friend task<Rp> make_async_task(task<Rp>&) noexcept;
};

template <class Tp>
task<Tp> make_async_task(task<Tp>& t) noexcept {
  t.typ_ = task_type::async_runner;
  return task<Tp>(t.handle_, task_type::async);
}

template <class Tp>
  requires(!std::is_reference_v<Tp>)
task<Tp> promise<Tp>::get_return_object() noexcept {
  return task<Tp>(std::coroutine_handle<promise>::from_promise(*this),
                  task_type::deferred);
}

inline task<void> promise<void>::get_return_object() noexcept {
  return task<void>(std::coroutine_handle<promise>::from_promise(*this),
                    task_type::deferred);
}

template <class Tp>
template <class R>
inline run_to_done_awaiter<R> details_::promise_base<Tp>::await_transform(
    task<R> t) noexcept {
  return run_to_done_awaiter<R>{std::move(t)};
}

template <class Tp>
  requires(!std::is_reference_v<Tp>)
struct run_to_done_awaiter : public coro::task<Tp> {
  bool await_ready() const noexcept {
    using namespace std::chrono_literals;
    return this->wait_for(0s) != std::future_status::timeout;
  }

  bool await_suspend(std::coroutine_handle<>) const {
    this->run_to_done();
    return false;
  }

  Tp await_resume() { return this->get(); }
};

class static_thread_pool {
 public:
  explicit static_thread_pool(size_t n) : exit_(false) {}

  template <class Tp>
  task<Tp> emplace(task<Tp> t) {
    {
      std::unique_lock l(mu_);
      // que_.emplace(Args &&args...)
    }
  }

 private:
  void worker_routine() {
    while (true) {
      task<void> t;
      {
        std::unique_lock l(mu_);
        con_.wait(l, [&] { return exit_ || !que_.empty(); });
        t = std::move(que_.front());
        que_.pop();
      }
    }
  }

  std::queue<task<void>> que_;
  std::mutex mu_;
  std::condition_variable con_;
  std::vector<std::thread> ths_;
  bool exit_;
};

}  // namespace coro