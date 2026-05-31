# 更新日志
本文件记录 TinyRPC 所有重要版本变更。

## v0.1 — 序列化模块（2026-05-30）

### 新增功能
- 实现 `rpc::Serializer` 类，支持 TLV 二进制编解码
- 支持 6 种基础数据类型：32位整型、64位整型、单精度浮点数、双精度浮点数、字符串、布尔值
- 提供跨平台字节序转换能力（主机字节序 ↔ 网络大端字节序）
- 反序列化接口返回 `std::optional<T>`，安全处理异常场景
- 增加类型不匹配检测：读取类型与数据实际类型不一致时返回空值
- 所有读取操作均做边界校验
- 共计 11 项单元测试，覆盖完整读写、混合类型、类型错误、数据截断、空缓冲区、长度不匹配等场景

### 设计思路
- 自主实现 TLV 协议，而非使用 Protobuf，深入理解编码底层原理
- 采用位运算实现字节序转换，不依赖系统 `htonl` 系列接口，实现零依赖、全平台兼容
- 核心链路使用 `std::optional` 而非异常机制处理错误，保证运行性能
---


## v0.2 — 协议帧层（2026-05-30）

### 新增功能
- 实现 `ProtocolFrame` 类，支持完整的二进制帧编解码
- 定义帧格式：魔数(2) + 总长度(4) + 请求ID(4) + 消息类型(1) + 方法名长度(2) + 方法名(N) + body(M)
- 实现 `Buffer` 类，通过长度前缀法解决 TCP 粘包/拆包问题
- 解码时全字段校验：魔数、总长度上下限、消息类型合法性、方法名长度溢出
- 2 字节字节序转换函数 `HostToNetwork16`/`NetworkToHost16`
- 新增 `MessageType` 枚举：Request、Response、Error
- 共计 16 项单元测试：覆盖编解码往返、各种异常输入、粘包/拆包/半包场景

### 设计思路
- 帧层与序列化层完全解耦：Buffer 只识别帧边界（魔数+总长度），ProtocolFrame 负责字段编解码
- Buffer 采用状态机隐式实现（TryPopFrame 无状态、靠缓冲区累积），无需显式状态标志
- 单帧上限 10MB，防止恶意超大长度声明耗尽内存

---

## v0.3 — 网络 IO 层（2026-05-31）

### 新增功能
- 实现 `Socket` 类：RAII 包装 socket fd，支持 bind/listen/accept、非阻塞设置、移动语义
- 实现 `EventLoop` 类：基于 epoll 的事件循环，支持 Register/Unregister fd + EventHandler 映射
- 实现 `EventHandler` 抽象基类：OnRead/OnWrite/OnClose 虚方法，fd 统一管理
- 实现 `Acceptor` 类：处理新连接，自动创建 Connection 并注册到 EventLoop
- 实现 `Connection` 类：ET 模式循环 recv → Buffer 累积 → TryPopFrame → ProtocolFrame 解码 → 回调
- 接收路径使用 `recv()` 替代 `read()`，允许后续扩展 flags
- 共计 7 项测试：Socket(4)、EventLoop(2)、集成(1) 覆盖完整数据路径

### 设计思路
- Reactor 模式：EventLoop（事件分发）→ EventHandler（多态处理）→ Buffer + ProtocolFrame（已有层）三层协作
- Epoll 边缘触发（ET）+ 非阻塞 IO：循环 recv 到 EAGAIN，防止漏读
- EventHandler 多态替代 if-else 分支：EventLoop 不关心 fd 类型，后续加定时器等新 handler 无需改 EventLoop
- Frame 回调机制：v0.3 用 `FrameCallback` 把解码后的帧传出，后续 v0.5 替换为 Dispatch

---

