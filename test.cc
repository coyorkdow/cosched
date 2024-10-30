
#include <iostream>
#include <thread>
#include <type_traits>

#include "promise.hpp"

coro::task<void> print_tid() {
  std::cout << "thread id is " << std::this_thread::get_id() << '\n';
  co_return;
}

coro::task<int> fibonacci(int n) {
  if (n == 0 || n == 1) {
    co_await print_tid();
    co_return n;
  }
  co_return co_await fibonacci(n - 1) + co_await fibonacci(n - 2);
}

coro::task<std::string> foo(std::string v) {
  std::cout << v << '\n';
  co_return v;
}

int main() {
  auto fib = fibonacci(1);
  fib.wait();
  std::cout << fib.get() << '\n';

  auto f = foo("abc");

  auto b = [](auto f) -> coro::task<void> {
    using namespace std::chrono_literals;
    std::this_thread::sleep_for(1s);
    while (!f.done()) {
      f.resume();
      co_await coro::this_scheduler::yield;
    }
    f.destroy();
    co_return;
  }(f.release_coroutine_handle());

  std::jthread th([f = std::move(f)]() mutable {
    std::cout << f.get();
    std::cout << "\nsub thread exit\n";
  });

  b.get();
  static_assert(!std::is_copy_constructible_v<decltype(f)>);

  coro::static_thread_pool pool(3);
  std::cout << pool.schedule(fibonacci(10)).get() << std::endl;
}