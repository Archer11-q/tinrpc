# 工程日志
记录 TinyRPC 开发过程中的**设计决策、遇到的问题与解决方案**。

---

## v0.1 — 序列化模块（2026-05-30）

### 决策：TLV 与 Protobuf、JSON 选型对比
**背景**：需要为 RPC 的参数与返回值设计传输格式。

**选型对比**：

| 方案 | 优点 | 缺点 |
|------|------|------|
| JSON | 人类可读、易于调试 | 开销大：文本解析、数字与字符串互转、字段名重复传输 |
| Protobuf | 工业级标准、体积小、基于 schema | 外部依赖（protoc + 库文件），隐藏底层编码细节 |
| 自主实现 TLV | 零依赖、完全掌握每一字节逻辑 | 非生产级标准、无 schema 校验 |

**最终决策**：自主实现 TLV。
**原因**：作为学习型项目，**理解原理优先**。TLV 与 Protobuf 核心思想一致（带类型标记的二进制字段），且无需工具链依赖。README 中会补充「与 Protobuf 的关联」章节体现技术认知。

---

### 决策：`std::optional` 与异常的错误处理选型
**背景**：传输中可能出现非法数据：缓冲区截断、类型标记错误、长度不匹配等。

**选项**：
- **异常**：正常/错误流程分离清晰，但 RPC 高频路径性能开销大
- **错误码**：C 风格写法，污染返回值设计
- **`std::optional<T>`**：成功路径零开销、语义清晰（要么有值，要么空）

**最终决策**：使用 `std::optional<T>`。
RPC 高频路径下异常代价过高，`optional` 能在**类型系统层面表达“可能失败”**，且成功路径无运行时损耗。

---

### 决策：位运算实现字节序转换，而非系统 `htonl`
**背景**：需要在**主机字节序（x86 小端）**与**网络字节序（大端）**之间转换。

**选项**：
- **系统 API**（Linux `<arpa/inet.h>` / Windows `Winsock2.h`）：代码简短，但平台相关
- **自主位运算**：`((x & 0xFF) << 24) | ...` — 跨平台、零头文件依赖

**最终决策**：自主位运算实现。
项目同时支持 WSL2 与原生 Linux，位运算仅 5 行代码且全平台通用。

---

### 问题：全局静态测试自动注册 vs 显式 main()
**问题**：最初测试文件使用**全局静态对象**在 `main()` 前自动注册测试，存在未定义行为风险（C 运行时未完全初始化就调用 `printf`）。

**解决方案**：改为在 `main()` 中**显式调用 `RunTest()`**。代码稍繁琐，但 100% 可移植。

---

---

## v0.2 — 协议帧层（2026-05-30）

### 决策：Buffer 与 ProtocolFrame 的职责切分

**背景**：需要设计接收端如何处理 TCP 字节流，同时解包成可用字段。

**选项**：
- **合并为一个大类**：直接把解码和缓冲逻辑写在一起，少写接口
- **分离 Buffer + ProtocolFrame**：Buffer 只管找帧边界（识别魔数+总长度、累积到完整帧），ProtocolFrame 只管字段编解码

**最终决策**：分离 Buffer + ProtocolFrame。
**原因**：Buffer 的粘包/拆包逻辑与帧格式的字段解析逻辑是两个独立关注点。分离后各自独立可测——Buffer 测试不需要构造有效的方法名和 body；ProtocolFrame 测试不需要模拟网络接收。后续如果改用其他帧格式（如 Protobuf 封帧），只需替换 ProtocolFrame，Buffer 保持不变。

### 决策：帧头中魔数用 2 字节而非 4 字节

**背景**：魔数用于快速识别是否为合法帧，需要选择一个不容易随机撞上的值。

**选项**：
- **2 字节**：`0xBABE`，节省带宽，极端情况下随机撞上概率为 1/65536
- **4 字节**：如 `0x0BADF00D`，误识别概率更低但每帧多 2 字节

**最终决策**：2 字节。RPC 帧是 TCP 传输的，TCP 自带校验和，数据损坏概率极低。魔数的主要作用是防御"错误端口连接"而非"传输错误"。2 字节已足够。

### 决策：单帧上限 10MB

**背景**：总长度字段允许服务端声明任意长度的帧。如果攻击者发送声称 frame_len = 2GB 的帧头，服务端可能在分配内存时崩溃。

**最终决策**：硬编码上限 `kMaxFrameSize = 10MB`。这是工程上的合理默认值——RPC 调用的参数和返回值通常远小于此值。超过上限直接拒绝并关闭连接。

### 问题：如何测试粘包/拆包逻辑

**问题**：Buffer 的正确性依赖模拟 TCP 的任意字节切分行为——单测需要验证"一帧分 N 次 Append"的各种组合。

**解决方案**：设计了三类测试用例：
1. **粘包测试**（TestBufferStickyPackets）：连续 Append 两帧完整数据，验证 TryPopFrame 逐帧弹出
2. **拆包测试**（TestBufferSplitPacket）：一帧分多次 Append，前几次 TryPopFrame 返回 nullopt，最后一次弹出完整帧
3. **组合测试**（TestBufferStickyAndSplitCombo）：第一帧的后半部分 + 第二帧的全部粘在一起到达

这三类覆盖了 TCP 字节流的所有实际场景。

---

## v0.3 — 网络 IO 层（2026-05-31）

### 决策：epoll 边缘触发（ET）vs 水平触发（LT）

**背景**：epoll 提供两种触发模式，需要选择默认行为。

**对比**：

| 模式 | 行为 | 优点 | 缺点 |
|------|------|------|------|
| LT | 缓冲区有数据就持续通知 | 编程简单，漏读会再次通知 | 每次 wait 都返回未处理完的 fd，多余系统调用 |
| ET | 状态变化时通知一次 | 每次事件只通知一次，减少系统调用 | 必须循环读到 EAGAIN，漏读导致数据永久丢失 |

**最终决策**：ET 模式。高性能服务器（Nginx、Redis）都用 ET。代价是编程复杂度——必须循环读到 EAGAIN，但这是 RD 该承受的成本。面试能讲清楚 ET 漏读的后果和 EAGAIN 的意义本身就是加分项。

### 决策：EventHandler 多态 vs 函数指针回调

**背景**：EventLoop 收到 epoll 事件后需要调用对应的处理逻辑。两种方式：
- **函数指针/回调**：`std::function<void(int fd, uint32_t events)>`，注册时绑定
- **多态**：`EventHandler` 抽象基类，每个 fd 类型一个子类

**最终决策**：EventHandler 多态。
**原因**：
1. 每种 fd（监听 socket、客户端 socket、未来定时器 fd）行为不同，天然是"同一接口、不同实现"的多态场景
2. 每个 handler 可以持有自己的状态（Buffer、FrameCallback），比函数指针 + 外部状态管理更内聚
3. 面试能讲 Reactor 模式的多态设计

### 决策：v0.3 只做接收不做发送

**背景**：RPC 的发送路径同样涉及非阻塞 write、发送缓冲区、一次 send 写不完整等问题。

**最终决策**：v0.3 只实现接收路径。
**原因**：接收路径涉及 epoll、ET 循环读、Buffer、ProtocolFrame——这四层串联是面试核心话题。发送路径相对简单（send 循环到 EAGAIN），放在 v0.5 与 Stub 层一起实现，届时接收+发送形成完整的双向数据通道。

### 问题：Connection::OnClose 中 close(fd) 与 Socket RAII 的冲突

**问题**：`Socket` 类在析构时自动 `close(fd)`，但 `Connection` 中的客户端 fd 没有用 `Socket` 对象管理——它是 accept 返回的裸 fd。OnClose 时需要谁负责关闭？

**解决方案**：`Connection` 在 `OnClose` 中直接 `close(fd_)`。Acceptor 创建的客户端 socket 不通过 Socket RAII 管理（避免所有权复杂化）。后续可以考虑让 Connection 持有一个 Socket 对象，用移动语义转移所有权。

### 问题：集成测试中服务端与客户端的时间同步

**问题**：测试中服务端线程需要在客户端 connect 之前就进入 `epoll_wait`，否则客户端先 connect 再 start loop 会导致连接事件丢失（ET 模式下，不在 epoll 中的 fd 的连接请求不会触发事件）。

**解决方案**：服务端先调用 `loop.Run()` 进入事件循环，客户端再 connect。用 `std::atomic<bool> loop_running` 作为同步信号——服务端进入 Run 后置 true，客户端检测到 true 后才 connect。

### 下一版本计划（v0.4）
- 实现线程池：任务队列 + worker 线程 + 异步回调支持
- 将 Connection 的 `FrameCallback` 执行从 IO 线程转移到工作线程

---

## v0.4 — 线程池（2026-06-01）

### 决策：`std::mutex` + `std::condition_variable` vs 无锁队列

**背景**：任务队列需要支持多生产者（多个 IO 线程）和多消费者（多个 worker 线程）并发访问。

**最终决策**：标准库方案。
**原因**：
1. 引入第三方无锁队列违背"零外部依赖"原则
2. 自实现无锁队列正确性极难保证（ABA 问题、内存序）
3. 标准库方案性能对于当前 QPS 目标足够，性能差异留到 v0.6 benchmark
4. `condition_variable` 是面试高频考点（wait 原理、虚假唤醒、notify_one vs notify_all），自己实现一遍理解更深

### 决策：Shutdown 等待所有任务完成（drain），不丢弃

**背景**：关闭线程池时队列中可能还有积压任务。

**最终决策**：等待 drain。RPC 框架中每个任务对应一个客户端请求，丢弃任务 = 请求无响应，不可接受。

### 问题：WorkerLoop 中条件变量判断顺序

**问题**：`cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); })` 和退出判断 `if (stop_ && tasks_.empty())` 的顺序必须正确。如果 wait 条件是 `!tasks_.empty() || stop_`（顺序不同），在 Shutdown 时可能出现：stop_ 为 true → wait 返回 → 但 tasks_ 不为空（还有未处理的任务）。

**解决方案**：wait 条件用 `!tasks_.empty() || stop_`，退出判断用 `if (stop_ && tasks_.empty())`。这样保证先处理完所有任务，再响应关停信号。

### 下一版本计划（v0.5）
- 实现 Stub / Dispatch：远程调用透明化
- 实现发送路径：Connection 的 Send/OnWrite

---

## v0.5 — Stub / Dispatch（2026-06-03）

### 决策：FrameCallback 签名升级为 `void(const Frame&, Connection*)`

**背景**：v0.3/v0.4 的 FrameCallback 是 `void(const Frame&)`，回调只能读取请求，无法发送响应。v0.5 需要完成 RPC 闭环，回调必须能写回响应。

**最终决策**：增加 `Connection*` 参数。回调通过 `conn->Send()` 发送响应帧，形成完整的请求-响应闭环。比引入单独的 ResponseWriter 对象更简洁——Connection 本身就封装了发送能力。

### 决策：RpcClient 使用直接 send() 而非 Connection::Send()

**背景**：客户端发送请求帧时，直接调用 `send(fd, ...)` 而不是通过 `Connection::Send()`。

**最终决策**：客户端直接 send()。原因：
1. 客户端是请求-响应模式，一次只发一个请求，不会同时写大量数据导致缓冲区满
2. Connection 在 Register 后所有权转移给 EventLoop，客户端无法持有指针来调用 Send()
3. 如果未来需要客户端的高吞吐发送，可以再接入 Connection::Send() 的缓冲机制

### 决策：Dispatch 的 Handler 返回 `optional<vector<uint8_t>>`

**背景**：处理函数可能失败（参数解析错误、方法不存在等）。

**最终决策**：返回 `optional`。与序列化层 `Read*()` 的错误处理风格一致——在类型层面表达"可能失败"，调用方直接检查。返回 `nullopt` 时 RpcClient 发送 Error 帧。

### 问题：Acceptor 与 CreateListenSocket 端口冲突

**问题**：测试中先用 `CreateListenSocket` 创建监听 socket 获取端口号，再用 `Acceptor` 创建另一个监听 socket 绑定同一端口——第二个 bind 失败。

**解决方案**：`Acceptor` 直接使用端口 0（内核分配），通过 `getsockname(acceptor->GetFd())` 获取实际端口号。不再需要单独的 CreateListenSocket 函数。

### 问题：Connection 析构与 OnClose 的 fd 双重关闭

**问题**：`Connection::OnClose` 中 `close(fd_)`，但如果通过 `Unregister` 销毁 Connection（不走 OnClose），fd 就泄漏了。

**解决方案**：新增 `~Connection()` 析构函数，检查 `fd_ >= 0` 则 close。`OnClose` 中 close 后将 `fd_ = -1`，防止析构函数双重关闭。

### 下一版本计划（v0.6）
- Benchmark：RPC vs HTTP+JSON 性能对比
- 可能优化点：ThreadPool 接入 Connection、连接池、超时机制

---

## v0.6 — Benchmark（2026-06-04）

### 决策：Benchmark 代码放在独立 `bench/` 目录，不混入框架

**背景**：v0.6 需要实现 HTTP+JSON 对照组和压测工具，但这些都是**工具性代码**——只为跑数据、出报告，不属于 RPC 框架本身。

**最终决策**：所有 benchmark 文件放在 `bench/` 目录，不修改 `include/rpc/` 和 `src/`。对比完成后整个目录可保留或删除，零侵入。HTTP 解析和 JSON 序列化均为最小化实现（~50 行），不单独拆头文件。

### 决策：HTTP 对照组复用 EventLoop，仅替换协议层

**背景**：公平对比需要确保差异来源是**协议**（二进制 TLV vs 文本 JSON + HTTP 头），而非网络 IO 实现。

**最终决策**：HTTP 服务端复用 TinyRPC 的 `EventLoop` + `Socket` 基础设施，仅替换 `Connection` 为自实现的 `HttpConnection`（HTTP 解析 + JSON + 业务调用）。这样两者在 epoll 使用、非阻塞 IO 上完全一致，差异只来自协议。

### 问题：HTTP 连接 hang → 根因是 keep-alive 不匹配 + OnClose 自毁

**最初误判**：以为 HTTP 客户端 `connect()` 后立即 `send()` 导致 EPOLLET 竞态（数据早于 `epoll_ctl(ADD)` 到达，无状态变化事件丢失），于是将 HTTP 连接切换为 LT 模式。

**实际根因**：后续排查发现 EPOLLET 竞态理论不成立（Linux 内核在 `epoll_ctl(ADD)` 时会检查 fd 当前状态，已有数据会立即报告）。真正的 hang 原因有两个：
1. HTTP 服务端处理一个请求后立即 `close()`，客户端使用 keep-alive 期望持久连接 → 第二个请求起 send 全失败
2. `OnClose()` 中先 `Unregister`（销毁 this）再访问 `fd_` 成员 → Use-After-Free

**最终方案**：修复 keep-alive 和 OnClose 两个 Bug 后，HTTP 切回 ET 模式（`EPOLLIN | EPOLLET`）完全正常工作。**统一 ET 后整体 QPS 提升约 40%**，说明之前的 LT/ET 混用压制了性能上限。

### 问题：HTTP 连接模型不匹配（keep-alive vs close）

**问题**：初版 HTTP 服务端处理一个请求后立即关闭连接（HTTP/1.0 风格），而客户端使用 `Connection: keep-alive` 期望持久连接。第一次 warmup 后连接断开，后续所有 send 失败。

**解决方案**：改写 `HttpConnection::OnRead()` 为 keep-alive 模式——处理完请求后从 `read_buf_` 中移除已消耗数据，继续等待下一个 EPOLLIN 事件。同时支持 HTTP pipelining（一个 OnRead 中处理多个完整请求），用 `while (ProcessOneRequest())` 循环处理。

### 最终 Benchmark 结果（ET 统一）

**Layer 1：纯序列化（无网络）**

| 场景 | TLV 解码 | JSON 解码 | 加速比 | 体积节省 |
|------|---------|----------|-------|---------|
| 大整数(6字段) | 442 ns | 1,803 ns | **4.1x** | 29% |
| 多字段混合 | 704 ns | 1,907 ns | **2.7x** | 19% |
| +10KB字符串 | 1,270 ns | 2,325 ns | **1.8x** | 0.3% |

**Layer 2+3：端到端（变并发，每线程 5,000 次）**

| 线程 | RPC QPS | HTTP QPS | RPC p99 | HTTP p99 |
|------|---------|----------|---------|----------|
| 1 | 5,021 | 7,989 | 360 μs | 175 μs |
| 4 | 22,319 | 24,488 | 269 μs | 258 μs |
| 8 | 22,608 | 24,385 | 476 μs | 483 μs |

**分析**：
- Layer 1：TLV 二进制协议本身完胜 JSON（解码快 1.8x~4.1x，体积省 20~30%）
- Layer 2+3：两端均 ET + Reactor。1 线程 HTTP 更快（RPC 异步框架开销），4~8 线程 QPS 接近（22k vs 24k），epoll Reactor 抵消了框架开销
- 统一 ET 后整体 QPS 提升 ~40%（对比 LT/ET 混用时），说明变量统一对 benchmark 可信度至关重要
- 这是真实的工程 trade-off，而非伪造的"RPC 无条件更快"数据

### 下一版本计划
- 大消息体 benchmark（1KB / 10KB / 100KB payload）
- 高并发压力测试（64+ 线程，找 QPS 饱和点）
- 可能的优化：ThreadPool 接入、减少内存拷贝

---

## v0.7 — Protobuf 集成（2026-07-11）

### 项目转型背景

项目从纯 RPC 框架转向**游戏服务端**方向。游戏业务需要 enum、repeated、嵌套 message 等 TLV 无法原生支持的类型。需要选择游戏业务层的主力序列化方案。

### 决策：游戏业务层采用 Protobuf，TLV 保留作为技术展示

**背景**：TLV 已完成 11 项测试且端到端 RPC 闭环跑通，但游戏消息（房间、帧同步、匹配）需要复杂数据结构。

**选项**：
- **扩展 TLV**：为 TLV 增加 repeated/enum/nested 支持 → 本质是重新发明 Protobuf
- **全面替换为 Protobuf**：删除 TLV 代码 → 丢失"从零造轮子"的展示价值
- **双轨共存**：TLV 保留不动，游戏业务层新增 Protobuf

**最终决策**：双轨共存。
- TLV 的 `serializer.h/cpp` **一行不改**，保留作为理解序列化原理的证明
- 新增 `proto/game.proto` 定义游戏消息，protoc 生成 C++ 代码
- 协议帧层 `Frame.body` 是 `vector<uint8_t>`，不关心内容格式 → Dispatch 的 handler 内部自由选用 TLV 或 Protobuf 解析 body
- 后续新增的游戏业务模块统一使用 Protobuf

### TLV vs Protobuf 对比测试数据

测试环境：WSL2 (Linux), GCC, C++20, RelWithDebInfo。50 万次循环取均值。

**简单类型**（int32 + string(11B) + double + bool）：

| 方案 | 体积 | 编码(ns/次) | 解码(ns/次) |
|------|------|------------|------------|
| TLV | 44 B | 103.0 | 29.6 |
| Protobuf | 26 B | 58.5 | 58.6 |
| **Proto/TLV** | **59.1%** | **56.8%** | **197.9%** |

**RoomInfo**（含 repeated 嵌套消息，5 个 string + 5 个 int）：

| 方案 | 体积 | 编码(ns/次) | 解码(ns/次) |
|------|------|------------|------------|
| TLV | 98 B | 183.4 | 212.3 |
| Protobuf | 50 B | 102.5 | 248.7 |
| **Proto/TLV** | **51.0%** | **55.9%** | **117.1%** |

**分析**：
- Protobuf 体积约为 TLV 的 **51%~59%**，节省近一半。TLV 每字段需 1B Type + 4B Length = 5B 开销，Protobuf 用 varint + field number 编码只需 1~2B
- Protobuf 编码速度快约 **43~44%**，受益于更紧凑的内存布局和更少的写入操作
- TLV 解码比 Protobuf 快（简单类型快 2x，RoomInfo 相差不大），因为 TLV 解码是纯逐字节读取，无 varint 解码和嵌套消息的递归解析开销
- 解码的绝对值很小（~200ns），在 RPC 网络延迟（μs~ms 级）面前可忽略

**结论**：Protobuf 在体积和编码速度上全面优于 TLV，且原生支持 enum/repeated/nested 等游戏必需的复杂类型。**游戏业务层正式采用 Protobuf proto3。**

### 决策：`proto/` 和 `include/game/` 独立于 RPC 框架

**背景**：Protobuf 协议和游戏模块需要放置位置。

**最终决策**：
- `proto/game.proto` — 协议定义（.proto 非 C++ 源码，独立目录）
- `include/game/` — 游戏模块头文件（与 `include/rpc/` 平级）
- `src/game/` — 游戏模块实现（与 `src/` 下 RPC 实现平级）
- RPC 框架现有文件**位置不动、代码不改**

### 下一版本计划（v0.8）
- 游戏房间服务器：GameRoom 状态机 + RoomManager + Broadcast + TimerManager
- 房间 RPC Service 注册（handler 内部使用 Protobuf 解析 body）

---

## v0.8 — 游戏房间服务器（2026-07-12）

### 决策：TimerManager 用小顶堆而非时间轮

**背景**：游戏服务器需要定时器管理房间超时（空闲 5 分钟、等待 10 分钟、结算 30 秒）。需要在两种经典定时器方案中选择。

**方案对比**：

| 维度 | 小顶堆 | 时间轮 |
|------|--------|--------|
| 添加定时器 | O(log n) | O(1) |
| 触发（tick） | O(log n) | O(1) |
| 取消 | O(1) 惰性 | O(1) 惰性 |
| 实现复杂度 | **简单**，`std::push_heap` | 中等，需处理轮转 |
| 内存 | O(活跃 timer 数) | O(槽数)，固定 |
| 适用场景 | timer < 1000 | timer > 10000 |

**本项目的实际参数**：
- 同时存在的 timer：房间数 × 1~2 = 最多几百个
- timer 时长：分钟级
- 操作频率：低频（创建房间 / 游戏结束）

**最终决策**：小顶堆。

**原因**：
1. timer 数量最多几百个，`O(log 100) ≈ 7 次比较`，与时间轮的 O(1) 无实际差异
2. 用 `std::vector` + `std::push_heap` / `std::pop_heap` 只需 ~60 行实现
3. 时间轮的 O(1) 优势在 10 万级 timer 才显著，本项目差 3 个数量级
4. 惰性删除（Cancel 只标记 `cancelled = true`，Tick 时跳过）避免 O(n) 的查找删除

### TimerManager 接口设计

```cpp
class TimerManager {
    uint64_t Schedule(int64_t delay_ms, Callback callback);  // 注册 → 返回 timer_id
    void     Cancel(uint64_t timer_id);                       // 惰性删除
    size_t   Tick();                                          // 驱动：触发所有到期回调
};
```

设计要点：
- **单线程模型**：TimerManager 不加锁。与 EventLoop 单线程模型一致，EventLoop 通过 `epoll_wait` 的 timeout 参数周期性调用 `Tick()`
- **惰性删除**：Cancel 不立即从堆中移除（需要 O(n) 遍历），只标记 `cancelled = true`。Tick 时在堆顶遇到已取消的 timer 再弹出跳过
- **Vector 而非 priority_queue**：`std::priority_queue` 不暴露底层容器，无法惰性删除。用 `std::vector` + `std::push_heap/pop_heap` 直接操作底层数组

### 决策：TimerManager 归属 `game/` 而非 `rpc/`

**背景**：定时器不依赖任何 RPC 框架组件（EventLoop、Connection、ProtocolFrame），它是纯数据结构 + 时间比较。

**最终决策**：放在 `include/game/timer_manager.h` + `src/game/timer_manager.cpp`，作为游戏业务的基础设施模块。与 `rpc/` 完全隔离，后续 EventLoop 集成时只需在 `Tick()` 调用点做桥接。

### 下一版本计划
- GameRoom 状态机实现
- RoomManager 房间管理器
- Broadcast 广播系统

---

### 决策：GameRoom / RoomManager 线程安全模型（单线程，无锁）

**背景**：GameRoom 的 `players_`、`state_` 等字段在并发场景下存在竞态条件。需要确认是否需要 `std::mutex` 保护。

**调用链分析**：

```
客户端 send()                        定时器到期
    │                                    │
    ▼                                    ▼
┌──────────────────────────────────────────────────────┐
│      epoll_wait → EventLoop::Run() [唯一 IO 线程]      │
│                                                        │
│  Connection::OnRead                                    │
│    → Buffer → ProtocolFrame::Decode                    │
│    → FrameCallback → Dispatch::Call("JoinRoom", body) │
│        → handler(body)                                 │
│            → RoomManager::JoinRoom(rid, pid)          │
│                → GameRoom::AddPlayer(pid)              │
│                    → players_.push_back()              │
│                    → state_ 校验                        │
│                                                        │
│  CheckRoomTimeout()（同线程）                           │
│    → room->timer().Tick()                               │
│        → 超时回调: room->SetState(DESTROYED)           │
└──────────────────────────────────────────────────────┘
```

**结论**：所有 `players_` 和 `state_` 的读写路径最终都收敛到 `epoll_wait` 返回后的同一个 IO 线程内。不存在"线程 A 写，线程 B 读"的并发场景。当前阶段**不需要锁**。

**选项对比**：

| 方案 | 优点 | 缺点 |
|------|------|------|
| 无锁（当前） | 零开销、无死锁风险、代码简单 | 依赖调用方保证在 IO 线程使用 |
| `std::mutex` 每房间 | 未来接入 ThreadPool 后可直接跨线程调用 | 单线程下纯浪费、引入死锁风险、虚假安全感 |
| 状态变更投回 IO 线程 | Reactor 标准做法、保持无锁 | 需要 eventfd 通知机制、增加复杂度 |

**最终决策**：**当前阶段无锁**。

头文件在类注释中标注 `线程模型：所有方法必须在 EventLoop IO 线程调用，由调用方保证`。

**未来方案**：当 ThreadPool worker 线程需要修改房间状态时（如帧同步结算、匹配完成），**不采用加锁**，而是将状态变更封装为 task 通过 eventfd 投回 IO 线程执行。这是 Reactor 模式的标准做法（参考 Netty、Redis），比加锁方案更简单、性能更好。

---

### 决策：JoinRoom/LeaveRoom 返回值从 `bool` 升级为 `Result` + `ErrorCode`

**背景**：当前 `JoinRoom` / `LeaveRoom` 只返回 `bool`，调用方只知道失败，不知道失败原因。客户端收到 `success=false` 后需要知道"房间已满"还是"玩家已在其他房间"才能给用户正确的提示。

**最终决策**：新增 `ErrorCode` proto 枚举和 `GameRoom::Result` 结构体。所有房间操作方法（CreateRoom、JoinRoom、LeaveRoom、StartGame 及其 *AndNotify 版本）统一返回 `Result`。

**ErrorCode 设计**：

```protobuf
enum ErrorCode {
    ERR_NONE                   = 0;  // 成功
    ERR_ROOM_NOT_FOUND         = 1;  // 房间不存在
    ERR_ROOM_FULL              = 2;  // 房间已满
    ERR_ROOM_NOT_JOINABLE      = 3;  // 状态不允许加入（PLAYING/FINISHED/DESTROYED）
    ERR_PLAYER_ALREADY_IN_ROOM = 4;  // 玩家已在房间中
    ERR_PLAYER_NOT_IN_ROOM     = 5;  // 玩家不在房间中
    ERR_NOT_OWNER              = 6;  // 不是房主，无权执行此操作
    ERR_WRONG_ROOM_STATE       = 7;  // 房间状态不允许此操作
}
```

**向后兼容**：`Result` 提供 `operator bool()`，现有测试中 `assert(mgr.JoinRoom(...))` 的写法无需修改。

**新增 `player_room_` 映射**：RoomManager 维护 `unordered_map<player_id, room_id>`，JoinRoom 时检查玩家是否已在其他房间，LeaveRoom/RemoveRoom 时清除映射。