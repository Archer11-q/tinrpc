# 帧同步系统实现：从 Jitter Buffer 到追帧的框架设计

> 本文记录了 TinyRPC v0.9 版本中，从零搭建帧同步系统的完整过程，涵盖 InputBuffer、FrameSyncManager、GameState、追帧、快照与和解六个核心模块。  
> 项目仓库：[github.com/Archer11-q/tinrpc](https://github.com/Archer11-q/tinrpc)

---

## 为什么需要自己写帧同步

市面上成熟的帧同步方案（GGPO、Photon Quantum）封装完整但黑盒，对理解底层原理帮助有限。我在完成了 RPC 框架和房间服务器之后，自然面临一个问题：**多个玩家在一个房间里，如何让所有人都看到相同的游戏画面**？答案就是帧同步——所有客户端执行相同的确定性逻辑，服务端负责收集和转发输入。

v0.9 的帧同步系统由五层组成：输入缓冲（InputBuffer）→ 帧管理器（FrameSyncManager）→ 状态更新（tickLogic）→ 追帧（CatchUp）→ 快照与和解（SnapshotManager + Reconciliation）。

## InputBuffer：乱序世界的秩序入口

帧同步的第一个挑战是网络抖动。三个玩家分别从不同延迟的网络发送输入，帧 3 的输入可能比帧 2 先到达服务端。如果按到达顺序直接消费，必然导致逻辑错乱。

InputBuffer 用 `std::deque` 解决了这个问题。每个帧的输入被包装为 `FrameInput` 结构（帧号 + 玩家输入映射），通过 `std::lower_bound` 二分查找插入位置，保证 deque 始终按帧号升序排列。同一帧号不同玩家的输入合并到同一个 `FrameInput` 中；同一玩家同一帧重复发送则取最新值（覆盖去重）。

环形缓冲区机制用 `max_frames_` 限制内存：超过 60 帧自动 `pop_front` 淘汰最旧数据。这是一个典型的 Jitter Buffer 实现，用几十行代码表达了核心理念。

## FrameSyncManager：帧同步引擎

InputBuffer 解决了"如何存"的问题，FrameSyncManager 解决"何时取、取后做什么"的问题。

它由 TimerManager 驱动，默认每 50ms（20fps）触发一次 `Tick()`。Tick 的逻辑简洁：帧号自增 → 从 InputBuffer 取出当前帧所有输入 → 回调广播 `FrameData`。广播内容被封装为 Protobuf 消息——帧号加上所有玩家的输入列表，客户端收到后在本地执行相同的 `tickLogic`，得到相同的游戏状态。

帧同步与房间状态机的衔接也在这里：`RoomService::StartGame` 成功后自动调用 `GameRoom::InitFrameSync` + `StartFrameSync`，游戏结束时 `StopGame` 停止帧同步并将房间状态标记为 `FINISHED`。这种衔接让上层只需调用 StartGame，不必关心帧同步的初始化细节。

## tickLogic：确定性的核心

帧同步要求"相同输入产生相同状态"。tickLogic 通过三条规则保证确定性：(1) 玩家按 `player_id` 字典序排序后处理，(2) 无随机数、无外部状态依赖，(3) 输入数据仅取第一个字节作为 `MoveDir` 枚举。

在 3 玩家 10 帧的模拟测试中，两次独立运行产生完全相同的最终位置（p1 向右到 (10,0)、p2 向下到 (0,10)、p3 交替到 (5,5)），验证了确定性的可靠性。

## CatchUp：慢客户端的加速追赶

当某个客户端因网络波动落后数帧时，直接丢弃它是不现实的。CatchUp 策略是：每次最多补发 2 帧。

FrameSyncManager 的每次 Tick 将帧数据存入 `frame_history_`（`deque<FrameRecord>`，最多 120 帧）。当落后客户端请求追帧时，`GetCatchUpFrames(client_frame)` 从历史中取出 `[client_frame+1, min(client_frame+3, current_frame)]` 的帧——每次最多 2 帧，既加速又避免一次灌入过多数据。

## SnapshotManager 与 Reconciliation：

快照管理器用同样的环形缓冲区保存最近 60 帧的 `GameState`，为断线重连提供基准状态。预测与和解（Reconciliation）则处理另一类问题：客户端本地预测的坐标与服务端权威坐标不一致时，`CompareStates` 计算偏差，`ReconcileState` 用 alpha 插值平滑纠正。

## 写在最后

v0.9 的帧同步系统完成了核心闭环。模拟测试中，Tick 平均耗时仅 0.4 微秒——CPU 计算开销不到帧间隔（50ms）的 0.002%。真正的挑战在网络层：延迟、丢包与多端一致性的权衡，这也是后续优化的方向。

---

*写于 2026 年 7 月，大二暑假。*
