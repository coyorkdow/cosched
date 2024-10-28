#pragma once
#include <cassert>
#include <coroutine>
#include <cstddef>
#include <future>
#include <thread>
#include <type_traits>
#include <vector>

namespace coro {

class static_thread_pool {
 public:
  explicit static_thread_pool(size_t n) {}

 private:
  std::vector<std::thread> ths_;
};

template <class Tp>
  requires(!std::is_reference_v<Tp>)
struct task_run_to_completion;

template <class Tp>
  requires(!std::is_reference_v<Tp>)
class task : protected std::future<Tp> {
 public:
  class promise_type : std::promise<Tp> {
   public:
    task get_return_object() noexcept {
      return std::coroutine_handle<promise_type>::from_promise(*this);
    }

    std::suspend_always initial_suspend() const noexcept { return {}; }
    std::suspend_always final_suspend() const noexcept { return {}; }

    template <class Awaiter>
    void await_transform(Awaiter) = delete;

    template <class R>
    inline task_run_to_completion<R> await_transform(task<R>) noexcept;

    void return_value(const Tp& value) noexcept(
        std::is_nothrow_copy_constructible_v<Tp>) {
      this->set_value(value);
    }

    void return_value(Tp&& value) noexcept(
        std::is_nothrow_move_constructible_v<Tp>) {
      this->set_value(std::move(value));
    }

    void unhandled_exception() noexcept {
      this->set_exception(std::current_exception());
    }

   private:
    friend class task;
  };

  using std::future<Tp>::valid;
  using std::future<Tp>::wait;
  using std::future<Tp>::wait_for;
  using std::future<Tp>::wait_until;

  Tp get() {
    while (!handle_.done()) {
      handle_.resume();
    }
    handle_.destroy();
    return std::future<Tp>::get();
  }

 protected:
  task(std::coroutine_handle<promise_type> h)
      : std::future<Tp>(h.promise().get_future()), handle_(h) {}

  std::coroutine_handle<promise_type> handle_;
};

template <>
class task<void> : protected std::future<void> {
 public:
  class promise_type : std::promise<void> {
   public:
    task get_return_object() noexcept {
      return std::coroutine_handle<promise_type>::from_promise(*this);
    }

    std::suspend_always initial_suspend() const noexcept { return {}; }
    std::suspend_always final_suspend() const noexcept { return {}; }

    template <class Awaiter>
    void await_transform(Awaiter) = delete;

    template <class R>
    inline task_run_to_completion<R> await_transform(task<R>) noexcept;

    void return_void() noexcept { this->set_value(); }

    void unhandled_exception() noexcept {
      this->set_exception(std::current_exception());
    }

   private:
    friend class task;
  };

  using std::future<void>::valid;
  using std::future<void>::wait;
  using std::future<void>::wait_for;
  using std::future<void>::wait_until;

  void run_to_completion() {
    while (!handle_.done()) {
      handle_.resume();
    }
    handle_.destroy();
    std::future<void>::get();
  }

 protected:
  task(std::coroutine_handle<promise_type> h)
      : std::future<void>(h.promise().get_future()), handle_(h) {}

  std::coroutine_handle<promise_type> handle_;
};

template <class Tp>
  requires(!std::is_reference_v<Tp>)
template <class R>
inline task_run_to_completion<R> task<Tp>::promise_type::await_transform(
    task<R> t) noexcept {
  return task_run_to_completion<R>{std::move(t)};
}

template <class Tp>
  requires(!std::is_reference_v<Tp>)
struct task_run_to_completion : public coro::task<Tp> {
  bool await_ready() const noexcept {
    using namespace std::chrono_literals;
    return this->wait_for(0s) != std::future_status::timeout;
  }

  bool await_suspend(std::coroutine_handle<>) const {
    while (!this->handle_.done()) {
      this->handle_.resume();
    }
    this->handle_.destroy();
    return false;
  }

  Tp await_resume() { return std::future<Tp>::get(); }
};

}  // namespace coro