#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <iostream>
#include <latch>
#include <mutex>
#include <ostream>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

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
  coro::async_latch l(2);
  auto count_up = [](std::atomic<int>& cnt,
                     coro::async_latch& l) -> coro::task<> {
    co_await l;
    cnt.fetch_add(1);
    co_return;
  };

  coro::static_thread_pool pool(1);
  pool.schedule(count_up(cnt, l));
  pool.schedule(count_up(cnt, l));
  std::this_thread::sleep_for(500ms);
  EXPECT_EQ(0, cnt.load());
  l.count_down();
  EXPECT_EQ(0, cnt.load());
  l.count_down();
  std::this_thread::sleep_for(5ms);
  EXPECT_EQ(2, cnt.load());
}

TEST(TimerTest, NoScheduler) {
  using namespace std::chrono_literals;
  std::vector<int64_t> tss;
  auto waiting = [&](std::chrono::milliseconds ms) -> coro::task<> {
    co_await coro::this_scheduler::sleep_for(ms);
    int64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    std::cout << "thread id is " << std::this_thread::get_id()
              << ", timepoint is " << timestamp << '\n';
    tss.push_back(timestamp);
    co_return;
  };
  waiting(3ms).get();
  waiting(5ms).get();
  waiting(10ms).get();
  ASSERT_EQ(3, tss.size());
  EXPECT_LE(tss[0] + 4, tss[1]);
  EXPECT_GE(tss[0] + 6, tss[1]);
  EXPECT_LE(tss[1] + 9, tss[2]);
  EXPECT_GE(tss[1] + 11, tss[2]);
}

TEST(TimerTest, WithScheduler) {
  using namespace std::chrono_literals;
  std::vector<int64_t> tss;
  // tids.clear();
  auto waiting = [&](std::chrono::milliseconds ms) -> coro::task<> {
    co_await coro::this_scheduler::sleep_for(ms);
    int64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    co_await print_tid();
    std::cout << "timepoint is " << timestamp << '\n';
    tss.push_back(timestamp);
    co_return;
  };
  coro::static_thread_pool pool(1);
  auto w1 = pool.schedule(waiting(3ms));
  auto w2 = pool.schedule(waiting(5ms));
  auto w3 = pool.schedule(waiting(10ms));
  w1.wait();
  w2.wait();
  w3.wait();
  ASSERT_EQ(3, tss.size());
  EXPECT_LE(tss[0] + 1, tss[1]);
  EXPECT_GE(tss[0] + 3, tss[1]);
  EXPECT_LE(tss[1] + 4, tss[2]);
  EXPECT_GE(tss[1] + 6, tss[2]);
}

TEST(TimerTest, WithScheduler2) {
  using namespace std::chrono_literals;

  struct MockFileReader {
    std::map<std::string, std::pair<int, std::string>> mock_confs;
    void SetMock(const std::string& path, int delay_in_ms,
                 std::string content) {
      mock_confs.emplace(
          std::piecewise_construct, std::forward_as_tuple(path),
          std::forward_as_tuple(delay_in_ms, std::move(content)));
    }
    coro::task<std::string> Read(std::string path) const {
      auto it = mock_confs.find(path);
      if (it == mock_confs.end()) {
        throw std::runtime_error("not found");
      }
      co_await coro::this_scheduler::sleep_for(
          std::chrono::milliseconds(it->second.first));
      co_return it->second.second;
    }
  };

  MockFileReader r;
  r.SetMock("/opt/tiger/a", 50, "content a,");
  r.SetMock("/home/youtao/b", 60, "content b,");
  r.SetMock("/usr/local/bin/c", 70, "content c");

  auto process_file_task = [&]() -> coro::task<std::string> {
    using namespace coro;
    std::string file1, file2, file3, file4;
    auto t1 = co_await this_scheduler::parallel(r.Read("/opt/tiger/a"));
    auto t2 = co_await this_scheduler::parallel(r.Read("/home/youtao/b"));
    auto t3 = co_await this_scheduler::parallel(r.Read("/usr/local/bin/c"));
    auto t4 = co_await this_scheduler::parallel(r.Read("/dev/null"));
    try {
      file1 = co_await std::move(t1);
      file2 = co_await std::move(t2);
      file3 = co_await std::move(t3);
      file4 = co_await std::move(t4);
    } catch (const std::exception& e) {
      std::cout << "there is a file that cannot be found\n";
      EXPECT_STREQ("not found", e.what());
    }
    co_return file1 + file2 + file3 + file4;
  };
  coro::static_thread_pool pool(1);
  auto start = std::chrono::steady_clock::now();
  auto t = pool.schedule(process_file_task());
  EXPECT_EQ("content a,content b,content c", t.get());
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);
  EXPECT_LE(69, elapsed.count());
  EXPECT_GE(71, elapsed.count());
}

TEST(MutexTest, Basic) {
  using namespace std::chrono_literals;
  coro::static_thread_pool scheduler(3);
  std::vector<int> v;
  std::latch l(5);
  coro::async_mutex mu;

  auto delayed_task = [&l]() -> coro::task<> {
    co_await coro::this_scheduler::sleep_for(100ms);
    l.count_down();
  };
  auto push_task = [&]() -> coro::task<> {
    co_await mu.lock();
    std::cout << "push back task begin, timestamp="
              << std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now().time_since_epoch())
                     .count()
              << '\n';
    v.push_back(v.size());
    co_await coro::this_scheduler::sleep_for(10ms);
    mu.unlock();
  };

  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 5; i++) {
    scheduler.schedule(delayed_task());
    scheduler.schedule(push_task());
  }
  l.wait();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);
  EXPECT_EQ(5, v.size());
  EXPECT_LE(99, elapsed.count());
  EXPECT_GE(101, elapsed.count());
}