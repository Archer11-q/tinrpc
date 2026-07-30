# RpcClient 多线程并发 send() — Bad File Descriptor

**发现时间**：2026-07-25  
**发现场景**：压测 v0.11，客户端长时间运行时偶发 `send() failed, errno=9 (EBADF)`，TCP 流出现交织错乱  
**严重程度**：高（数据损坏 + 连接断开）

## 问题描述

压测客户端的 `StatsReporterLoop`（独立线程，每 5s 查询一次服务端 metrics）与消息循环线程**同时调用同一个 `RpcClient` 对象的 `Call()` 方法**。两个线程并发调用 `send(fd, ...)` 到同一个 socket fd，在 TCP 层面没有任何同步保护：

1. **EBADF（Bad File Descriptor）**：一个线程正在 send，另一个线程的调用导致某种异常状态
2. **TCP 流交织**：两个 `send()` 的字节在协议栈中交错，服务端收到无法解析的垃圾数据

## 错误架构

```
消息循环线程 ──→ RpcClient::Call("SendMessage", ...) ──→ send(fd, msg_data)
                                                            ↑
StatsReporterLoop ──→ RpcClient::Call("GetMetrics", ...) ──→ send(fd, metrics_data)
                                                            ↑
                                        fd 是同一个 socket，无锁保护
```

## 正确方案

两处修改：

### 方案 1：RpcClient 内部加 send_mutex_

```cpp
// rpc_client.h
class RpcClient {
    std::mutex send_mutex_;
};

// rpc_client.cpp
std::future<Response> RpcClient::Call(const std::string& method, const std::vector<uint8_t>& body) {
    std::lock_guard<std::mutex> lock(send_mutex_);  // 保证单次 send 的原子性
    uint32_t id = SendRequest(method, body);
    // ...
}
```

但仅靠 mutex 不能解决 pending 表的并发问题——`Call()` 和 `OnRead()`（匹配 request_id）仍需协调。

### 方案 2：Metrics 查询用独立连接（最终采用）

```cpp
// bench_game_client.cpp
GameClient metrics_client;  // 独立 RpcClient + 独立 fd
metrics_client.Connect(host, port);
// ...
void StatsReporterLoop() {
    metrics_client.Call("GetMetrics", ...);  // 独立连接，零竞争
}
```

**方案 2 更优**：每个线程拥有自己的 RpcClient 和 socket fd，从根本上消除共享状态。

## 经验教训

1. **socket fd 不是线程安全的**：POSIX 保证 `send()` 本身是线程安全的（不会 crash），但**不保证数据不交织**。两个线程并发 send 同一个 fd，内核可能把 A 线程的一半数据 + B 线程的一半数据拼在一起发送
2. **RpcClient 设计初衷就是单线程使用**：pending 表、request_id 计数器都无锁。多线程共享需要对整个请求-响应链路加锁
3. **独立连接 > 加锁**：加锁解决并发问题但引入争用。每种用途一个独立连接（metrics 连接 / 业务连接），零锁、零竞争
