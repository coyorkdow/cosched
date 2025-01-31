# cosched

A simple c++20 header-only coroutine scheduler.

Let's start from several examples.

# Example

## Recursive call

The following snippet shows a recursive coroutine which calculate fibonacci number in a most naive way.
We can `co_await` a `task<Tp>` object and get its result. If the current coroutine is running in a scheduler,
`co_await` will lead an asynchronous invocation and the caller coroutine will be suspended until the callee coroutine is finished.
However, we can also synchronized a coroutine by `task<Tp>::get`, which allows us call a coroutine from a normal function.
```c++
#include "cosched.hpp"

coro::task<int> fibonacci(int n) {
  if (n == 0 || n == 1) {
    co_return n;
  }
  co_return co_await fibonacci(n - 1) + co_await fibonacci(n - 2);
  
}

int main() {
  coro::task<int> fib = fibonacci(5);
  fib.get(); // result is 5
}
```

## Run in parallel

With a scheduler we can run multiple coroutines simultaneously.
The following snippet shows a task receives the results from two delayed subtasks.
Each subtask will spend 1 second to return. And the main task have to wait until both subtasks return their results.
The main task also spends 1 second to do its own stuff which is independent to the subtasks.
By scheduling this task with a scheduler that has three worker threads, we can get the final result in one second.
Because we can have two subtasks run in parallel.
```c++
coro::task<int> slow_response(int a, int b) {
  using namespace std::chrono_literals;
  auto request = [](int v) -> coro::task<int> {
    std::this_thread::sleep_for(1s);
    co_return v;
  };
  coro::task<int> resp1 = co_await coro::this_scheduler::parallel(request(a));
  coro::task<int> resp2 = co_await coro::this_scheduler::parallel(request(b));
  std::this_thread::sleep_for(1s);
  co_return co_await std::move(resp1) + co_await std::move(resp2);
}

int main() {
  coro::static_thread_pool pool(3);
  coro::task<int> resp = pool.schedule(slow_response(1, 2));
  resp.get(); // result is 3 
}
```

## Asynchronous timer

In the previous example, we mocked a 1ms latency request by sleeping the whole thread. We also proved the asynchronous timer that won't block any thread. It must be used with a scheduler.
The following snippet shows another mocking scenario that reading several files from the disk and merge theirs content. We use the async timer to mock the read file latency.
We can read the files simultaneously even our scheduler has only one worker thread.
```c++
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

int main() {
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
    try {
      file1 = co_await std::move(t1);
      file2 = co_await std::move(t2);
      file3 = co_await std::move(t3);
    } catch (const std::exception& e) {
    }
    co_return file1 + file2 + file3 + file4;
  };
  auto t = pool.schedule(process_file_task());
  t.get(); // it will only take 70ms to complete the task.
}
```

# Key Design

In this chapter I will introduce how this tiny scheduler works in behind. It involves the core concepts of the c++20 coroutine.

The coroutine is represented by the `task` object. A coroutine function should return `task` and contain `co_await` or `co_return` (`co_yield` is not supported).
Assume we have a function `foo` with return type `Tp`, the first step that rewrite it in coroutine is to change the return type to `task<Tp>`.

`task<Tp` is somehow very similar to the `std::future<Tp>`. We can use `get()` method to retrieve it result, which is a synchronized (and maybe blocking) call. But unlike
`std::future`, coroutine also supports asynchronous invocation.
Under a scheduler context (which means we are running coroutines in a scheduler), if we call another coroutine `bar` in coroutine `foo` by `co_await`ing it, then the scheduler
could pause `foo` immediately and switch to `bar`. The `foo` will be resumed upon the completion of the `bar`. Once it is resumed, it will retrieve the return value of `bar` without blocking,
and continues the following codes. It seems like a thread switching, except everything is happened in the user mode. And it is possible that both two coroutines are running in a same thread
despite they are keeping switch in and switch out.

```c++
task<Tp> foo() {
  // By calling `bar()` a new coroutine is created.
  // co_await it will suspend the current coroutine until the new coroutine returns.
  auto v = co_await bar(); // Suspend before the co_await expression returns.
                           // When it is resumed, `v` will have the returned value from `bar`.
  // continue running
  co_return ...; // Finish this task.
                 // If this coroutine is also invoked by another coroutine,
                 // then the completion of `foo` will also lead another resumption.
}
```

A `task` has two different types: it is either `deferred` or `async`. This is also very similar to the `std::launch`:
a deferred task is a lazy-evaulation function, it won't run untill we try to get its result. On the other hand, an async task is a task that can be executed by a scheduler in the background.

`static_thread_pool` is our coroutine scheduler. Just as its name describes, it's a simple thread pool that not much different than other thread pool implementations.
A `static_thread_pool` object is always associated with a time manager. The time manager manages the timer tasks (e.g., `this_scheduler::sleep_for`).
It is implemented by a priority queue which always returns the task with the smallest timestamp.

**Awaiter** specifies the behaviour of how the scheduler switches coroutines. There are five different kind of awaiters in cosched. They are
- `always_awaiter` Always suspends the current coroutine and puts it to the end of the scheduler's task queue.
- `async_awaiter` is the most common awaiter. It appears when we invoke another coroutine (i.e., the callee) from the current coroutine (i.e., the caller).
  It won't suspend if the callee has finished (which means we can retrieve its result immediately), otherwise, it will suspend the caller and make it as the callee's "wait coroutine".
  Once a coroutine is finished, its `wait coroutine` will be back to the scheduler.
- `parallel_awaiter` is similar to the `async_awaiter`, but it never suspend the current coroutine. If the scheduler has two or more worker threads, these two coroutines can run in parallel.
- `final_awaiter` is the awaiter which returned by `final_suspend()` of the promise object. It retrieves the coroutine's "wait coroutine" and puts it back to the scheduler's task que.
- `condition_awaiter` is a class template. Its behaviour can be customized. Our timer, latch, and mutex all utilize it.
