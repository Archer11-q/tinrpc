# 03 — RPC 网络 IO 层：epoll + 非阻塞 IO + Reactor 模式

---

## 一、这一层解决什么问题？

### 当前状态：序列化层和协议帧层只是"数据加工厂"

v0.1 序列化层能把 `Add(3, 5)` 变成 14 字节的二进制数据。
v0.2 协议帧层能把这 14 字节包上帧头，变成一帧完整的网络消息。

**但这两层都不知道"怎么把数据发出去"和"怎么收到数据"。** 它们只处理字节序列，不知道 socket 是什么。

### 网络 IO 层的职责

把数据从内存搬到网络（send），把网络数据搬到内存（recv），并且做到：

- **同时处理多个连接** — 服务端不能一次只服务一个客户端
- **不阻塞主线程** — 一个慢客户端不能拖死其他所有客户端
- **高效利用 CPU** — 不能让线程在"等数据到达"时空转

**一句话：让服务端能用 epoll 管理数百个 TCP 连接，并在数据到达时自动触发对应的处理逻辑。**

---

## 二、核心问题：如何处理大量并发连接？

### 方案 A：一个连接一个线程（传统做法）

```cpp
while (true) {
    int client_fd = accept(server_fd, ...);
    std::thread([client_fd]() {
        // 在这个线程里处理这个客户端的所有请求
        while (recv(client_fd, buf, sizeof(buf), 0) > 0) { ... }
    }).detach();
}
```

**问题**：

- 1000 个连接 = 1000 个线程。每个线程默认栈 8MB，光栈内存就 8GB
- 线程上下文切换开销随连接数线性增长，CPU 大量时间花在切换上而非业务逻辑
- 大部分线程大部分时间在 `recv` 上阻塞（客户端不发数据时线程空等）

这个模型叫 **Thread-Per-Connection**，Nginx 出来之前 Apache 就这么干的。

### 方案 B：IO 多路复用（epoll）

**核心思想：用一个线程监听所有连接的读写事件，哪个连接有数据就处理哪个，没数据的连接不消耗 CPU。**

```
            ┌──────────────────────┐
            │     epoll_wait()     │
            │  "哪些 fd 有事件？"   │
            └──────┬───────────────┘
                   │
       ┌───────────┼───────────┐
       ▼           ▼           ▼
    fd=5        fd=8        fd=12
   (可读)      (可读)       (可写)
       │           │           │
   处理客户端A    处理客户端B    发送响应给客户端C
```

**C10k 问题（一万并发连接）** 就是靠 IO 多路复用解决的。

---

## 三、epoll 核心原理

### 3.1 三个系统调用

```c
int epoll_create1(0);                         // 创建一个 epoll 实例，返回 epoll fd
int epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev); // 把要监听的 fd 注册进去
int epoll_wait(epfd, events, maxevents, timeout); // 等待事件发生
```

### 3.2 epoll 和 select/poll 的本质区别

| | select | poll | epoll |
|------|--------|------|-------|
| 数据结构 | 固定 1024 的 fd_set | 动态数组 | 红黑树 + 就绪链表 |
| 查找就绪 fd | O(n) 遍历全部 fd | O(n) 遍历全部 fd | O(1) 只返回就绪的 fd |
| 每次调用 | 重新传入全部 fd 列表 | 重新传入全部 fd 列表 | fd 注册一次，持续有效 |
| 1000 个连接时 | 每次 wait 遍历 1000 个 | 每次 wait 遍历 1000 个 | 只处理有事件的 3 个 |

**epoll 赢在哪**：它把"监听哪些 fd"和"哪些 fd 有事件"分离了。fd 注册一次存在内核红黑树里，事件发生时内核把就绪 fd 加到链表上，`epoll_wait` 直接从链表取——**不遍历全部 fd。**

### 3.3 水平触发（Level Triggered）vs 边缘触发（Edge Triggered）

这是面试最高频考点。

**水平触发（LT，默认模式）**：

```
只要 fd 的读缓冲区有数据没读完，每次 epoll_wait 都会通知你。
```

```
你: epoll_wait()
内核: fd=5 可读！buffer 里有 100 字节
你: read 了 50 字节
你: epoll_wait()
内核: fd=5 还是可读！buffer 里还有 50 字节  ← 又通知你
你: read 了剩余 50 字节
你: epoll_wait()
内核: [安静，没数据了]
```

**边缘触发（ET，需设置 `EPOLLET`）**：

```
只在 fd 状态发生变化的那一刻通知一次。数据没读完不会再次通知。
```

```
你: epoll_wait()
内核: fd=5 可读！buffer 里有 100 字节  ← 只通知这一次
你: read 了 50 字节
你: epoll_wait()
内核: [一直阻塞，kernel 不会提醒你还有 50 字节没读！]
```

**ET 模式的核心要求**：收到通知后必须循环 read，直到返回 `EAGAIN`（表示缓冲区已空），否则那 50 字节就丢了——客户端不会再发新数据，内核不会再通知，服务器永远等不到。

```cpp
// ET 模式的正确读法：循环读到 EAGAIN
while (true) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
        // 处理这 n 字节
    } else if (n == -1 && errno == EAGAIN) {
        break;  // 缓冲区读空了，退出循环
    } else {
        // n == 0: 对端关闭连接
        // n == -1: 其他错误
        close(fd);
        break;
    }
}
```

**为什么用 ET 而不用 LT？**

- LT 模式下，同一事件可能被通知多次，增加了不必要的系统调用开销
- ET 强制你一次性读完所有数据，配合非阻塞 IO，性能更好
- 高性能服务器（Nginx、Redis）都用 ET 模式

**说了 ET 这么多优点，缺点呢？**

- 编程难度大。忘了读到 EAGAIN 就是 bug，而且很难复现（只在特定发包速率下才触发）
- ET 必须配合非阻塞 IO。如果 fd 是阻塞的，read 在读到 EAGAIN 之前可能阻塞住

---

## 四、非阻塞 IO

### 为什么必须非阻塞

在 ET + epoll 下，你收到"可读"通知后循环 read。如果 fd 是阻塞模式，最后一个 read（缓冲区已空）不会返回而是阻塞住整个线程——后面的 epoll_wait 没法执行，其他连接全部饿死。

**设置 socket 非阻塞**：

```cpp
int flags = fcntl(fd, F_GETFL, 0);
fcntl(fd, F_SETFL, flags | O_NONBLOCK);
```

非阻塞模式下，缓冲区空时 read 不会阻塞，而是立即返回 -1 并设 `errno = EAGAIN`（或 `EWOULDBLOCK`，两者同值）。

### `EAGAIN` 不是错误

```cpp
ssize_t n = read(fd, buf, size);
if (n == -1 && errno == EAGAIN) {
    // 这不是错误。只是"现在没数据，下次再来"。
    // 很正常，表示本次循环读完了一轮数据。
}
```

你的代码里看到 `EAGAIN` 不能 panic，不能打 error 日志，不能关连接——它只是"读完了，该退出 ET 循环了"的信号。

---

## 五、Reactor 模式

### 5.1 为什么需要 Reactor？

裸写 epoll 的代码是这样的：

```cpp
while (true) {
    int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
    for (int i = 0; i < n; i++) {
        if (events[i].data.fd == server_fd) {
            int client = accept(server_fd, ...);
        } else {
            char buf[4096];
            int len = read(events[i].data.fd, buf, sizeof(buf));
            // ... 处理数据 ...
        }
    }
}
```

写一两次还行，但你需要：
- 处理不同类型的连接（监听 socket、客户端 socket）
- 处理不同的事件（可读、可写、错误）
- 把数据拼成完整帧（v0.2 的 Buffer）
- 帧解析出来后调对应的业务逻辑

全塞在一个循环里会变成意大利面条。**Reactor 模式把这些关注点拆开。**

### 5.2 Reactor 三层结构

```
┌───────────────────────────────────────────┐
│               EventLoop                    │  ← 主循环：epoll_wait + 分发事件
│   while(true) { epoll_wait(...) → 分发给 Handler }
└──────────────┬────────────────────────────┘
               │ 事件到达
       ┌───────┼───────┐
       ▼               ▼
┌──────────────┐ ┌──────────────┐
│  Acceptor    │ │  Connection  │           ← EventHandler：处理具体 fd 的事件
│  处理新连接    │ │  处理客户端数据 │
└──────────────┘ └──────┬───────┘
                        │
                        ▼
              ┌─────────────────┐
              │    Buffer       │            ← 粘包/拆包（已有，v0.2）
              │    ProtocolFrame │
              └─────────────────┘
```

### 5.3 EventLoop 伪代码

```cpp
class EventLoop {
    int epfd_;                    // epoll 实例 fd
    std::map<int, EventHandler*> handlers_;  // fd → 处理器的映射

public:
    void Run() {
        while (running_) {
            epoll_event events[64];
            int n = epoll_wait(epfd_, events, 64, -1);  // -1 = 阻塞直到有事件

            for (int i = 0; i < n; i++) {
                int fd = events[i].data.fd;
                auto* handler = handlers_[fd];  // 查表找到这个 fd 的处理器

                if (events[i].events & EPOLLIN)   // 可读
                    handler->OnRead();
                if (events[i].events & EPOLLOUT)  // 可写
                    handler->OnWrite();
                if (events[i].events & (EPOLLERR | EPOLLHUP))  // 错误/挂断
                    handler->OnClose();
            }
        }
    }
};
```

**注意**：伪代码中 `epoll_wait` 的 timeout 设为 -1（永久阻塞）。这在单线程 Reactor 里是合理的——没事件时空等不消耗 CPU，新连接到来时 epoll 立即唤醒。

### 5.4 Acceptor：处理新连接

```cpp
class Acceptor {
    int listen_fd_;  // 监听 socket
    EventLoop* loop_;

    void OnRead() override {
        while (true) {
            int client_fd = accept(listen_fd_, nullptr, nullptr);
            if (client_fd == -1) {
                if (errno == EAGAIN) break;  // 所有新连接都 accept 了
                continue;  // 其他错误（如 EINTR）
            }

            // 将客户端 socket 设为非阻塞
            SetNonBlocking(client_fd);

            // 为新连接创建 Connection 对象，注册到 EventLoop
            auto* conn = new Connection(client_fd, loop_);
            loop_->Register(client_fd, conn);
        }
    }
};
```

**为什么 accept 也要循环**：ET 模式下，`listen_fd` 变为可读只通知一次。如果同时来了 3 个新连接你只 accept 了 1 个，剩下 2 个就丢了——和读数据一样，必须循环到 `EAGAIN`。

### 5.5 Connection：处理客户端数据

```cpp
class Connection {
    int fd_;
    Buffer read_buffer_;   // v0.2 的 Buffer：接收缓冲 + 帧切分

    void OnRead() override {
        // 1. 从 socket 读数据到临时缓冲区（ET 模式循环读到 EAGAIN）
        uint8_t tmp[4096];
        while (true) {
            ssize_t n = read(fd_, tmp, sizeof(tmp));
            if (n > 0) {
                read_buffer_.Append(tmp, n);
            } else if (n == -1 && errno == EAGAIN) {
                break;  // 本轮数据读完
            } else {
                OnClose();  // 错误或对端关闭
                return;
            }
        }

        // 2. 尝试从 Buffer 切出完整帧
        while (auto frame_bytes = read_buffer_.TryPopFrame()) {
            // 3. 解码帧
            auto frame = ProtocolFrame::Decode(*frame_bytes);
            if (frame) {
                // 4. 交给上层 Dispatch 处理（v0.5 实现）
                Dispatch(frame.value());
            }
        }
    }
};
```

**这是三层协作的精华**：

```
socket recv → Buffer::Append（累积字节）
           → Buffer::TryPopFrame（切出帧边界）
           → ProtocolFrame::Decode（解析字段）
           → Dispatch（调 C++ 函数）
```

每一层只做自己的事，层与层之间通过明确的数据接口连接。面试时这个图比你讲一百句都有说服力。

---

## 六、v0.3 实现范围

### 要做

| 类 | 职责 |
|----|------|
| `Socket` | 封装 socket()、bind()、listen()、accept()、SetNonBlocking()、close() |
| `EventLoop` | epoll 创建、注册/注销 fd、epoll_wait 事件循环 |
| `EventHandler` | 抽象基类，定义 `OnRead()`、`OnWrite()`、`OnClose()` 接口 |
| `Acceptor` | 继承 EventHandler，accept 新连接并创建 Connection |
| `Connection` | 继承 EventHandler，读写数据，对接 v0.2 Buffer |

### 不做

- 不实现 Write（发送响应）。v0.3 只做到能接收数据并解码成 Frame，发送在后续版本
- 不实现线程池。`Dispatch` 在 v0.3 是空壳（打印 Frame 字段），真正的业务调用在 v0.4-0.5
- 不实现客户端连接逻辑（RPC Client 在 v0.5）
- 不做 Linux 的 TCP_NODELAY、SO_REUSEADDR 等 socket option 微调（后续 benchmark 阶段再加）

### 测试方式

网络层测试分为两类：

**1. 单元级测试（独立可测）**

- `Socket` 类的创建、非阻塞设置、bind/listen
- `EventLoop` 的 fd 注册与注销
- 不需要真实客户端

**2. 集成测试（需要真实 TCP 连接）**

- 启动服务端，用 `telnet` 或一段测试用的客户端代码发数据
- 验证：服务端能 accept 连接 → 收到数据 → Buffer 切出完整帧 → ProtocolFrame 解码成功

---

## 七、关键设计决策

### 7.1 为什么不实现发送（先只读）

RPC 框架的发送相对简单——你知道要发多少字节、什么时候发完。而接收涉及粘包/拆包、事件驱动、ET 循环读——这些是面试的核心话题。**v0.3 集中精力把接收路径做得扎实，发送逻辑在 v0.5 Stub 层一起实现。**

### 7.2 为什么需要 EventHandler 抽象基类

`EventLoop` 不关心某个 fd 是"监听 socket"还是"客户端 socket"——它只知道"epoll 告诉我这个 fd 有事件，我交给它的 handler 处理"。用多态替代 if-else 分支，这是 Reactor 模式的核心。后续加新类型的 handler（如定时器 fd）不需要改 EventLoop。

### 7.3 Acceptor 创建 Connection 后谁管理 Connection 的生命周期？

`EventLoop` 内部用一个 `std::unordered_map<int, std::unique_ptr<EventHandler>>` 管理。Acceptor 创建 Connection 交给 EventLoop，Connection 在 `OnClose` 时从 EventLoop 移除并自动析构。

### 7.4 为什么 EventLoop 用 `epoll_wait(timeout=-1)` 无限等待

单线程 Reactor 在没有事件时阻塞在 `epoll_wait` 不消耗 CPU——这是 IO 多路复用的标准行为。后续如果加入定时任务（如心跳检测），再把 timeout 改成最近一个定时任务的到期时间。

---

## 八、面试重点

这一层是整个 RPC 框架面试价值的核心，能展开聊的话题最多：

1. **为什么 epoll 比 select/poll 好？** — O(1) 事件获取 vs O(n) 遍历，红黑树 + 就绪链表的数据结构设计
2. **ET 和 LT 的区别？ET 为什么必须循环读到 EAGAIN？** — 状态变化通知 vs 状态保持通知，漏读的后果
3. **为什么必须设为非阻塞？** — ET + 循环 read，最后一次 read 如果是阻塞的就卡死整个线程
4. **Reactor 和 Proactor 的区别？** — Reactor 是事件通知（数据到了你自己读），Proactor 是完成通知（系统帮你读好了）
5. **单线程 Reactor 能处理多少并发？** — C10k（一万并发连接），瓶颈不是连接数而是每个请求的处理耗时
