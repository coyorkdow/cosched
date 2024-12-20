#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <iostream>
#include <mutex>
#include <ostream>
#include <set>
#include <string>
#include <thread>

#include "cosched.hpp"

std::set<uint64_t> tids;
std::mutex tid_mu;

coro::task<void> print_tid() {
  std::cout << "thread id is " << std::this_thread::get_id() << '\n';
  {
    std::unique_lock l(tid_mu);
    tids.insert(std::hash<std::thread::id>{}(std::this_thread::get_id()));
  }
  co_return;
}

coro::task<int> fibonacci(int n) {
  if (n == 0 || n == 1) {
    co_await print_tid();
    co_return n;
  }
  co_return co_await fibonacci(n - 1) + co_await fibonacci(n - 2);
}

coro::task<std::string> echo(std::string v) {
  std::cout << v << '\n';
  co_return v;
}

coro::task<int> slow_response(int a, int b) {
  using namespace std::chrono_literals;
  auto request = [](int v) -> coro::task<int> {
    std::this_thread::sleep_for(1s);
    co_return v;
  };
  coro::task<int> resp1 = co_await coro::this_scheduler::parallel(request(a));
  coro::task<int> resp2 = co_await coro::this_scheduler::parallel(request(b));
  std::this_thread::sleep_for(1s);
  auto immediate = co_await coro::this_scheduler::parallel(
      []() -> coro::task<void> { co_return; }());
  co_await std::move(immediate);
  co_return co_await std::move(resp1) + co_await std::move(resp2);
}

TEST(CoroRunTest, Fib) {
  auto fib = fibonacci(5);
  fib.wait();
  EXPECT_EQ(5, fib.get());
}

TEST(CoroRunTest, Async) {
  auto f = echo("abc");

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
    // std::cout << f.get();
    EXPECT_EQ("abc", f.get());
    std::cout << "sub thread exit\n";
  });

  b.get();
}

TEST(StaticThreadPoolTest, Fib) {
  coro::static_thread_pool pool(3);
  tids.clear();
  EXPECT_EQ(55, pool.schedule(fibonacci(10)).get());
  EXPECT_EQ(3, tids.size());
}

TEST(StaticThreadPoolTest, Parallel) {
  coro::static_thread_pool pool(3);
  auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(3, pool.schedule(slow_response(1, 2)).get());
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);
  std::cout << "elapsed time: " << elapsed.count() << "ms\n";
  EXPECT_LE(elapsed.count(), 1100);
}

TEST(StaticThreadPoolTest, Yield) {
  auto yield_some = [](int n) -> coro::task<std::string> {
    while (n--) {
      co_await coro::this_scheduler::yield;
    }
    co_return "complete";
  };
  EXPECT_EQ("complete", yield_some(10).get());
  int n = 0;
  for (auto h = yield_some(10).release_coroutine_handle();
       !h.done() || (h.destroy(), false); h.resume()) {
    n++;
  }
  EXPECT_EQ(11, n);

  coro::static_thread_pool pool(1);
  auto task = pool.schedule(yield_some(10));
  EXPECT_EQ("complete", task.get());
}

TEST(StaticThreadPoolTest, Latch) {
  using namespace std::chrono_literals;
  std::atomic<int> cnt{0};
  coro::latch l(2);
  auto count_up = [](std::atomic<int>& cnt, coro::latch& l) -> coro::task<> {
    co_await l;
    cnt.fetch_add(1);
    co_return;
  };

  coro::static_thread_pool pool(1);
  pool.schedule(count_up(cnt, l));
  pool.schedule(count_up(cnt, l));
  std::this_thread::sleep_for(5ms);
  EXPECT_EQ(0, cnt.load());
  l.count_down();
  EXPECT_EQ(0, cnt.load());
  l.count_down();
  std::this_thread::sleep_for(5ms);
  EXPECT_EQ(2, cnt.load());
}
