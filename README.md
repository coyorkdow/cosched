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

# Key Design

In this chapter I will introduce how this tiny scheduler works in behind. It involves the core concepts of the c++20 coroutine.

The coroutine is represented by the `task<>` object. A coroutine function should return `task<>` and contain `co_await` or `co_return` (`co_yield` is not supported).
Assume we have a function `foo` with return type `Tp`, the first step that rewrite it in coroutine is to change the return type to `task<Tp>`.

`task<Tp` is somehow very similar to the `std::future<Tp>`. We can use `get()` method to retrieve it result, which is a synchronized (and maybe blocking) call. But unlike
`std::future`, coroutine also supports asynchronous invocation.
Under a scheduler context (which means we are running coroutines in a scheduler), if we call another coroutine `bar` in coroutine `foo` by `co_await`ing it, then the scheduler
could pause `foo` immediately and switch to `bar`. The `foo` will be resumed upon the completion of the `bar`. Once it is resumed, it will retrieve the return value of `bar` without blocking,
and continues the following codes. It seems like a thread switching, except everything is happened in the user mode. And it is possible that both two coroutines are running in a same thread
despite they are keeping switch in and switch out.

```c++
task<Tp> foo() {
  // By calling `bar()` a new coroutine is created. co_await it will suspend the current coroutine until the new coroutine returns.
  auto v = co_await bar(); // suspend before the co_await expression returns. When it is resumed, `v` will have the returned value from `bar`.
  // continue running
  co_return ...; // Finish this task. If this coroutine is also invoked by another coroutine, then the completion of `foo` will also lead another resumption.
}
```

