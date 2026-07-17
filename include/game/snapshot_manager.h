#pragma once

#include "game/game_state.h"

#include <string>
#include <vector>
#include <cstdint>
#include <deque>
#include <cstddef>

namespace game {

// ============================================================
// SnapshotManager — 快照管理器（帧同步断线重连基础）
//
// 职责：
// - 保存最近 N 帧的 GameState 快照（环形缓冲区，默认 60 帧）
// - 按帧号查询历史快照
// - 提供快照回滚入口（restoreFromSnapshot，待客户端联调完善）
//
// 使用方式：
//   FrameSyncManager::Tick() 后布 → tickLogic → SaveSnapshot(new_state)
//   断线重连时 → GetSnapshot(client_frame) → 恢复基础状态 → 追帧加速
//
// 当前完成度（v0.9）：
//   ✅ saveSnapshot / getSnapshot — 环形缓冲区增删查
//   ✅ 环形缓冲区自动淘汰（超过 max_snapshots 时淘汰最旧帧）
//   🚧 restoreFromSnapshot — 接口已定义，内部仅返回快照副本；
//      实际的"服务端从快照恢复 + 客户端同步"流程需要客户端联调后完善：
//      1) 客户端断线后发送 ReconnectReq（携带最后已知帧号）
//      2) 服务端调用 restoreFromSnapshot(client_frame) 获取基准状态
//      3) 服务端用 GetCatchUpFrames(client_frame) 获取缺失帧输入
//      4) 将快照状态 + 缺失帧数据打包为 ReconnectRes 发给客户端
//      5) 客户端本地重放 tickLogic 追上当前帧
//
// 线程模型：所有方法在 EventLoop IO 线程调用，单线程无锁。
// ============================================================
class SnapshotManager {
public:
    // max_snapshots: 最大快照数（默认 60 帧 ≈ 3 秒 @20fps）
    explicit SnapshotManager(size_t max_snapshots = 60);

    // ---- 快照存取 ----

    // 保存当前帧的游戏状态快照
    // frame_no: 帧号（应与 GameState.frame_no 一致）
    // state: 该帧的完整游戏状态
    void SaveSnapshot(uint32_t frame_no, const GameState& state);

    // 获取指定帧的游戏状态快照
    // 返回 nullptr 表示快照不存在（已被淘汰或从未保存）
    const GameState* GetSnapshot(uint32_t frame_no) const;

    // ---- 快照回滚（断线重连入口） ----

    // 从指定帧的快照恢复游戏状态
    // frame_no: 客户端最后同步的帧号
    // 返回：该帧的完整状态副本（快照不存在时返回 frame_no=0 的空状态）
    //
    // TODO: 待客户端联调完善
    //   当前仅返回快照的裸副本。完整流程需要：
    //   1. 服务端用返回的 GameState 作为基准
    //   2. 结合 FrameSyncManager::GetCatchUpFrames(frame_no) 获取后续帧输入
    //   3. 将 (基准状态 + 后续帧输入) 打包发给客户端
    //   4. 客户端本地逐帧执行 tickLogic 追上服务端当前帧
    GameState RestoreFromSnapshot(uint32_t frame_no) const;

    // ---- 查询 ----

    size_t Count()        const { return snapshots_.size(); }
    size_t MaxSnapshots() const { return max_snapshots_; }
    bool   IsEmpty()      const { return snapshots_.empty(); }
    void   Clear()              { snapshots_.clear(); }

private:
    struct Snapshot {
        uint32_t   frame_no = 0;
        GameState  state;
    };

    std::deque<Snapshot> snapshots_;
    size_t max_snapshots_;

    // 二分查找 frame_no，返回迭代器（未找到返回 end()）
    auto FindSnapshot(uint32_t frame_no) const -> decltype(snapshots_.begin());
};

} // namespace game
