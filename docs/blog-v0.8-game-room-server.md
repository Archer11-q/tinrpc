# 游戏房间服务器设计与实现：从状态机到广播系统

> 本文记录了 TinyRPC 项目 v0.8 版本中，从零搭建游戏房间服务器的完整过程。  
> 项目仓库：[github.com/Archer11-q/tinrpc](https://github.com/Archer11-q/tinrpc)

---

## 背景

TinyRPC 最初是一个学习型 RPC 框架——自实现 TLV 序列化、协议帧编解码、epoll 网络 IO。到了 v0.7，六层通信内核已经稳定，我开始思考一个更实际的问题：**在这个框架之上，能不能搭一个真正的游戏服务端？**

v0.8 是这个转型的第一步。目标是实现一套完整的**房间服务器**——玩家可以创建房间、加入/离开、收发消息、开始游戏，而这一切都通过自研 RPC 层完成远程调用。

## 架构总览

v0.8 的四个核心模块构成了一个清晰的分层：

```
RPC 接口层    RoomService（6个RPC方法） ← 客户端通过 Stub 远程调用
  ↑
业务逻辑层    RoomManager（房间CRUD + 玩家映射）+ Broadcast（事件广播）
  ↑
领域模型层    GameRoom（六状态机 + 玩家管理 + 超时定时器）
  ↑
基础设施      TimerManager（小顶堆定时器）+ Protobuf（游戏协议）
```

各层职责严格分离：GameRoom 不感知网络，RoomManager 不感知序列化，RoomService 只做 proto ↔ 委托转译。这种分层让每一层可以独立测试——实际上我们也确实这么做了。

## GameRoom：六状态机的设计

房间是游戏服务器的核心实体。我没有简单用几个布尔标志来表示"房间能不能加入"，而是设计了显式的六状态机：

```
IDLE → WAITING → PLAYING → FINISHED → DESTROYED
         ↑                      ↑
         └── 玩家离开（人数>0）──┘
         
任何状态下最后一个玩家离开 → DESTROYED
```

**为什么需要六个状态？**

IDLE 和 WAITING 的区别很微妙但重要：IDLE 是"房间刚创建，房主还没按开始"，WAITING 是"房主点了开始，等待其他人加入"。如果只有"等待中"一个状态，就无法区分"创建后放弃"和"主动等待匹配"这两种情况——超时策略会需要不同处理。

PLAYING 和 FINISHED 的分离则是为了结算流程：游戏结束时需要展示结果、计算分数，这个阶段房间仍有意义，不应立即销毁。

状态机通过 `GameRoom::SetState()` 驱动，状态转换约束在 `AddPlayer` 和 `StartGame` 等方法中集中校验。例如只有 IDLE 或 WAITING 状态允许加入，PLAYING 状态下加入会被拒绝并返回 `ERR_ROOM_NOT_JOINABLE`。

## RoomManager：从 CRUD 到事件驱动

如果只做 `CreateRoom` + `JoinRoom` + `LeaveRoom`，RoomManager 就是一个简单的 map 封装。真正的挑战在于**玩家-房间映射**和**超时管理**。

**玩家跨房间约束**：一个玩家不能同时在一个以上房间中。RoomManager 维护了 `player_room_` 映射表（`unordered_map<player_id, room_id>`），在 CreateRoom 和 JoinRoom 时校验，LeaveRoom 时清除。这个看似简单的约束催生了 5 个 ErrorCode（ERR_PLAYER_ALREADY_IN_ROOM 等），全部通过 RPC 链路透传给了客户端。

**超时自动淘汰**：每个房间创建时注册一个 TimerManager 定时器（默认 5 分钟），到期自动标记 DESTROYED。关键的细节是：PLAYING 状态下的房间不会被超时销毁——游戏进行中不能因为超时踢人。这个判断放在定时器的回调里：

```cpp
raw_ptr->timer().Schedule(timeout_ms, [raw_ptr]() {
    if (raw_ptr->state() == ROOM_STATE_PLAYING) return;  // 游戏中不超时
    raw_ptr->SetState(ROOM_STATE_DESTROYED);
});
```

## Broadcast：事件通知的设计

房间内的状态变化需要**主动推送**给其他玩家——有人加入、有人离开、游戏开始。这些不是请求-响应模式，而是服务端主动广播。

Broadcast 的设计很简单：持有 RoomManager 引用和 `SendToPlayer` 回调。`BroadcastToRoom` 遍历房间玩家列表，跳过指定玩家（如加入者自身），逐个调用回调发送通知。回调由上层（RoomService）注入，负责实际的网络发送。

这种"数据持有 + 外部回调"的模式让 Broadcast 既不依赖具体网络实现，也不持有 Connection 对象——换一种网络层只需换一个回调。

## RoomService：RPC Service 模式

做完 RoomManager 和 Broadcast 后，最后的拼图是把它们暴露为 RPC 服务。我定义了 `RoomService` 纯虚基类——6 个方法（CreateRoom/JoinRoom/LeaveRoom/SendMessage/GetRoomList/StartGame），签名与 Dispatch 的 Handler 对齐（raw bytes → raw bytes）。

`RoomServiceImpl` 实现每个方法时遵循固定模式：**解析 Protobuf → 委托 RoomManager → 序列化响应**。错误处理统一通过 `set_error_code(result.code)` 透传，客户端拿到的不是无差别的 `success=false`，而是可编程的 ErrorCode 枚举。

客户端通过 `RoomServiceStub` 调用——封装了 Protobuf 序列化、RpcClient::Call 发送、响应反序列化。调用方看起来就像本地方法调用：

```cpp
RoomServiceStub stub(&client);
auto res = stub.CreateRoom(req);
if (res.success()) {
    std::string room_id = res.room_info().room_id();
}
```

## 断连检测：EPOLLRDHUP 的决策

玩家掉线是游戏服务器的必修课。我在 EPOLLRDHUP 和心跳超时之间做了权衡：

EPOLLRDHUP 是内核事件，客户端进程退出、崩溃、kill -9 都会触发——覆盖了 80% 以上的断连场景，零额外开销。心跳能覆盖网络断开等极端情况，但需要协议扩展和每连接定时器。

**决策**：先上 EPOLLRDHUP，心跳留给帧同步阶段（帧同步天然需要 tick 机制，届时统一实现更合理）。

实现上，在 `Connection::OnClose` 中注入 `DisconnectCallback`，服务端注册回调：

```
epoll → EPOLLRDHUP → OnClose → DisconnectCallback
  → 反查 fd→player_id → LeaveRoomAndNotify → 移除 + 广播 PlayerLeaveNtf
```

整个链路从断连到通知完成，全部在 EventLoop 线程内完成调度。

## 测试：51项，全部手写

项目不使用 Google Test，所有测试用 `assert()` + `printf()` 手写。v0.8 的测试覆盖：

- **37项** GameRoom/RoomManager 单元测试：状态转换、边界条件（max_players=1）、超时逻辑、错误码验证
- **14项** RoomService 端到端测试：6 个 RPC 方法的成功路径 + 6 个错误码链路 + 断连自动清理 + 重复创建拒绝

端到端测试最有价值的一个教训是：**永远不要把有副作用的函数放在 `assert()` 里**。CMake 的 RelWithDebInfo 构建类型默认定义 NDEBUG，导致 `assert(client.Connect(port))` 中的 Connect 从未执行——所有测试虚假通过。

## 下一步

v0.8 建立了房间服务器的骨架。接下来的帧同步系统（FrameSyncManager + InputBuffer + SnapshotManager）会在此基础上叠加，届时心跳机制也可以和帧同步的 tick 统一实现。匹配系统也在路线图上——ELO 计算和匹配队列将复用已有的 TimerManager 做超时放宽。

---

*写于 2026 年 7 月，大二暑假。*
