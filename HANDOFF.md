# HANDOFF.md — TinyRPC 项目交接摘要

## 项目目标

TinyRPC 是基于 C++20 的轻量级 RPC 框架，零外部依赖。**用户为大二学生，目标暑期/大三上找到 C++ 后端实习**，项目用于学习后端底层原理和面试展示。

- 仓库：`git@github.com:Archer11-q/tinrpc.git`
- 开发环境：WSL2 + CLion + CMake + GCC ≥ 9
- 代码风格：中文注释，`assert()` 手写测试，不引入第三方库
- Git：`main` 分支直推，每个版本 tag + push

---

## 当前进度（截至 2026-06-04）

| 版本 | 模块 | 状态 | 关键产出 |
|------|------|------|----------|
| v0.1 | 序列化层 | ✅ | TLV 编码，位运算字节序转换，`std::optional` 错误处理，11 项测试 |
| v0.2 | 协议帧层 | ✅ | 13 字节帧头（0xBABE），Buffer 粘包/拆包，16 项测试 |
| v0.3 | 网络 IO 层 | ✅ | epoll ET + Reactor，EventLoop/EventHandler/Acceptor/Connection，7 项测试 |
| v0.4 | 线程池 | ✅ | 生产者-消费者，`mutex`+`condition_variable`，6 项测试 |
| v0.5 | Stub/Dispatch | ✅ | RpcClient + Dispatch + Connection Send/OnWrite 发送路径，4 项集成测试 |
| v0.6 | Benchmark | ✅ | 三层 benchmark：纯序列化 + 端到端变并发 + 汇总脚本 |
| **v0.7** | **下一阶段** | 🚧 | 见下方「下一阶段方向」 |

**总计 44 项测试，全部通过。**

---

## 项目文件结构

```
D:\CLion\rpc\
├── include/rpc/
│   ├── common.h              # 类型枚举、字节序、协议常量、MessageType
│   ├── serializer.h          # TLV 序列化器
│   ├── protocol.h            # ProtocolFrame 编解码 + Frame 结构体
│   ├── buffer.h              # 接收缓冲 + TryPopFrame 粘包/拆包
│   ├── socket.h              # RAII socket 封装
│   ├── event_handler.h       # EventHandler 抽象基类
│   ├── event_loop.h          # EventLoop — epoll 事件循环 + UpdateEvents
│   ├── acceptor.h            # Acceptor — 监听新连接，透传 FrameCallback
│   ├── connection.h          # Connection — OnRead/OnWrite/OnClose/Send + 发送缓冲区
│   ├── thread_pool.h         # ThreadPool — 生产者-消费者
│   ├── dispatch.h            # Dispatch — 方法注册表
│   └── rpc_client.h          # RpcClient — 客户端代理 + pending 表 + future
├── src/                      # 所有 .cpp 对应实现
├── bench/                    # 🆕 Benchmark 工具（独立目录，与框架隔离）
│   ├── bench_server.cpp      # 双模式服务端（--mode rpc|http）
│   ├── bench_client.cpp      # 多线程压测客户端 + 延迟分位数统计
│   ├── bench_serialize.cpp   # Layer 1 纯序列化对比（无网络）
│   └── run_all.sh            # 一键运行三层 benchmark + 生成 Markdown 报告
├── tests/
│   ├── test_serializer.cpp    # 11 项
│   ├── test_protocol.cpp      # 16 项
│   ├── test_network.cpp       # 7 项
│   ├── test_thread_pool.cpp   # 6 项
│   └── test_rpc.cpp           # 4 项
├── docs/
│   ├── 01-serialization-layer.md     # 序列化层理论（不上传 GitHub）
│   ├── 02-protocol-frame-layer.md    # 协议帧层理论（不上传）
│   ├── 03-epoll-network-io.md        # 网络 IO 层理论（不上传）
│   ├── 04-thread-pool.md             # 线程池理论（不上传）
│   ├── 05-stub-dispatch.md           # Stub/Dispatch 理论（不上传）
│   ├── 06-benchmark.md               # Benchmark 理论（不上传）
│   ├── CHANGELOG.md                   # 版本更新日志（上传 GitHub）
│   └── devlog.md                      # 工程日志（上传 GitHub）
├── main.cpp                  # 空壳，尚未使用
├── CMakeLists.txt
├── README.md
├── HANDOFF.md                # 本文档
├── CLAUDE.md                 # 项目内部上下文
└── .gitignore
```

---

## 当前架构：v0.5 完整 RPC 闭环

```
客户端                                        服务端

stub->Call("Add", body)
  → Serializer(参数)
  → ProtocolFrame::Encode(id, Request, "Add", body)
  → send()
  → return future<int>                       epoll_wait → Connection::OnRead [IO线程]
                                                  → Buffer → ProtocolFrame::Decode
                                                  → FrameCallback(frame, conn) [IO线程]
                                                        → Dispatch::Call("Add", body)
                                                        → Add(a,b) → result
                                                        → ProtocolFrame::Encode(id, Response, ...)
                                                        → conn->Send(rsp_bytes)
  → future.get()  ← promise.set_value ──── ← OnRead → FrameCallback → 匹配 request_id
  → Serializer(rsp_body).ReadInt32() → 8
```

---

## v0.6 Benchmark 核心数据

### Layer 1：纯序列化（无网络，展示协议层差异）

6 字段结构体（int64×2 + int32×2 + double + bool + string），50 万次迭代：

| 场景 | TLV体积 | JSON体积 | 节省 | TLV解码 | JSON解码 | 加速比 |
|------|--------|---------|------|---------|---------|-------|
| 大整数(6字段) | 63 B | 89 B | **29%** | 442 ns | 1,803 ns | **4.1x** |
| 多字段混合 | 132 B | 163 B | **19%** | 704 ns | 1,907 ns | **2.7x** |
| +10KB字符串 | 10,322 B | 10,358 B | 0.3% | 1,270 ns | 2,325 ns | **1.8x** |

### Layer 2+3：端到端变并发（Add(3,5) 小请求，每线程 5000 次）

| 线程 | RPC QPS | HTTP QPS | RPC p99 | HTTP p99 |
|------|---------|----------|---------|----------|
| 1 | 5,021 | **7,989** | 360 μs | **175 μs** |
| 4 | 22,319 | **24,488** | **269 μs** | 258 μs |
| 8 | 22,608 | **24,385** | 476 μs | **483 μs** |

### 数据解读

- **Layer 1 明确证明 TLV 二进制协议优于 JSON**：解码快 1.8x~4.1x，体积省 20~30%
- **Layer 2+3**：两端均 ET + Reactor（统一变量）。1 线程 HTTP 更快（RPC 异步开销），4~8 线程 QPS 接近（22k vs 24k），epoll 抵消框架开销
- 统一 ET 后整体吞吐提升 ~40%，之前 LT/ET 混用压制了双方性能上限

---

## v0.6 开发中的关键 Bug 和教训

1. **EPOLLET 竞态条件**：HTTP 客户端 connect 后立即 send，数据可能早于 epoll_ctl(ADD) 到达内核缓冲区——边缘触发下无状态变化，epoll_wait 永不返回。HTTP 连接改用水平触发（EPOLLIN）解决。

2. **HTTP 连接模型不匹配**：初版服务端处理一个请求即关闭（HTTP/1.0 风格），客户端用 keep-alive 期望持久连接 → 100% 失败率。重写为 keep-alive + pipelining 解决。

3. **OnClose 自毁顺序**：Unregister 会销毁 unique_ptr（即 this），之后不能再访问任何成员。必须先 close(fd) 再 Unregister。

4. **Benchmark 公平性**：HTTP 对照组复用 TinyRPC 的 EventLoop 网络层，仅替换协议解析部分——确保对比的是协议差异而非网络实现差异。

---

## 关键设计决策（新 Agent 需知晓）

1. **零外部依赖**：不用 Protobuf、Google Test、无锁队列。Benchmark 对照组的 HTTP/JSON 也是自实现（最小化，~50 行）。
2. **Benchmark 独立隔离**：所有对比代码在 `bench/` 下，不修改 `include/rpc/` 和 `src/`。用完可整体删除，零侵入。
3. **FrameCallback 签名**：`void(const Frame&, Connection* conn)` — 第二个参数允许回调发送响应。
4. **ThreadPool 已实现但未接入**：`Connection` 当前没有 `ThreadPool*` 成员，FrameCallback 在 IO 线程执行。benchmark 数据表明这是优化点。
5. **RpcClient 使用直接 send()**：客户端请求通过 `send()` 直接发送。Connection 所有权在 Register 后转移给 EventLoop。
6. **C++ 测试约定**：`assert()` + `printf()` 手写测试，不用 Google Test。每个模块独立 test target。
7. **理论文档不上传 GitHub**：`docs/0*-*.md` 在 `.gitignore`，仅 `CHANGELOG.md` 和 `devlog.md` 上传。

---

## 开发协作模式（核心规则）

1. **理论先行**：每层先写 `docs/0X-模块名.md`，用户学完确认后再写代码。
2. **设计讨论**：实现前先谈接口设计、新增文件清单、关键决策点。用户确认后再写代码。
3. **AI 执笔，用户审阅**：AI 写代码，用户有权质疑和修改。
4. **逐层递进**：每层独立完成 + 测试通过 + 文档更新 + git tag + push。
5. **中文注释和文档**。
6. **不提前优化**：优化以 benchmark 数据为驱动，每次只改一个变量，对比前后数据。

---

## 下一阶段方向（v0.7+）

### 优先级高

1. **main.cpp 填充**：用命令行参数启动 server/client，变成一个可运行的 RPC 程序
   - `./tinrpc --mode server --port 8080` 启动服务端
   - `./tinrpc --mode client --host 127.0.0.1 --port 8080` 启动客户端
   - 把目前的 Add/Sub 注册进去，展示完整流程
2. **ThreadPool 接入 Connection**：benchmark 表明 IO 线程执行 FrameCallback 是瓶颈，接入现有 ThreadPool 做异步回调
3. **大消息 benchmark**：当前 benchmark 仅测了小请求（40B），补充 1KB / 100KB payload 的端到端数据

### 优先级中

4. **减少内存拷贝**：Serializer GetBuffer() 返回拷贝、Frame 传递时拷贝 body、回调入队时拷贝 Frame。尝试 move 语义或 shared_ptr
5. **RpcClient 连接池**：当前一个 RpcClient 一条 TCP 连接，高并发下需要连接池复用
6. **客户端超时与重试**：`future.wait_for()` 超时后清理 pending 表，支持自动重试

### 优先级低

7. **服务注册与发现**（第 6 层）：简单的静态服务注册表
8. **无锁队列**：如果 ThreadPool 接入后 `std::mutex` 成为瓶颈，考虑 SPSC 无锁队列
9. **跨机器 benchmark**：两台 WSL2 实例间的真实网络延迟测试

---

## 运行命令速查

```bash
# 构建
cd build && cmake .. && make -j$(nproc)

# 跑全部测试
./test_serializer && ./test_protocol && ./test_network && ./test_thread_pool && ./test_rpc

# 启动服务端
./bench_server --mode rpc --port 8080     # TinyRPC
./bench_server --mode http --port 8080    # HTTP+JSON

# 运行压测
./bench_client --mode rpc --port 8080 --threads 8 --requests 10000 --warmup 1000

# 纯序列化对比
./bench_serialize

# 一键全量 benchmark
bash ../bench/run_all.sh

# Git 发布
git add . && git commit -m "v0.X: ..." && git tag v0.X && git push origin main --tags
```