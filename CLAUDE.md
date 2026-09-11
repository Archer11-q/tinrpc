# CLAUDE.md — TinyRPC 项目交接文档

## 项目概况

TinyRPC 是一个基于 C++20 的轻量级 RPC 框架。已完成六层通信内核（v0.6），正在转型为**基于自研 RPC 通信层的游戏服务端项目**。

用户为大二学生，目标转向 **C++ 游戏服务端开发**。

- **仓库**：`git@github.com:Archer11-q/tinrpc.git`
- **开发环境**：WSL2（Linux），CLion
- **构建系统**：CMake ≥ 3.16，GCC ≥ 9（C++20）
- **语言约定**：所有文档和注释使用中文

### 实测环境（2026-09 核对）

| 工具 | 版本 | 说明 |
|------|------|------|
| GCC | 15.2.0 | 最低要求 GCC 9 |
| CMake | 4.2.3 | 最低要求 3.16 |
| protoc | 3.21.12 | Protobuf |
| 核心数 | 16 | `make -j16` |
| 构建目录 | `build/`（WSL2 侧） | 推荐 `cmake .. && make -j16` |

> **注意**：`build/` 中的二进制可能早于当前源码。跑测试前务必先 `make`，
> 否则会被陈旧产物误导（v0.12 排查 `test_frame_sync` 时就踩过：旧二进制通过、新源码实际失败）。

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
| v0.11 | 压测工具 + 服务端 Metrics + 全档位容量测试 | ✅ |
| v0.11 | Bug 修复：RoomServiceImpl 悬空指针、Connection::OnClose UAF、RpcClient 并发 send、GetMetricsRes 字段缺失 | ✅ |
| v0.11 | perf + FlameGraph 火焰图分析（3 业务场景 × 3 并发档位） | ✅ |
| v0.12 | 代码质量 — clang-format 统一风格 + Doxygen 注释规范化 | ✅ |
| v0.12 | Docker 化部署 — Dockerfile + stdout 缓冲修复 | ✅ |
| v0.12 | Bug 修复：CatchUp 追帧 off-by-one（每次补 3 帧 → 修正为 2 帧） | ✅ |
| v0.13 | 断线重连（SessionManager 实现 + 心跳 + 快照恢复） | 🚧 进行中 |

## 演化方向

项目从纯 RPC 框架向**游戏服务端**演进。底层 RPC 六层保持不变，之上逐步叠加游戏业务模块：

```
游戏业务层（✅ v0.11 已完成，🚧 v0.12/v0.13 增补中）
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
│   │                        🚧 v0.13 — 内置服务端权威 GameState + 每帧自动快照
│   ├── InputBuffer       ← Jitter Buffer（deque, 乱序支持）✅
│   ├── GameState         ← 确定性状态更新（tickLogic）✅
│   ├── SnapshotManager   ← 环形缓冲区快照/回滚（断线重连）✅
│   ├── Reconciliation    ← 预测/和解（CompareStates + ReconcileState）✅
│   └── 房间衔接           ← StartGame自动启动帧同步 + SendInput/StopGame RPC ✅
├── 匹配系统
│   ├── EloCalculator     ← ELO 分计算 ✅
│   ├── MatchQueue        ← 匹配队列 + 超时放宽 ✅
│   └── MatchService      ← 匹配→房间→通知→超时 ✅
├── 会话与断线重连（v0.13 🚧）
│   ├── SessionManager    ← 会话生命周期 ACTIVE→DISCONNECTED→EXPIRED 🚧
│   ├── 心跳机制           ← Ping/Pong + 心跳超时判定 🚧
│   ├── 宽限期             ← 断连后 30s 保留房间位置/匹配状态 🚧
│   └── 重连恢复           ← 快照 + 批量追帧一次性下发 🚧
├── GameService           ← 集中入口: 组装全部模块 ✅
│                            🚧 v0.13 — 接入 SessionManager + 周期性 Tick 驱动
├── ServerMetrics         ← 服务端实时指标 ✅
├── 压测工具              ← 游戏业务全流程压测 ✅
└── Docker 部署           ← Dockerfile + stdout 缓冲修复 ✅ v0.12

RPC 通信层（已完成）
├── 序列化层              ← TLV（保留，不再扩展）+ Protobuf（游戏业务主力）
├── 协议帧层              ← 13 字节帧头，body 不关心内容格式
├── 网络 IO 层            ← epoll ET + Reactor
├── 线程池                ← 生产者-消费者异步回调
├── Stub / Dispatch       ← 客户端代理 + 服务端方法分发
└── Benchmark             ← RPC 框架层性能基准
```

关键转型原则：
- **RPC 六层基本零改动**：协议帧的 `body` 字段是 `vector<uint8_t>`，帧层不关心内容是 TLV 还是 Protobuf
  - 唯一例外（v0.13）：`EventLoop` 增加周期性 Tick 钩子（`SetTickInterval`/`SetTickCallback`），
    用于驱动游戏业务定时器。这是纯追加接口，不改动既有事件分发路径
- **TLV 保留不删**：作为"从零造轮子"的能力证明，但不再扩展新类型
- **Protobuf 接管游戏业务**：新增文件（`proto/`、`game/`），不修改现有框架代码
- **仓库不换**：在旧仓库上继续开发，git 历史完整记录从 RPC 框架到游戏服务器的演进过程

## 项目文件结构

```
D:\CLion\rpc\
├── include/
│   ├── rpc/                  # RPC 框架头文件（13 个）
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
│   │   ├── rpc_client.h      # RpcClient — 客户端代理 + pending 表
│   │   └── bench_stats.h     # ✅ v0.11 — 压测统计（直方图/分位数/QPS计数器）
│   └── game/                 # 游戏模块头文件（15 个）
│       ├── timer_manager.h     ✅ v0.8
│       ├── game_room.h         ✅ v0.8
│       ├── room_manager.h      ✅ v0.8
│       ├── broadcast.h         ✅ v0.8
│       ├── room_service.h      ✅ v0.8
│       ├── input_buffer.h      ✅ v0.9
│       ├── frame_sync.h        ✅ v0.9
│       ├── game_state.h        ✅ v0.9
│       ├── snapshot_manager.h  ✅ v0.9
│       ├── elo_calculator.h    ✅ v0.10
│       ├── match_queue.h       ✅ v0.10
│       ├── match_service.h     ✅ v0.10
│       ├── game_service.h      ✅ v0.10
│       ├── session_manager.h   🚧 v0.13 — 接口已定义，实现中
│       └── server_metrics.h    ✅ v0.11
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
│   └── game/                 # 游戏模块实现（14 个）
│       ├── timer_manager.cpp     ✅ v0.8
│       ├── game_room.cpp         ✅ v0.8
│       ├── room_manager.cpp      ✅ v0.8
│       ├── broadcast.cpp         ✅ v0.8
│       ├── room_service.cpp      ✅ v0.8
│       ├── input_buffer.cpp      ✅ v0.9
│       ├── frame_sync.cpp        ✅ v0.9
│       ├── game_state.cpp        ✅ v0.9
│       ├── snapshot_manager.cpp  ✅ v0.9
│       ├── elo_calculator.cpp    ✅ v0.10
│       ├── match_queue.cpp       ✅ v0.10
│       ├── match_service.cpp     ✅ v0.10
│       ├── game_service.cpp      ✅ v0.10
│       └── session_manager.cpp   🚧 v0.13 — 骨架（全 TODO）
├── proto/                    # Protobuf 协议定义（.proto，非 C++ 源码）
│   └── game.proto            # Login/Room/Frame/Match/Metrics 等消息
├── scripts/                  # 压测 / 火焰图 / 一键执行脚本
│   ├── run_baseline.sh
│   ├── run_capacity_test.sh
│   ├── run_serialize_bench.sh
│   ├── run_exception_test.sh
│   ├── run_lv3_e2e.sh
│   ├── run_profile_flamegraph.sh
│   ├── run_profile_framesync.sh
│   └── run_profile_match.sh
├── perf/                     # perf 采集脚本 + 火焰图产物
│   ├── gen_flame.sh
│   ├── run_perf.sh
│   └── *.svg
├── bench/                    # Benchmark 工具
│   ├── bench_client.cpp      # RPC 层压测（TLV vs HTTP+JSON）
│   ├── bench_server.cpp
│   ├── bench_serialize.cpp   # 序列化性能对比
│   ├── bench_game_client.cpp # ✅ v0.11 — 游戏层全流程压测（6 种模式）
│   ├── packet_frag_test.cpp
│   └── run_all.sh
├── tests/                    # ✅ 19 个独立 test target，共 260 项断言
│   ├── test_serializer.cpp       # 11 项
│   ├── test_protocol.cpp         # 16 项
│   ├── test_network.cpp          # 7 项
│   ├── test_thread_pool.cpp      # 6 项
│   ├── test_rpc.cpp              # 4 项
│   ├── test_proto_vs_tlv.cpp     # 4 项（TLV vs Protobuf 体积/速度对比）
│   ├── test_game_proto.cpp       # 7 项（游戏协议消息正确性）
│   ├── test_timer_manager.cpp    # 8 项（小顶堆定时器）
│   ├── test_game_room.cpp        # 37 项（GameRoom/RoomManager 状态机 + 超时）
│   ├── test_broadcast.cpp        # 8 项（房间广播）
│   ├── test_room_events.cpp      # 14 项（加入/离开/开始 事件通知）
│   ├── test_game_e2e.cpp         # 4 项（房间端到端流程）
│   ├── test_room_service.cpp     # 16 项（Stub → Dispatch → RoomManager）
│   ├── test_input_buffer.cpp     # 20 项（Jitter Buffer）
│   ├── test_frame_sync.cpp       # 27 项（FrameSyncManager + 追帧）
│   ├── test_game_state.cpp       # 21 项（tickLogic 确定性 + 预测/和解）
│   ├── test_snapshot_manager.cpp # 17 项（环形快照）
│   ├── test_frame_sync_flow.cpp  # 全流程模拟 + 耗时报告（无断言计数）
│   └── test_match_queue.cpp      # 33 项（匹配系统单元 + 集成）
├── docs/
│   ├── 01~06-*.md            # 各层理论文档（已提交 GitHub）
│   ├── game-room-state-machine.md
│   ├── room-service-interface.md
│   ├── frame-sync-flow-bench.md
│   ├── reconnect-design.md   # 断线重连方案设计（v0.13 依据）
│   ├── interview-prep-guide.md
│   ├── bench/                # ✅ v0.11 — 压测报告 + perf 火焰图分析
│   ├── doxygen/              # Doxygen 生成（忽略）
│   ├── devlog.md             # 工程日志（上传 GitHub）
│   └── pitfalls/             # 踩坑记录（8 篇，按模块/版本归档）
├── main.cpp                  # 游戏服务端入口（GameService::Run(8080)）
├── Dockerfile                # ✅ v0.12 — Ubuntu 24.04 + g++ + CMake + Protobuf
├── .clang-format             # ✅ v0.12 — 4 空格缩进 / 驼峰 / 下划线命名 / 中文注释
├── Doxyfile                  # ✅ v0.12 — Doxygen 配置
├── CMakeLists.txt
├── README.md
└── .gitignore
```

## 当前架构（v0.13 — 游戏服务端完整闭环）

```
客户端                                        服务端 GameService
                                              ┌──────────────────────────────┐
Login(token) ─────────────────────────────→   │ OnServerFrame                │
  ←──────────────── LoginRes(player_id)       │   ├─ Login → RegisterPlayerConn
                                              │   └─ 其他 → Dispatch::Call    │
StartGame(room_id) ───────────────────────→   │        → RoomService(8方法)   │
  ←──────────── StartGameRes + GameStartNtf   │        → EnterMatch/CancelMatch
                                              │        → Ping/Reconnect  🚧v0.13
SendInput(frame_no, input) ───────────────→   │                              │
                                              │ RoomManager                  │
                                              │   └─ GameRoom                │
                                              │        ├─ TimerManager       │
                                              │        ├─ InputBuffer        │
                                              │        ├─ FrameSyncManager   │
                                              │        │    └─ GameState 🚧  │
                                              │        └─ SnapshotManager    │
  ←──── FrameData(frame_no, inputs) 广播 ────  │   Timer 20fps Tick           │
                                              │                              │
[断网] EPOLLRDHUP ────────────────────────→   │ OnPlayerDisconnected         │
                                              │   └─ session → DISCONNECTED 🚧│
[30s 内重连] ReconnectReq(session_id, frame)  │   validateSession → 快照+追帧 │
  ←──── ReconnectRes(snapshot, catchup) ───── │   session → ACTIVE      🚧   │
[30s 未重连]                                   │   EXPIRED → 退房+退队    🚧   │
                                              └──────────────────────────────┘
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
9. **文档上传策略（按实际 .gitignore 修正）**：`docs/` 下的**理论文档、踩坑记录、压测报告、reconnect-design.md 均已提交 GitHub**（自 v0.1 起）。`.gitignore` 实际排除 `docs/doxygen/`、`docs/interview-prep-guide.md`、`.codegraph/`、`.claude/`、`build/`、`claude聊天记录/`、`实现原理/`。

### v0.13 断线重连关键决策（已与用户确认）

10. **服务端权威状态归属**：由 `FrameSyncManager` 内置 `GameState`，在 `Tick()` 中调用 `tickLogic()` 做确定性推演，并自动 `SaveSnapshot()`。理由：快照本来就是帧同步的产物，集中在一处可以避免"回调只在有输入时触发 → 空帧不推演 → 帧号错位"的坑。
11. **追帧语义分两套**：`GetCatchUpFrames()` 保持"每次最多 2 帧"（渐进追帧，用于正常运行期）；**新增批量只读取帧接口**供重连一次性下发全量缺失帧。理由：断线 5 秒（100 帧）若按 2 帧/次需 50 轮 RTT，不可接受。
12. **时钟注入**：`SessionManager::Tick()` / `Heartbeat()` / `ValidateSession()` 接受 `now_ms` 参数（默认取 `steady_clock`）。理由：15s/30s 超时若用真实 `sleep` 测试，既慢又 flaky（devlog 已记录过时间同步踩坑）。
13. **断连语义变更**：`OnPlayerDisconnected` 不再立即退房/退队，只把 session 标记为 `DISCONNECTED`；退房 + 退队迁移到**宽限期到期回调**。这是"宽限期内保留房间位置"的前提。
14. **EventLoop 增加周期性 Tick 钩子**：当前 `epoll_wait(..., -1)` 无限阻塞，全服务端无人驱动 `TimerManager`（房间超时淘汰、匹配 30s 超时、`TryMatch` 周期扫描全都失效）。v0.13 通过 `SetTickInterval` + `SetTickCallback` 补上，`epoll_wait` 使用超时参数唤醒。

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
- 测试输出统一以 `Results: N passed, M failed` 结尾（`test_frame_sync_flow` 是耗时报告，无此行）
- **当前规模：19 个 test target，260 项断言，全部通过**
- 每个模块独立测试 target，CMake 中 `rpc_lib` 是静态库，所有 test target 链接它
- **必须加 `pthread`**：`target_link_libraries(test_xxx rpc_lib pthread)`，漏了会链接失败

### 两条硬性纪律（v0.12 踩坑后确立）

1. **跑测试前先 `make`**。`build/` 里的二进制可能早于源码——v0.12 排查 `test_frame_sync`
   时，旧二进制通过、重新编译后才发现真实的断言失败。
2. **每个版本至少跑一次 ASan 全量**：
   ```bash
   cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON && make -j16
   ```
   v0.12 记录：`ENABLE_ASAN` 选项自 v0.8 起存在，但此前只用于排查单个 bug，
   从未跑过全量。2026-09 首次全量验证：**19/19 通过，零内存错误**。

### 严格告警

建议构建时加 `-Wall -Wextra`（2026-09 核对时全项目共 2 处告警，均已修复）：
```bash
cmake .. -DCMAKE_CXX_FLAGS="-Wall -Wextra"
```

## README 约定

- 中文撰写
- 分层架构图直接标注版本状态（✅/🚧/🔲）
- 开发路线图表格
- 设计文档索引

## docs/ 目录约定

- 理论文档：`docs/0X-模块名.md`
- 工程日志：`docs/devlog.md`（设计决策 + 遇到问题 + 解决方案，含版本变更记录）
- 全部中文撰写
