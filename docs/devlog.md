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