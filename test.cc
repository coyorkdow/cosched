
#include <iostream>
#include <thread>
#include <type_traits>

#include "promise.hpp"

coro::task<void> do_nothing() {
  std::cout << "I don't want to do anything!\n";
  co_return;
}

coro::task<int> fibonacci(int n) {
  if (n == 0 || n == 1) {
    co_await do_nothing();
    co_return n;
  }
  co_return co_await fibonacci(n - 1) + co_await fibonacci(n - 2);
}

coro::task<std::string> foo(std::string v) {
  std::cout << v << '\n';
  co_return v;
}

int main() {
  auto fib = fibonacci(10);
  fib.wait();
  std::cout << fib.get() << '\n';

  auto f = foo("abc");
  auto r = coro::make_async_task(f);

  std::jthread th([r = std::move(r)] {
    std::cout << r.get();
    std::cout << "\nsub thread exit\n";
  });
  auto b = [](auto f) -> coro::task<void> {
    using namespace std::chrono_literals;
    std::this_thread::sleep_for(1s);
    while (!f.done()) {
      f.resume();
      co_await coro::this_scheduler::suspend;
    }
    f.wait();
    co_return;
  }(std::move(f));
  b.get();
  static_assert(!std::is_copy_constructible_v<decltype(f)>);
}