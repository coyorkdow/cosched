# cosched

A simple c++20 coroutine scheduler with only single header.

Let's start from several examples.

## Example: Recursive call

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

## Example: Run in parallel

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
