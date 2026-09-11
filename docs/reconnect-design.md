# 断线重连方案设计

> 版本：v0.10 设计 / **v0.13 实施依据**  
> 依赖：v0.9 帧同步系统 + v0.8 EPOLLRDHUP  
> 状态：**v0.13 实施中**（5 项关键决策已于 2026-09 与用户确认，见下）
>
> **v0.13 实施补充（原设计方案的两处前提缺失，已确认修正）**
>
> 1. **服务端既要存快照，就必须先有权威状态。** 原方案假定
>    `SnapshotManager` 已在工作，但全项目除自身单元测试外**从未调用过 `SaveSnapshot`**，
>    服务端也不持有 `GameState`、从不跑 `tickLogic`。→ 决策：由 `FrameSyncManager`
>    内置 `GameState`，在 `Tick()` 中确定性推演并自动保存快照。
> 2. **追帧语义冲突。** 原第四章写"catchup_frames = N+1 到 server_current 的所有输入"，
>    但 `GetCatchUpFrames()` 按设计每次只返回 2 帧，一次重连响应根本追不上。→ 决策：
>    `GetCatchUpFrames()` 保持 2 帧（运行期渐进追帧），**另增批量只读取帧接口**供重连一次性下发。
> 3. 时钟注入：`SessionManager` 的超时判定接受 `now_ms` 参数，测试传假时间（不 sleep）。
> 4. 断连语义变更：`EPOLLRDHUP` 回调只标记 `DISCONNECTED`，退房/退队移到宽限期到期。
> 5. `EventLoop` 增加周期 Tick 钩子——原设计依赖"外部 Timer 驱动"，
>    但 `epoll_wait(..., -1)` 无限阻塞，服务端实际无人驱动任何定时器。

---

## 一、背景

v0.9 帧同步系统已具备断线重连所需的基础设施：

- **SnapshotManager**：保存最近 60 帧的 GameState 快照
- **FrameSyncManager::GetCatchUpFrames()**：每次最多返回 2 帧缺失输入
- **EPOLLRDHUP**：探测客户端断连
- **ReconcileState**：客户端位置插值平滑纠正

缺失的是**将三者串联为完整重连流程的会话管理层**。

## 二、Session 生命周期

```
客户端连接 ──→ createSession() ──→ ACTIVE
                  │
                  ├── 心跳正常 ──→ 保持 ACTIVE
                  │
                  ├── 心跳超时 ──→ DISCONNECTED（宽限期 T=30s）
                  │                    │
                  │                    ├── 宽限期内重连 ──→ ACTIVE（恢复）
                  │                    │
                  │                    └── 宽限期超时 ──→ EXPIRED → destroySession()
                  │
                  └── 主动登出 ──→ destroySession()
```

### 状态转换

| 状态 | 含义 | 允许操作 |
|------|------|---------|
| ACTIVE | 正常连接中 | 发送输入、接收帧数据 |
| DISCONNECTED | 断连但未过期 | 等待重连（客户端持有 session_id） |
| EXPIRED | 超时失效 | 不可恢复，需重新登录 |

## 三、心跳机制

```
客户端                            服务端
  │                                 │
  ├── Ping（每 5s） ──────────────→ │
  │                                 ├── 更新 last_heartbeat = now
  │ ←────────────── Pong ──────────┤
  │                                 │
  │  [网络断开]                      │
  │                                 ├── Timer 检测: now - last_heartbeat > 15s
  │                                 ├── 标记 session = DISCONNECTED
  │                                 ├── 启动 grace_timer（30s）
  │                                 │
  │  [网络恢复]                      │
  ├── ReconnectReq(session_id) ───→ │
  │                                 ├── validateSession(session_id) → player_id
  │                                 ├── 恢复 Snapshot + CatchUp 帧
  │ ←── ReconnectRes(snapshot...) ─┤
  │                                 ├── 标记 session = ACTIVE
  │                                 │
  │  [30s 内未重连]                   │
  │                                 ├── grace_timer 到期
  │                                 ├── destroySession(session_id)
  │                                 ├── RoomManager::LeaveRoom（如仍在房间）
  │                                 └── MatchQueue::CancelMatch（如仍在匹配）
```

### 参数配置

| 参数 | 建议值 | 说明 |
|------|--------|------|
| ping_interval | 5s | 客户端心跳间隔 |
| heartbeat_timeout | 15s | 服务端判定断连的阈值（3 倍 ping_interval） |
| grace_period | 30s | 断连后允许重连的宽限期 |
| 宽限期内行为 | 保留房间位置 + 匹配状态 | 断连不立即退房/退队 |

## 四、快照恢复策略

### 重连数据包

```
ReconnectRes:
  ┌─────────────────────────────────────┐
  │ 1. snapshot: GameState(frame_no=N)  │ ← 从 SnapshotManager::GetSnapshot(N)
  │    （客户端最后确认的帧号）            │
  ├─────────────────────────────────────┤
  │ 2. catchup_frames: [FrameRecord×M]  │ ← 批量接口: GetFramesAfter(N)
  │    （N+1 到 server_current 的所有输入）│    一次性返回全部缺失帧（不限于 2 帧）
  ├─────────────────────────────────────┤
  │ 3. server_frame: uint32            │ ← 服务端当前帧号
  └─────────────────────────────────────┘
```

> **v0.13 修正**：第 2 项不用 `GetCatchUpFrames()`（那是运行期"每次 2 帧"的渐进追帧），
> 而是新增的**批量只读取帧接口** `GetFramesAfter(frame_no, max_frames)`。
> 原因：断线 5 秒 ≈ 100 帧，按 2 帧/次需要 50 轮 RTT 才能追上，不可接受。
> 批量接口带 `max_frames` 上限（默认按帧历史上限），避免超长断线一次打包过大。

### 客户端恢复流程

```
1. 收到 ReconnectRes
2. 将本地 GameState 设为 snapshot（回退到 N 帧）
3. 逐帧执行 tickLogic(inputs[N+1], state) → state
4. 执行 tickLogic(inputs[N+2], state) → state
5. ...
6. 直到追上 server_frame
7. 如果仍有偏差 → ReconcileState 平滑纠正
```

## 五、重连完整流程

```
ACTIVE
  │
  ├── 断连检测（心跳超时 15s）
  │     ├── session → DISCONNECTED
  │     ├── 启动 grace_timer(30s)
  │     ├── 保留房间位置（不调 LeaveRoom）
  │     └── 保留匹配状态（不调 CancelMatch）
  │
  ├── [情况 A] 宽限期内重连 ✅
  │     ├── 客户端: ReconnectReq(session_id, last_known_frame)
  │     ├── 服务端: validateSession → player_id → 恢复 snapshot
  │     ├── 服务端: GetCatchUpFrames(last_known_frame)
  │     ├── 服务端: ReconnectRes(snapshot, catchup_frames, server_frame)
  │     ├── session → ACTIVE
  │     └── 取消 grace_timer
  │
  └── [情况 B] 宽限期超时 ❌
        ├── destroySession(session_id)
        ├── RoomManager::LeaveRoomAndNotify（如仍在房间）
        ├── MatchQueue::CancelMatch（如仍在匹配）
        └── 客户端需重新 login → createSession
```

## 六、SessionManager 接口

### 接口声明（`include/game/session_manager.h`）

```cpp
class SessionManager {
public:
    // 最大 session 数、心跳超时(ms)、宽限期(ms)
    SessionManager(size_t max_sessions = 1000,
                   int64_t heartbeat_timeout_ms = 15000,
                   int64_t grace_period_ms = 30000);

    // 创建 session（登录时调用）
    std::string CreateSession(const std::string& player_id);

    // 验证 session（重连/请求时调用，成功后重置心跳计时器）
    // 返回 player_id，session 无效返回空串
    std::string ValidateSession(const std::string& session_id);

    // 刷新心跳（客户端定期 Ping 时调用）
    bool Heartbeat(const std::string& session_id);

    // 销毁 session（登出/超时/主动踢出）
    void DestroySession(const std::string& session_id);

    // Tick：检查心跳超时 + 宽限期超时（需外部 Timer 驱动）
    // 返回本次标记为 DISCONNECTED 和 EXPIRED 的 session 数
    struct TickResult {
        size_t disconnected = 0;  // 新标记为断连的
        size_t expired = 0;       // 宽限期到期的
    };
    TickResult Tick();

private:
    // 生成 session_id（UUID 简化版）
    std::string GenerateSessionId();

    struct Session {
        std::string session_id;
        std::string player_id;
        enum State { ACTIVE, DISCONNECTED, EXPIRED } state = ACTIVE;
        int64_t last_heartbeat_ms = 0;
        int64_t disconnect_time_ms = 0;  // 断连时刻
    };
    std::unordered_map<std::string, Session> sessions_;
    std::unordered_map<std::string, std::string> player_to_session_;
    // ... 参数
};
```

### 待实现的核心逻辑（标注在 .cpp 中）

```cpp
// TODO: SessionManager::Tick() 完整逻辑
//   1. 遍历 sessions_
//   2. 对每个 ACTIVE session:
//      if (now - last_heartbeat > heartbeat_timeout) → DISCONNECTED, 记录 disconnect_time
//   3. 对每个 DISCONNECTED session:
//      if (now - disconnect_time > grace_period) → EXPIRED
//   4. 对每个 EXPIRED session:
//      → DestroySession → LeaveRoom + CancelMatch
//   5. 返回 TickResult{disconnected_count, expired_count}

// TODO: SessionManager::CreateSession
//   1. 限制 max_sessions（超过拒绝）
//   2. 同一 player_id 已存在 session → 复用旧 session（覆盖登录）
//   3. 生成 session_id → 存入 map → 建立 player_to_session_ 映射
//   4. 返回 session_id

// TODO: SessionManager::ValidateSession
//   1. 查 sessions_ map
//   2. session 存在 && state != EXPIRED → 返回 player_id
//   3. 如果 state == DISCONNECTED → 恢复为 ACTIVE + 重置心跳
//   4. 如果不存在或 EXPIRED → 返回空串
```

## 七、Proto 消息（待补充）

```protobuf
// 心跳
message PingReq { string session_id = 1; }
message PongRes { int64 server_time_ms = 1; }

// 重连请求
message ReconnectReq {
    string session_id = 1;
    uint32 last_known_frame = 2;  // 客户端最后确认的帧号
}

// 重连响应
message ReconnectRes {
    bool success = 1;
    GameState snapshot = 2;              // 基准快照
    repeated FrameRecord catchup_frames = 3;  // 缺失帧的输入
    uint32 server_frame = 4;            // 服务端当前帧号
}
```

## 八、与现有模块的集成点

| 模块 | 集成点 | 触发时机 |
|------|--------|---------|
| `RoomManager` | `LeaveRoomAndNotify` | session EXPIRED 时（宽限期后） |
| `MatchQueue` | `CancelMatch` | session EXPIRED 时 |
| `SnapshotManager` | `GetSnapshot(last_known_frame)` | 重连验证成功后 |
| `FrameSyncManager` | `GetCatchUpFrames(last_known_frame)` | 重连验证成功后 |
| `PlayerConn` | `player_conns` 映射 | session 创建/销毁时更新 |
| `EPOLLRDHUP` | 断连回调 | 现有断连回调中标记 session → DISCONNECTED（不立即销毁） |

## 九、实现计划（v0.13）

| 阶段 | 内容 | 状态 |
|------|------|:--:|
| v0.10 | 接口设计 + 方案文档 | ✅ |
| v0.13 | 前置补齐：服务端权威状态 + 每帧自动快照 + 批量取帧接口 | 🚧 |
| v0.13 | SessionManager 实现（时钟注入）+ 单元测试 | 🚧 |
| v0.13 | 心跳 Ping/Pong + ReconnectReq/Res 协议 | 🚧 |
| v0.13 | EventLoop 周期 Tick 钩子 + GameService 集成 | 🚧 |
| v0.13 | 重连流程端到端测试 | 🚧 |
