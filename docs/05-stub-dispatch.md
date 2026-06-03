# 05 — RPC Stub 代理与 Dispatch 分发：远程调用透明化

---

## 一、这一层解决什么问题？

### 当前状态：手动解包 + 手动回调

v0.4 的 `FrameCallback` 能做的最多是这样：

```cpp
// 用户写的回调 — 手动解析 body，手动判断方法名
void HandleFrame(const rpc::Frame& frame) {
    if (frame.method_name == "Add") {
        rpc::Serializer reader(frame.body);
        auto a = reader.ReadInt32();
        auto b = reader.ReadInt32();
        if (!a || !b) return;
        int result = Add(*a, *b);
        // ... 手动序列化结果，手动发送响应 ...
    } else if (frame.method_name == "Sub") {
        // ...
    }
}
```

**问题**：
1. **客户端没有封装**：调用方需要手动序列化参数、手动构造 Frame、手动 send、手动等待响应。调用 `Add(3, 5)` 不像调用本地函数。
2. **服务端用 if-else 链做分发**：每加一个方法就要加一个 `else if`，方法多了没法维护。
3. **缺少发送路径**：v0.3/v0.4 只实现了接收，`Connection` 没有 `OnWrite`。服务端没法发响应，客户端也没法发请求。

### Stub/Dispatch 层的职责

**让远程调用看起来像本地调用。**

```
客户端                                        服务端
┌──────────────────┐                      ┌──────────────────┐
│ stub->Add(3, 5)  │  ──── 网络 ────→    │ Dispatch::Call() │
│      ↓           │                      │   ↓              │
│ 返回 future<int> │  ←──── 网络 ────     │ 调用 Add(3,5)    │
│ future.get() → 8 │                      │ 返回 8           │
└──────────────────┘                      └──────────────────┘
```

客户端写 `stub->Add(3, 5)` 就像调用一个本地对象的方法。服务端"不知道"调用方在另一台机器上——它收到的是一个 Frame，Dispatch 查到方法名对应的函数，执行后返回结果。

---

## 二、Stub（客户端代理）：把远程调用伪装成本地调用

### 2.1 代理模式

Stub 是一个本地 C++ 对象，对外暴露和真实服务一模一样的接口：

```cpp
// 服务接口定义（客户端和服务端共享）
class CalcService {
public:
    virtual int Add(int a, int b) = 0;
    virtual int Sub(int a, int b) = 0;
};

// 客户端 Stub：实现同样的接口，但内部走网络
class CalcStub : public CalcService {
    RpcClient* client_;  // 持有 RPC 客户端引用

    int Add(int a, int b) override {
        // 1. 序列化参数
        Serializer ser;
        ser.WriteInt32(a);
        ser.WriteInt32(b);

        // 2. 发送请求，拿到 future
        auto future = client_->Call("Add", ser.GetBuffer());

        // 3. 等待响应，反序列化结果
        auto response = future.get();
        Serializer reader(response);
        return reader.ReadInt32().value();
    }
};
```

客户端代码：

```cpp
CalcStub stub(client);
int result = stub.Add(3, 5);  // 看着像本地调用，实际走了网络
```

**调用的真实链路**：

```
stub.Add(3, 5)
  → Serializer.WriteInt32(3) + WriteInt32(5)  → body = 18 字节 TLV
  → client_->Call("Add", body)
      → ProtocolFrame::Encode(request_id=1, Request, "Add", body)
      → send(fd, frame, ...)                  → 51 字节发往服务端
      → 返回 std::future<vector<uint8_t>>
  → future.get()                               → 阻塞等待响应
  → Serializer(response).ReadInt32()           → 反序列化得到 8
  → return 8
```

### 2.2 异步调用：`std::future` + `std::promise`

RPC 调用本质是异步的——你发了一个请求，不知道什么时候能收到响应。`std::future` / `std::promise` 是 C++ 标准库提供的"将来会有结果"机制：

```
RpcClient::Call() 内部:
  promise = std::promise<vector<uint8_t>>()  // 创建承诺
  future = promise.get_future()              // 获取 future
  将 {request_id, promise} 存入 pending_requests_ 表
  发送请求帧（含 request_id）
  return future;                             // 立即返回，不阻塞

... 一段时间后，响应到达 ...

响应处理:
  从 Frame 中读出 request_id
  查 pending_requests_ 表找到对应的 promise
  promise.set_value(response_body)            // 兑现承诺
  → future.get() 立即返回
```

**关键**：`future.get()` 在响应到达前会阻塞当前线程，响应到达后立即返回。这就是"同步等待异步结果"。

### 2.3 请求 ID 与响应匹配

同一客户端可能同时发出多个请求：

```
时间线：
  t=0: stub.Add(1, 2)   → 发送 request_id=1
  t=1: stub.Sub(5, 3)   → 发送 request_id=2
  t=2: 收到响应 request_id=2  → 可能是 Sub 的结果先到
  t=3: 收到响应 request_id=1  → Add 的结果后到
```

**request_id 保证响应能找到对应的调用者**。每个请求带唯一 ID，响应帧带上同一个 ID，客户端根据 ID 找到对应的 `promise`。

---

## 三、Dispatch（服务端分发）：从方法名到函数调用

### 3.1 方法注册表

服务端需要一个数据结构把"方法名字符串"映射到"C++ 函数"：

```cpp
// 服务端处理器类型：接收 body 字节，返回 response 字节
using ServiceHandler = std::function<std::vector<uint8_t>(const std::vector<uint8_t>& body)>;

class Dispatch {
public:
    // 注册方法
    void RegisterMethod(const std::string& name, ServiceHandler handler) {
        handlers_[name] = std::move(handler);
    }

    // 根据方法名查找并调用
    std::optional<std::vector<uint8_t>> Call(const std::string& method_name,
                                              const std::vector<uint8_t>& body) {
        auto it = handlers_.find(method_name);
        if (it == handlers_.end()) {
            return std::nullopt;  // 方法未注册
        }
        return it->second(body);
    }

private:
    std::unordered_map<std::string, ServiceHandler> handlers_;
};
```

服务端注册示例：

```cpp
// 服务端：实现真正的 Add 函数
int Add(int a, int b) { return a + b; }

// 注册到 Dispatch
Dispatch dispatch;
dispatch.RegisterMethod("Add", [](const std::vector<uint8_t>& body) {
    Serializer reader(body);
    auto a = reader.ReadInt32();
    auto b = reader.ReadInt32();
    if (!a || !b) throw std::runtime_error("bad params");

    int result = Add(*a, *b);

    Serializer writer;
    writer.WriteInt32(result);
    return writer.GetBuffer();  // 序列化后的返回值
});
```

### 3.2 完整的服务端接收 → 分发 → 响应链路

```
socket recv → Connection::OnRead → Buffer → ProtocolFrame::Decode → Frame
  → ThreadPool::Enqueue([Frame]() {
        // Worker 线程中执行：
        auto response_body = dispatch.Call(frame.method_name, frame.body);
        if (!response_body) {
            // 方法未注册 → 返回 Error 帧
        }
        // 构造响应帧
        auto rsp_frame = ProtocolFrame::Encode(
            frame.request_id, Response, frame.method_name, *response_body);
        // 通过 Connection 发送回去
        conn->Send(rsp_frame);
    })
```

**这就是 RPC 调用的完整闭环**：

```
客户端: stub.Add(3,5) → 序列化 → 帧封装 → TCP send
                                              ↓
服务端: TCP recv → 帧解码 → Dispatch.Call("Add", body) → Add(3,5) → 返回 8
                                              ↓
客户端: TCP recv ← 帧封装 ← 序列化结果(8)
    → 匹配 request_id → promise.set_value → future.get() 返回 8
```

---

## 四、发送路径：Connection 的 OnWrite

### 4.1 为什么发送也需要事件驱动

v0.3/v0.4 的 `Connection` 只处理 `OnRead`，没有 `OnWrite`。直接 `send()` 在简单场景下可以工作，但有隐患：

**问题：非阻塞 socket 的 send 可能写不完。**

```cpp
// 简单写法：直接 send
ssize_t n = send(fd, data, len, 0);
// 可能只发了 len 的一部分！
// 剩下的数据呢？没人管了。
```

**正确做法**：和读一样——每个 Connection 维护一个发送缓冲区，写不完的留在缓冲区里，等 epoll 通知 `EPOLLOUT` 时继续写。

```
发送流程：
  1. 上层调用 conn->Send(data)
     → 如果发送缓冲区为空：直接 write()
        → 全部写完 → 完成
        → 部分写完 → 剩余数据放入缓冲区，注册 EPOLLOUT
     → 如果发送缓冲区非空：追加到缓冲区尾部

  2. epoll 通知 EPOLLOUT
     → OnWrite()：从发送缓冲区取数据，write()
        → 全部写完 → 注销 EPOLLOUT
        → 部分写完 → 继续等待下一次 EPOLLOUT
```

### 4.2 OnWrite 伪代码

```cpp
class Connection {
    std::vector<uint8_t> write_buffer_;  // 发送缓冲区
    size_t write_offset_ = 0;            // 已发送的偏移量

    void Send(const std::vector<uint8_t>& data) {
        if (write_buffer_.empty()) {
            // 尝试直接发送
            ssize_t n = send(fd_, data.data(), data.size(), MSG_NOSIGNAL);
            if (n == static_cast<ssize_t>(data.size())) return;  // 全部发送完毕
            if (n > 0) {
                write_offset_ = n;  // 部分发送，记录偏移
            }
        }
        // 剩余数据追加到发送缓冲区
        write_buffer_.insert(write_buffer_.end(),
                             data.begin() + write_offset_, data.end());
        // 注册 EPOLLOUT
        loop_->UpdateEvents(fd_, EPOLLIN | EPOLLOUT | EPOLLET);
    }

    void OnWrite() override {
        while (!write_buffer_.empty()) {
            ssize_t n = send(fd_, write_buffer_.data() + write_offset_,
                             write_buffer_.size() - write_offset_, MSG_NOSIGNAL);
            if (n > 0) {
                write_offset_ += n;
                if (write_offset_ >= write_buffer_.size()) {
                    write_buffer_.clear();
                    write_offset_ = 0;
                    // 发送完成，取消 EPOLLOUT
                    loop_->UpdateEvents(fd_, EPOLLIN | EPOLLET);
                    return;
                }
            } else if (n == -1 && errno == EAGAIN) {
                return;  // 内核缓冲区满，等下次 EPOLLOUT
            } else {
                OnClose();  // 错误
                return;
            }
        }
    }
};
```

---

## 五、在本项目中的设计决策

### 5.1 Stub 和 Service 接口：纯虚基类 vs 模板

**方案 A — 纯虚基类**：

```cpp
class CalcService {
public:
    virtual int Add(int a, int b) = 0;
    virtual int Sub(int a, int b) = 0;
};
class CalcStub : public CalcService { ... };
class CalcImpl : public CalcService { ... };
```

优点：接口清晰，客户端和服务端共享同一份接口定义，编译器强制检查方法签名一致性。

**方案 B — 模板/C++20 元编程**：更灵活但复杂，不适合当前学习阶段。

**决策**：方案 A。纯虚基类方式直观、可理解，是 gRPC 也在用的模式（`.proto` → 生成 service 基类 → 用户实现 → 框架生成 Stub）。

### 5.2 v0.5 实现范围

**客户端**：

| 类 | 职责 |
|----|------|
| `RpcClient` | 管理连接、发送请求、pending request 表、响应匹配 |

**服务端**：

| 类 | 职责 |
|----|------|
| `Dispatch` | 方法注册表：method_name → handler 映射，Call() 查找并调用 |

**发送路径**：

| 改动 | 说明 |
|------|------|
| `Connection::Send()` | 新增发送接口，非阻塞写 + 发送缓冲区 |
| `Connection::OnWrite()` | 实现 EPOLLOUT 处理 |
| `EventLoop::UpdateEvents()` | 新增，支持动态修改 epoll 事件掩码 |

### 5.3 响应帧的 msg_type

目前协议定义了三种消息类型：

```cpp
enum class MessageType : uint8_t {
    Request  = 0x01,  // 客户端 → 服务端：请求
    Response = 0x02,  // 服务端 → 客户端：正常响应
    Error    = 0x03,  // 服务端 → 客户端：错误响应（方法不存在等）
};
```

v0.5 中，服务端 Dispatch 调用成功返回 `Response` 帧，失败（方法未注册、参数解析错误）返回 `Error` 帧。

### 5.4 客户端超时

RPC 调用不能无限等待。如果服务端挂了，`future.get()` 会永远阻塞。

v0.5 可以用 `std::future::wait_for()` 实现超时：

```cpp
auto future = stub->Add(3, 5);
if (future.wait_for(3s) == std::future_status::timeout) {
    // 超时处理：返回错误、重试、抛异常
    throw TimeoutException("Add(3,5) timed out");
}
int result = future.get();
```

### 5.5 为什么不在 v0.5 做连接池和多路复用

一个 `RpcClient` 维护一条 TCP 连接。生产环境中的连接池、HTTP/2 风格的单连接多路复用等优化留到 v0.6 benchmark 之后。v0.5 的目标是**打通 RPC 调用的完整闭环**。

---

## 六、新增文件清单（v0.5）

| 文件 | 作用 |
|------|------|
| `include/rpc/rpc_client.h` | RpcClient 类声明 |
| `src/rpc_client.cpp` | RpcClient 实现 |
| `include/rpc/dispatch.h` | Dispatch 类声明 |
| `src/dispatch.cpp` | Dispatch 实现 |
| `tests/test_rpc.cpp` | RPC 端到端集成测试 |
| `docs/05-stub-dispatch.md` | 本文档 |

**需修改**：
- `include/rpc/connection.h` — 新增 `Send()`、`OnWrite()`、发送缓冲区
- `src/connection.cpp` — 实现发送路径
- `include/rpc/event_loop.h` — 新增 `UpdateEvents()` 方法
- `src/event_loop.cpp` — 实现 `UpdateEvents()`
- `CMakeLists.txt` — 添加新源文件和测试 target

---

## 七、v0.5 完成后框架的全貌

```
┌─────────────────────────────────────────────────────────────┐
│                        客户端                                │
│                                                             │
│  stub->Add(3, 5)  ← 用户代码（看起来像本地调用）              │
│       │                                                     │
│       ▼                                                     │
│  CalcStub::Add()   → Serializer → ProtocolFrame::Encode     │
│       │                                                     │
│       ▼                                                     │
│  RpcClient::Call() → 分配 request_id → 存入 pending 表       │
│       │                  │                                  │
│       │                  ▼                                  │
│       │            send(fd, frame)                          │
│       │                  │                                  │
│       │                  ▼                                  │
│       │            return future<int>                       │
│       │                                                     │
│       ▼                                                     │
│  future.get() 等待 ──── 响应到达 ──→ promise.set_value       │
│       │                                                     │
│       ▼                                                     │
│  return 8                                                    │
└──────────────────────────┬──────────────────────────────────┘
                           │ TCP
┌──────────────────────────┴──────────────────────────────────┐
│                        服务端                                │
│                                                             │
│  epoll_wait → Connection::OnRead → Buffer → Decode → Frame  │
│       │                                                     │
│       ▼                                                     │
│  ThreadPool::Enqueue([Frame]() {                            │
│      body = dispatch.Call(frame.method_name, frame.body)    │
│      rsp = ProtocolFrame::Encode(id, Response, ..., body)   │
│      conn->Send(rsp)                                        │
│  })                                                         │
│       │                                                     │
│       ▼                                                     │
│  Connection::Send → write/send → ...                        │
│       │                                                     │
│       ▼                                                     │
│  EPOLLOUT → OnWrite → 继续发送 → 发送完成                    │
└─────────────────────────────────────────────────────────────┘
```

**这是 v0.1 ~ v0.5 五层协作的完整链路**。v0.6 在此基础上做 benchmark，测量 RPC 框架的实际性能。

---

## 八、面试重点

1. **Stub（代理模式）是什么？** — 本地对象代表远程服务。调用方不知道自己调用的是一个跨网络的函数。这是 RPC 框架透明性的核心。

2. **`std::future` / `std::promise` 的配合机制？** — Promise 是"承诺将来给结果"，future 是"将来拿结果的凭证"。客户端发送请求后立即返回 future，响应到达后 promise.set_value() 兑现，future.get() 返回。这对 C++ 并发面试是必考题。

3. **request_id 的分配与匹配？** — 每次调用生成唯一 request_id，写入帧头。服务端原样返回。客户端用 request_id 查到对应的 promise。线程安全的 ID 分配器（`std::atomic<uint32_t>`）。

4. **Dispatch 为什么用方法名而不用数字 ID？** — 字符串方法名是可读的、可调试的。数字 ID 效率更高但需要额外的 IDL 编译器生成映射。本题选择字符串映射，简单直观。

5. **非阻塞 send 和发送缓冲区** — 与 v0.3 的读路径对称：写不完的数据留在缓冲区，等 EPOLLOUT 通知继续写。面试能对比讲清楚读/写两条路径的对称设计。

6. **RPC 框架的完整链路能画出来吗？** — `stub.Add(3,5) → 序列化 → 帧 → send → 网络 → recv → 帧解码 → Dispatch → Add(3,5) → 序列化结果 → 帧 → send → 网络 → recv → 帧解码 → promise.set_value → future.get() → return 8`。这就是面试最核心的"讲清楚一个 RPC 调用发生了什么"。
