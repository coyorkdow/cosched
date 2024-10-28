
#include <iostream>

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

int main() { std::cout << fibonacci(10).get() << '\n'; }