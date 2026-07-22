# CLAUDE.md — TinyRPC 项目交接文档

## 项目概况

TinyRPC 是一个基于 C++20 的轻量级 RPC 框架。已完成六层通信内核（v0.6），正在转型为**基于自研 RPC 通信层的游戏服务端项目**。

用户为大二学生，目标转向 **C++ 游戏服务端开发**。

- **仓库**：`git@github.com:Archer11-q/tinrpc.git`
- **开发环境**：WSL2（Linux），CLion
- **构建系统**：CMake ≥ 3.16，GCC ≥ 9（C++20）
- **语言约定**：所有文档和注释使用中文

## 当前进度

| 版本 | 模块 | 状态 |
|------|------|------|
| v0.1 | 序列化层（TLV 编码） | ✅ |
| v0.2 | 协议帧层（粘包/拆包） | ✅ |
| v0.3 | 网络 IO 层（epoll + Reactor） | ✅ |
| v0.4 | 线程池 | ✅ |
| v0.5 | Stub / Dispatch | ✅ |
| v0.6 | Benchmark | ✅ |
| v0.7 | Protobuf 集成 + 游戏协议 | ✅ |
| v0.8 | TimerManager 定时器 | ✅ |
| v0.8 | GameRoom / RoomManager | ✅ |
| v0.8 | Broadcast + 房间事件通知 | ✅ |
| v0.8 | RoomService RPC 注册 + ErrorCode 整合 | ✅ |
| v0.8 | EPOLLRDHUP 断连检测 + 自动清理 | ✅ |
| v0.9 | InputBuffer — Jitter Buffer | ✅ |
| v0.9 | FrameSyncManager — 帧号/输入收集/广播/Timer驱动 | ✅ |
| v0.9 | GameState + tickLogic — 确定性状态更新 | ✅ |
| v0.9 | CatchUp 追帧 — 每次2帧加速策略 | ✅ |
| v0.9 | SnapshotManager — 环形缓冲区快照/回滚占位 | ✅ |
| v0.9 | 预测/和解(Reconciliation) — CompareStates + ReconcileState | ✅ |
| v0.9 | 帧同步与房间衔接 — StartGame自动启动帧同步 + SendInput/StopGame RPC | ✅ |
| v0.9 | 全流程模拟测试 + 耗时报告 | ✅ |
| v0.10 | EloCalculator — ELO 分计算 | ✅ |
| v0.10 | MatchQueue — 匹配队列 + 超时放宽 | ✅ |
| v0.10 | MatchService — 匹配→房间→通知→超时 | ✅ |
| v0.10 | 匹配队列断连清理 — CancelMatch在断连回调中调用 | ✅ |
| v0.10 | GameService — 集中入口 + main() | ✅ |
| v0.10 | SessionManager 接口 + 断线重连方案文档 | ✅ |

## 演化方向

项目从纯 RPC 框架向**游戏服务端**演进。底层 RPC 六层保持不变，之上逐步叠加游戏业务模块：

```
游戏业务层（🚧 进行中）
├── 游戏协议层           ← Protobuf proto3，定义 LoginReq、Room、Frame、Match 等消息
├── TimerManager         ← 跨模块基础设施，小顶堆定时器
├── 游戏房间服务器
│   ├── GameRoom          ← 房间状态机（空闲→等待→游戏中→结算→销毁）✅
│   ├── RoomManager       ← 房间 CRUD + 超时淘汰 ✅
│   ├── Broadcast         ← 房间内广播 ✅
│   └── RoomService       ← RPC Service 注册 + Stub 代理 ✅
│   └── 断连检测            ← EPOLLRDHUP + 自动房间清理 ✅
├── 帧同步系统
│   ├── FrameSyncManager  ← 输入收集 + 帧广播 + 追帧 + Timer驱动 ✅
│   ├── InputBuffer       ← Jitter Buffer（deque, 乱序支持）✅
│   ├── GameState         ← 确定性状态更新（tickLogic）✅
│   ├── SnapshotManager   ← 环形缓冲区快照/回滚（断线重连）✅
│   ├── Reconciliation    ← 预测/和解（CompareStates + ReconcileState）✅
│   └── 房间衔接           ← StartGame自动启动帧同步 + SendInput/StopGame RPC ✅
├── 匹配系统
│   ├── EloCalculator     ← ELO 分计算 ✅
│   ├── MatchQueue        ← 匹配队列 + 超时放宽 ✅
│   └── MatchService      ← 匹配→房间→通知→超时 ✅
├── GameService           ← 集中入口: 组装全部模块 ✅
├── SessionManager        ← 会话管理 + 断线重连（接口定义）✅
└── 压测工具              ← 游戏业务全流程压测

RPC 通信层（已完成）
├── 序列化层              ← TLV（保留，不再扩展）+ Protobuf（游戏业务主力）
├── 协议帧层              ← 13 字节帧头，body 不关心内容格式
├── 网络 IO 层            ← epoll ET + Reactor
├── 线程池                ← 生产者-消费者异步回调
├── Stub / Dispatch       ← 客户端代理 + 服务端方法分发
└── Benchmark             ← RPC 框架层性能基准
```

关键转型原则：
- **RPC 六层零改动**：协议帧的 `body` 字段是 `vector<uint8_t>`，帧层不关心内容是 TLV 还是 Protobuf
- **TLV 保留不删**：作为"从零造轮子"的能力证明，但不再扩展新类型
- **Protobuf 接管游戏业务**：新增文件（`proto/`、`game/`），不修改现有框架代码
- **仓库不换**：在旧仓库上继续开发，git 历史完整记录从 RPC 框架到游戏服务器的演进过程

## 项目文件结构

```
D:\CLion\rpc\
├── include/
│   ├── rpc/                  # RPC 框架头文件（已有）
│   │   ├── common.h          # 类型枚举、字节序转换、协议常量
│   │   ├── serializer.h      # TLV 序列化器
│   │   ├── protocol.h        # ProtocolFrame — 帧编解码
│   │   ├── buffer.h          # Buffer — 接收缓冲+粘包/拆包
│   │   ├── socket.h          # Socket — RAII socket 封装
│   │   ├── event_handler.h   # EventHandler — 抽象基类
│   │   ├── event_loop.h      # EventLoop — epoll 事件循环
│   │   ├── acceptor.h        # Acceptor — 监听新连接
│   │   ├── connection.h      # Connection — 客户端连接处理
│   │   ├── thread_pool.h     # ThreadPool — 生产者-消费者
│   │   ├── dispatch.h        # Dispatch — 方法注册表
│   │   └── rpc_client.h      # RpcClient — 客户端代理 + pending 表
│   └── game/                 # 游戏模块头文件
│       ├── timer_manager.h    ✅ v0.8
│       ├── game_room.h        ✅ v0.8
│       ├── room_manager.h     ✅ v0.8
│       ├── broadcast.h        ✅ v0.8
│       ├── room_service.h     ✅ v0.8
│       ├── input_buffer.h     ✅ v0.9
│       ├── frame_sync.h       ✅ v0.9
│       ├── game_state.h       ✅ v0.9
│       ├── snapshot_manager.h ✅ v0.9
│       ├── match_queue.h
│       ├── match_service.h
│       ├── game_service.h
│       └── session_manager.h
├── src/                      # 实现文件
│   ├── serializer.cpp        # RPC 框架（已有，位置不动）
│   ├── protocol.cpp
│   ├── buffer.cpp
│   ├── socket.cpp
│   ├── event_loop.cpp
│   ├── acceptor.cpp
│   ├── connection.cpp
│   ├── thread_pool.cpp
│   ├── dispatch.cpp
│   ├── rpc_client.cpp
│   └── game/                 # 游戏模块实现
│       ├── timer_manager.cpp  ✅ v0.8
│       ├── game_room.cpp     ✅ v0.8
│       ├── room_manager.cpp  ✅ v0.8
│       ├── broadcast.cpp     ✅ v0.8
│       ├── room_service.cpp  ✅ v0.8
│       ├── input_buffer.cpp  ✅ v0.9
│       ├── frame_sync.cpp    ✅ v0.9
│       ├── game_state.cpp    ✅ v0.9
│       ├── snapshot_manager.cpp ✅ v0.9
│       ├── match_queue.cpp
│       ├── match_service.cpp
│       ├── game_service.cpp
│       └── session_manager.cpp
├── proto/                    # Protobuf 协议定义（.proto，非 C++ 源码）
│   └── game.proto            # Login/Room/Frame/Match 等消息（v0.7~v0.9 持续扩展）
├── stress/                   # [待建] 游戏业务压测工具
│   └── stress_client.cpp
├── bench/                    # Benchmark 工具（RPC 框架层，已完成）
├── tests/
│   ├── test_serializer.cpp    # 11 项
│   ├── test_protocol.cpp      # 16 项
│   ├── test_network.cpp       # 7 项
│   ├── test_thread_pool.cpp   # 6 项
│   ├── test_rpc.cpp           # 4 项
│   ├── test_room_service.cpp  # 14 项
│   ├── test_input_buffer.cpp   # 20 项（InputBuffer 单元）
│   ├── test_frame_sync.cpp     # 27 项（FrameSyncManager + 追帧）
│   ├── test_game_state.cpp     # 21 项（tickLogic 确定性 + 预测/和解）
│   ├── test_snapshot_manager.cpp # 17 项（SnapshotManager）
│   ├── test_frame_sync_flow.cpp  # 全流程模拟 + 耗时报告
│   ├── test_match_queue.cpp      # 33 项（匹配系统单元+集成）
│   （共 165 项测试，全部通过）
├── docs/
│   ├── 01-serialization-layer.md
│   ├── 02-protocol-frame-layer.md
│   ├── 03-epoll-network-io.md
│   ├── 04-thread-pool.md
│   ├── 05-stub-dispatch.md
│   ├── 06-benchmark.md
│   ├── CHANGELOG.md
│   ├── devlog.md
│   └── pitfalls/          # 踩坑记录
├── main.cpp              # 空壳，尚未使用
├── CMakeLists.txt
├── README.md
└── .gitignore
```

## 当前架构（v0.5 完整 RPC 闭环）

```
客户端                                        服务端

stub->Call("Add", body)
  → Serializer(参数)
  → ProtocolFrame::Encode(id, Request, "Add", body)
  → send()
  → return future<int>                       epoll_wait → Connection::OnRead [IO线程]
                                                  → Buffer → ProtocolFrame::Decode
                                                  → FrameCallback(frame, conn)
                                                        → Dispatch::Call("Add", body)
                                                        → Add(a,b) → result
                                                        → ProtocolFrame::Encode(id, Response, ...)
                                                        → conn->Send(rsp_bytes)
  → future.get()  ← promise.set_value ──── ← OnRead → FrameCallback → 匹配 request_id
  → Serializer(rsp_body).ReadInt32() → 8
```

## 关键设计决策

1. **序列化双轨**：TLV 保留作为历史版本和造轮子能力证明；游戏业务层使用 Protobuf proto3。两者在 `Frame.body` 层面共存，协议帧层不受影响。
2. **Protobuf 集成策略**：新增文件（`proto/`、`game/`），零侵入 RPC 六层。Dispatch 的 `Handler` 签名为 `vector<uint8_t> → optional<vector<uint8_t>>`，body 内容由 handler 内部用 Protobuf 解析。
3. **协议帧**：13 字节帧头（魔数 0xBABE + 总长度 + 请求ID + 消息类型 + 方法名长度）。Buffer + ProtocolFrame 分离。帧层不关心里面是 TLV、JSON 还是 Protobuf。
4. **网络 IO**：epoll ET 模式 + 非阻塞 IO + Reactor 模式。EventLoop 用 eventfd 做 wakeup 机制。
5. **FrameCallback 签名**：`void(const Frame&, Connection* conn)` — 第二个参数允许回调发送响应。
6. **RpcClient 使用直接 send()**：客户端请求通过 `send()` 直接发送。Connection 所有权在 Register 后转移给 EventLoop。
7. **TimerManager**：跨模块基础设施，不归属任一业务模块。房间超时、帧同步 tick、匹配超时共用。
8. **Benchmark 独立目录**：RPC 框架层对比代码在 `bench/`，游戏业务压测在 `stress/`，层次清晰。
9. **理论文档不上传**：`docs/0*-*.md` 在 `.gitignore`，仅 `CHANGELOG.md` 和 `devlog.md` 上传 GitHub。

## 开发协作模式（必须遵守）

这是用户和 AI 之间建立的协作流程，**下一轮对话必须按此模式继续**：

1. **理论先行**：每一层开始前，先生成该层的理论文档存入 `docs/`，涵盖"为什么需要、核心原理、设计决策、面试重点"。用户学完确认后再进入实现。
2. **设计讨论**：实现前 AI 先谈接口设计、新增文件清单、关键决策点。用户确认或修改后再写代码。
3. **逐层递进**：每层独立完成 + 测试通过 + 更新文档 + git tag + push。不跨层、不跳跃。
4. **不提前优化**：功能优先，优化留到后期统一做。
5. **AI 执笔，用户审阅**：AI 生成代码，用户有权质疑和修改任何设计。代码必须匹配用户已有的代码风格（中文注释）。

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
- 分层架构图直接标注版本状态（✅/🚧/🔲）
- 开发路线图表格
- 设计文档索引

## docs/ 目录约定

- 理论文档：`docs/0X-模块名.md`
- 更新日志：`docs/CHANGELOG.md`（每个版本新增功能 + 设计思路）
- 工程日志：`docs/devlog.md`（设计决策 + 遇到问题 + 解决方案）
- 全部中文撰写
