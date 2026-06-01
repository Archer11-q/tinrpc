# 04 — RPC 线程池：任务队列 + Worker 线程

---

## 一、这一层解决什么问题？

### 当前状态：回调跑在 IO 线程里

v0.3 的数据流：

```
socket recv → Connection::OnRead → Buffer::Append
  → Buffer::TryPopFrame → ProtocolFrame::Decode → FrameCallback  ← 在 IO 线程！
```

`FrameCallback` 是同步调用的——在 `Connection::OnRead` 里面直接执行，而 `OnRead` 是由 `EventLoop` 的 IO 线程在 `epoll_wait` 返回后调用的。

**这意味着一件危险的事**：如果业务逻辑耗时 100ms（比如查了一次数据库），`epoll_wait` 就被推迟了 100ms。在这 100ms 内，其他所有连接的数据到达了也得不到处理。

```
时间线：
│ epoll 通知 fd=5 可读
│ IO 线程: Connection::OnRead → 解码帧 → FrameCallback(执行 100ms 的业务逻辑)
│                                                                    ↑
│                                                    这 100ms 内 fd=8/9/10 的数据到了但没人处理
│     ……100ms 后……
│ IO 线程: epoll_wait 终于被调用 → 处理积压的事件
```

**一句话：IO 线程不应该做任何耗时操作。IO 线程只应该做 IO。** 这就是 Reactor 模式的核心原则。

### 线程池的职责

把业务逻辑的执行从 IO 线程**转移**到一组专门的工作线程上：

```
                     IO 线程                          工作线程
             ┌──────────────────┐            ┌──────────────────┐
             │  epoll_wait()    │            │  while(true) {   │
             │  ↓               │            │    task = 取任务() │
             │  OnRead → Decode │  ──放入──→ │    执行 task()    │
             │  ↓               │   任务队列  │  }               │
             │ 继续 epoll_wait  │            └──────────────────┘
             └──────────────────┘
```

IO 线程的任务是"尽快把数据收进来，打包成任务，扔进队列，马上回去继续收数据"。真正的业务计算交给工作线程慢慢跑。

---

## 二、核心模型：生产者-消费者

### 2.1 基本结构

```
        生产者（IO 线程）              消费者（Worker 线程 × N）
       ┌─────────────┐              ┌──────────┐ ┌──────────┐ ┌──────────┐
       │ 收到 Frame   │              │ Worker 0 │ │ Worker 1 │ │ Worker 2 │
       │ 封装成 task  │              │ 取任务()  │ │ 取任务()  │ │ 取任务()  │
       │  push 到队列 │              │ 执行()    │ │ 执行()    │ │ 执行()    │
       └──────┬───────┘              └────┬─────┘ └────┬─────┘ └────┬─────┘
              │                           │            │            │
              └───────────→  ┌────────────────────────┐ ←──────────┘
                             │  任务队列 (线程安全)     │
                             │  [task1][task2][task3]  │
                             └────────────────────────┘
```

**生产者**：IO 线程（`Connection::OnRead`）——解码出完整 Frame 后，封装成 task，push 到队列末尾。

**消费者**：N 个 worker 线程——每个线程在循环里做同一件事：从队列取 task → 执行 task → 取下一个。

### 2.2 为什么需要多个 Worker 而不仅仅一个？

一个 worker 处理不过来的场景：

```
IO 线程 2ms 就收到一帧（500 QPS）
Worker 处理一帧平均需要 8ms
→ 1 个 worker 只能处理 125 QPS
→ 任务队列越积越长，最终 OOM
```

N 个 worker 意味着 N 倍吞吐。当然 N 不是越大越好——CPU 核心数是物理上限，过了反而因为上下文切换变慢。

---

## 三、线程池核心组件

### 3.1 任务队列（线程安全）

任务队列是一个 **FIFO 队列**，需要支持：

- **push**（生产者放入任务）
- **pop（阻塞）**（消费者取出任务，队列空时阻塞等待）
- **线程安全**（多个生产者和多个消费者可以同时访问）

C++ 标准库提供了开箱即用的组合：`std::mutex` + `std::condition_variable`。

```cpp
// 任务类型：一个可以无参调用的函数对象
using Task = std::function<void()>;

std::queue<Task> queue_;         // 任务队列
std::mutex mutex_;               // 保护队列的互斥锁
std::condition_variable cv_;     // 条件变量：队列非空时唤醒 worker

// 生产者（IO 线程调用）
void Enqueue(Task task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(task));
    }
    cv_.notify_one();   // 唤醒一个等待的 worker
}

// 消费者（worker 线程调用）
Task Dequeue() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return !queue_.empty() || stop_; });
    // wait 做了什么：
    // 1. 条件不满足 → 释放锁 → 线程进入睡眠
    // 2. 被 notify_one 唤醒 → 重新获取锁 → 检查条件
    // 3. 条件满足 → 返回（持有锁）
    // 4. 函数返回后 unique_lock 析构 → 释放锁
    if (stop_ && queue_.empty()) return nullptr;  // 关闭信号
    Task task = std::move(queue_.front());
    queue_.pop();
    return task;
}
```

### 3.2 condition_variable 详解（面试高频）

`std::condition_variable` 解决了"忙等"问题。

**没有条件变量时**，worker 只能轮询：

```cpp
// 忙等 — 浪费 CPU
while (true) {
    if (!queue_.empty()) break;  // 每秒检查上百万次，CPU 空转
}
```

**有条件变量后**，worker 在队列空时进入**睡眠**，不消耗 CPU。生产者放入任务后通过 `notify_one()` **唤醒**一个睡眠的 worker。内核负责这个唤醒机制。

```
Worker 视角：
  queue 空 → cv_.wait() → 线程睡眠（CPU 使用率 0%）
  ...等待中...
  queue 有数据 → 被 notify → 线程醒来 → 取任务 → 执行
```

`cv_.wait(lock, predicate)` 等价于：

```cpp
while (!predicate()) {
    cv_.wait(lock);  // 原子操作：释放锁 + 进入睡眠
    // 被唤醒后自动重新获取锁
}
```

**为什么用 while 而不是 if？** 因为有**虚假唤醒（spurious wakeup）**——操作系统可能在没有 `notify` 的情况下唤醒线程。用 `while` 确保唤醒后重新检查条件，条件不满足就继续睡。

### 3.3 Worker 线程生命周期

每个 worker 线程的主循环：

```cpp
void WorkerLoop() {
    while (true) {
        Task task = Dequeue();      // 阻塞等待任务
        if (task == nullptr) break; // nullptr 是关闭信号
        task();                     // 执行任务
    }
}
```

**启动**：构造函数中创建 N 个 `std::thread`，每个线程运行 `WorkerLoop`。

**关闭**：
1. 设置 `stop_ = true`
2. `cv_.notify_all()` 唤醒所有睡眠的 worker
3. 每个 worker 看到 `stop_ && queue_.empty()` 后退出循环
4. 主线程 `join()` 所有 worker 线程

---

## 四、在本项目中的集成点

### 4.1 修改 Connection：回调变为入队

v0.3 的 `Connection` 在解码出 Frame 后直接调 `frame_callback_`：

```cpp
// v0.3（当前）
if (frame_callback_) {
    frame_callback_(*frame);    // ← 在 IO 线程执行！
}
```

v0.4 需要改为：

```cpp
// v0.4
if (frame_callback_) {
    // 封装成 task，交给线程池，IO 线程立即返回
    auto frame_copy = std::move(*frame);        // 拷贝/移动 Frame
    thread_pool_->Enqueue([cb = frame_callback_, f = std::move(frame_copy)]() {
        cb(f);                                   // ← 在 worker 线程执行
    });
}
```

**关键点**：Frame 必须拷贝一份，不能直接传引用。因为引用指向的对象可能在 IO 线程下一次循环时被覆盖。

### 4.2 ThreadPool 的创建位置

ThreadPool 需要在 `Connection` 构造时可用。最自然的做法是：

- `main()` 中创建 ThreadPool
- 通过 `Acceptor` → 传递给每个 `Connection`
- 或者让 `Connection` 通过 `EventLoop` 间接持有 ThreadPool 引用

具体设计在实现前讨论。

### 4.3 关闭顺序

先停 EventLoop（不再有新事件），再停 ThreadPool（等正在执行的任务完成），最后 join worker 线程。

```
Stop EventLoop → 不再生成新 task
  → ThreadPool::Shutdown → drain 剩余任务
    → join 所有 worker
```

---

## 五、关键设计决策

### 5.1 任务粒度：一帧一个任务

在 v0.4 中，每个完整的 RPC 请求（一个 Frame）就是一个任务。这个粒度恰好合适：

- 太粗（多个 Frame 一个任务）：一个慢 Frame 阻塞同批次其他 Frame
- 太细（半帧一个任务）：拆分没有意义，Frame 是最小可处理单元

### 5.2 线程数：`std::thread::hardware_concurrency()`

默认创建等于 CPU 逻辑核心数的 worker 线程。这是"CPU 密集型任务"的标准做法。

对于 RPC 服务端（很多时候是 IO 密集型），线程数可以略多于 CPU 核数——但 v0.4 先不做这个微调，留在 v0.6 benchmark 阶段。

### 5.3 关停策略：等待所有已提交任务完成

调用 `Shutdown()` 后：
- 不再接受新任务（或接受但返回错误）
- 等待队列中所有已提交任务执行完毕
- 然后通知所有 worker 退出

**不选择"丢弃未完成任务"**——那会导致客户端请求丢失，RPC 框架不能这样做。

### 5.4 为什么不用无锁队列

MoodyCamel 等无锁队列性能更好，但：
- 引入外部依赖，违背"零外部依赖"原则
- 自实现无锁队列正确性极难保证（ABA 问题、内存序）
- `std::mutex` + `std::condition_variable` 对于学习项目完全足够
- 性能差异在 v0.6 benchmark 之前不关心

### 5.5 notify_one vs notify_all

`notify_one`：只唤醒**一个**等待的 worker。适合本场景——一个任务只需要一个 worker 处理。

`notify_all`：唤醒**所有**等待的 worker。用于关停场景——需要所有 worker 都醒来检查 `stop_` 标志。

---

## 六、新增文件清单（v0.4）

| 文件 | 作用 |
|------|------|
| `include/rpc/thread_pool.h` | ThreadPool 类声明 |
| `src/thread_pool.cpp` | ThreadPool 实现 |
| `tests/test_thread_pool.cpp` | 线程池单元测试 |
| `docs/04-thread-pool.md` | 本文档 |

**不新增但需修改**：
- `include/rpc/connection.h` — 持有 ThreadPool 指针或引用
- `src/connection.cpp` — 回调改为入队
- `src/acceptor.cpp` — 传递 ThreadPool 给 Connection
- `CMakeLists.txt` — 添加新源文件和测试 target

---

## 七、面试重点

这一层在面试中主要考察对并发编程基础的理解：

1. **生产者-消费者模型是什么？** — 解耦生产速率和消费速率，通过队列做缓冲。生产者（IO 线程）和消费者（worker 线程）各自独立运行，互不影响。

2. **`std::condition_variable` 的 `wait` 是怎么工作的？** — 原子地"释放锁 + 进入睡眠"，被 `notify` 后"醒来 + 重新获取锁 + 检查条件"。虚假唤醒、为什么用 while 不用 if。

3. **`notify_one` 和 `notify_all` 的区别？** — 一个唤醒一个线程，一个唤醒所有。什么场景用哪个。

4. **线程池怎么优雅关闭？** — 设 `stop_` 标志 → `notify_all` 唤醒所有 worker → worker 检查标志退出 → 主线程 `join` 等待。

5. **线程安全的三要素？** — 互斥（mutex）、同步（condition_variable / atomic）、正确的生命周期管理（join / detach）。

6. **为什么任务要拷贝 Frame 而不能传引用？** — IO 线程的栈变量，worker 线程不能持有指向 IO 线程栈的引用——生命周期不匹配。悬空引用（dangling reference）是 C++ 并发 bug 第一来源。

7. **`std::function` 的开销？** — 类型擦除有虚函数调用级别的开销（间接调用）。对 RPC 框架来说这个开销相对于网络 IO 可以忽略。如果有必要优化，可以在 v0.6 用模板替换为可移动的 callable。

---

## 八、与 v0.5 的关系

v0.4 的线程池是 v0.5 的**基础设施**：

```
v0.5 的 Stub（客户端）：
  Stub::Add(3, 5)
    → 序列化请求
    → 发送到服务端
    → 返回 std::future<Result>    ← 异步等待结果
    → ThreadPool 执行回调

v0.5 的 Dispatch（服务端）：
  Frame 到达 → ThreadPool::Enqueue
    → Worker 线程执行 registered_handler(Frame)
```

线程池让 v0.5 可以做到"客户端异步调用 + 服务端异步处理"，而不是同步阻塞等待。