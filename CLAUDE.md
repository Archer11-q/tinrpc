# CLAUDE.md — TinyRPC 项目交接文档

## 项目概况

TinyRPC 是一个基于 C++20 的轻量级 RPC 框架，面向后端开发学习与面试展示。用户为大二学生，目标暑期/大三上找到 C++ 后端实习。

- **仓库**：`git@github.com:Archer11-q/tinrpc.git`
- **开发环境**：WSL2（Linux），CLion
- **构建系统**：CMake ≥ 3.16，GCC ≥ 9（C++20）
- **语言约定**：所有文档和注释使用中文

## 当前进度

| 版本 | 模块 | 状态 |
|------|------|------|
| v0.1 | 序列化层（TLV 编码） | ✅ |
| v0.2 | 协议帧层（粘包/拆包） | ✅ |
| v0.3 | 网络 IO 层（epoll + Reactor） | ✅ 已完成并推送 |
| v0.4 | 线程池 | 🚧 下一步 |
| v0.5 | Stub / Dispatch | 🚧 |
| v0.6 | Benchmark | 🚧 |

## 项目文件结构

```
D:\CLion\rpc\
├── include/rpc/
│   ├── common.h          # 类型枚举、字节序转换、协议常量
│   ├── serializer.h      # TLV 序列化器
│   ├── protocol.h        # ProtocolFrame — 帧编解码
│   ├── buffer.h          # Buffer — 接收缓冲+粘包/拆包
│   ├── socket.h          # Socket — RAII socket 封装
│   ├── event_handler.h   # EventHandler — 抽象基类
│   ├── event_loop.h      # EventLoop — epoll 事件循环
│   ├── acceptor.h        # Acceptor — 监听新连接
│   └── connection.h      # Connection — 客户端连接处理
├── src/                  # 对应实现文件
│   ├── serializer.cpp / protocol.cpp / buffer.cpp
│   ├── socket.cpp / event_loop.cpp / acceptor.cpp / connection.cpp
├── tests/
│   ├── test_serializer.cpp   # 11 项
│   ├── test_protocol.cpp     # 16 项
│   └── test_network.cpp      # 7 项
├── docs/
│   ├── 01-serialization-layer.md     # 序列化层理论
│   ├── 02-protocol-frame-layer.md    # 协议帧层理论
│   ├── 03-epoll-network-io.md        # 网络 IO 层理论
│   ├── CHANGELOG.md
│   └── devlog.md
├── main.cpp              # 空壳，尚未使用
├── CMakeLists.txt
├── README.md
└── .gitignore
```

## 当前架构（v0.3 完成时）

```
┌──────────────────────────────────────────┐
│  6. 服务注册与发现      静态配置           │  🚧
├──────────────────────────────────────────┤
│  5. Stub / Dispatch     远程调用透明化     │  🚧
├──────────────────────────────────────────┤
│  4. 协议帧层            二进制帧格式       │  ✅ v0.2
├──────────────────────────────────────────┤
│  3. 序列化层            TLV 编码          │  ✅ v0.1
├──────────────────────────────────────────┤
│  2. 线程池              任务调度           │  🚧  ← 下一步
├──────────────────────────────────────────┤
│  1. 网络 IO 层          epoll + Reactor   │  ✅ v0.3
└──────────────────────────────────────────┘
```

数据流（接收路径）：
```
socket recv → Connection::OnRead → Buffer::Append
  → Buffer::TryPopFrame → ProtocolFrame::Decode → FrameCallback
```

## 当前代码的关键设计决策

1. **序列化**：自实现 TLV，不用 Protobuf。位运算字节序转换，零外部依赖。`std::optional<T>` 错误处理。
2. **协议帧**：13 字节帧头（魔数 0xBABE + 总长度 + 请求ID + 消息类型 + 方法名长度）。Buffer + ProtocolFrame 分离。
3. **网络 IO**：epoll ET 模式 + 非阻塞 IO + Reactor 模式。EventLoop 用 eventfd 做 wakeup 机制（Stop() 唤醒 epoll_wait）。v0.3 只实现接收路径，发送在 v0.5。

## 开发协作模式（必须遵守）

这是用户和 AI 之间建立的协作流程，**下一轮对话必须按此模式继续**：

1. **理论先行**：每一层开始前，先生成该层的理论文档存入 `docs/0X-模块名.md`，涵盖"为什么需要、核心原理、设计决策、面试重点"。用户学完确认后再进入实现。
2. **设计讨论**：实现前 AI 先谈接口设计、新增文件清单、关键决策点。用户确认或修改后再写代码。
3. **逐层递进**：每层独立完成 + 测试通过 + 更新文档 + git tag + push。不跨层、不跳跃。
4. **不提前优化**：用户明确不采用"每版本记录已知不足"的模式。优化留到 v0.6 benchmark 之后。但 devlog 继续记录设计决策。
5. **AI 执笔，用户审阅**：AI 生成代码，用户有权质疑和修改任何设计。代码必须匹配用户已有的代码风格（中文注释）。

## v0.4 线程池 — 简要上下文

v0.3 的 `Connection::OnRead` 解码 Frame 后通过 `FrameCallback` 回调。当前回调在 **IO 线程内执行**。v0.4 的目标：

- 实现一个 `ThreadPool` 类（任务队列 + N 个 worker 线程）
- 将 Frame 回调的执行从 IO 线程**转移到工作线程**，避免慢业务逻辑阻塞 epoll_wait
- 为 v0.5 的异步 RPC 调用打基础

## Git 工作流

- 所有开发在 `main` 分支直推，不使用分支
- 每个版本完成后：`git add . && git commit -m "v0.X: ..." && git tag v0.X && git push origin main --tags`
- `.gitignore` 已配置，排除 build/、.idea/、编译产物

## 测试约定

- 不使用 Google Test，所有测试用 `assert()` + `printf()` 手写
- 每个模块独立测试 target（test_serializer、test_protocol、test_network）
- CMake 中 `rpc_lib` 是静态库，所有 test target 链接它

## README 约定

- 中文撰写
- 分层架构图直接标注版本状态（✅/🚧）
- 开发路线图表格
- 设计文档索引
- 不写"面试可聊"等说明性标注
- v0.6 之前不加入性能对比表格

## docs/ 目录约定

- 理论文档：`docs/0X-模块名.md`
- 更新日志：`docs/CHANGELOG.md`（每个版本新增功能 + 设计思路）
- 工程日志：`docs/devlog.md`（设计决策 + 遇到问题 + 解决方案）
- 全部中文撰写
