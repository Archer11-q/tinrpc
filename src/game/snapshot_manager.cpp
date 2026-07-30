#include "game/snapshot_manager.h"

#include <algorithm>

namespace game {

// ============================================================
// SnapshotManager
// ============================================================

SnapshotManager::SnapshotManager(size_t max_snapshots) : max_snapshots_(max_snapshots) {
}

auto SnapshotManager::FindSnapshot(uint32_t frame_no) const -> decltype(snapshots_.begin()) {
    // deque 按 frame_no 升序，二分查找
    auto it = std::lower_bound(snapshots_.begin(), snapshots_.end(), frame_no,
                               [](const Snapshot& s, uint32_t no) { return s.frame_no < no; });
    if (it != snapshots_.end() && it->frame_no == frame_no) {
        return it;
    }
    return snapshots_.end();
}

// ---- 快照存取 ----

void SnapshotManager::SaveSnapshot(uint32_t frame_no, const GameState& state) {
    // 创建快照并插入有序位置
    Snapshot snap;
    snap.frame_no = frame_no;
    snap.state = state;

    auto it = std::lower_bound(snapshots_.begin(), snapshots_.end(), frame_no,
                               [](const Snapshot& s, uint32_t no) { return s.frame_no < no; });

    if (it != snapshots_.end() && it->frame_no == frame_no) {
        // 帧号已存在：覆盖旧快照
        *it = std::move(snap);
    } else {
        snapshots_.insert(it, std::move(snap));
    }

    // 环形缓冲区：超量时淘汰最旧快照
    while (snapshots_.size() > max_snapshots_) {
        snapshots_.pop_front();
    }
}

const GameState* SnapshotManager::GetSnapshot(uint32_t frame_no) const {
    auto it = FindSnapshot(frame_no);
    if (it == snapshots_.end()) {
        return nullptr;
    }
    return &it->state;
}

// ---- 快照回滚 ----

GameState SnapshotManager::RestoreFromSnapshot(uint32_t frame_no) const {
    // TODO: 待客户端联调完善
    //   当前仅返回快照的裸副本，不执行任何网络同步或状态重建。
    //   完整流程参见头文件注释中的 5 步骤。
    //
    //   后续需要的配合：
    //   - 客户端: 实现 ReconnectReq/Res 协议，携带 last_known_frame
    //   - 客户端: 收到 ReconnectRes 后本地重放 tickLogic
    //   - 服务端: 打包 (基准快照 + 后续帧输入) 的序列化逻辑

    const GameState* snap = GetSnapshot(frame_no);
    if (snap) {
        return *snap; // 返回副本
    }

    // 快照不存在（已淘汰）→ 返回空状态，由调用方决定降级策略
    GameState empty;
    empty.frame_no = 0;
    return empty;
}

} // namespace game
