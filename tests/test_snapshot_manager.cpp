// ============================================================
// test_snapshot_manager — SnapshotManager 单元测试
//
// 覆盖：SaveSnapshot / GetSnapshot / 环形缓冲区 / RestoreFromSnapshot
// ============================================================

#include "game/snapshot_manager.h"
#include "game/game_state.h"

#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <string>
#include <vector>

static int g_passed = 0;
static int g_failed = 0;

void RunTest(const char* name, void (*fn)()) {
    printf("  %-55s ... ", name);
    try {
        fn();
        printf("[PASS]\n");
        g_passed++;
    } catch (const std::exception& e) {
        printf("[FAIL] exception: %s\n", e.what());
        g_failed++;
    } catch (...) {
        printf("[FAIL] unknown exception\n");
        g_failed++;
    }
}

// ============================================================
// 辅助
// ============================================================

static game::GameState MakeState(uint32_t frame_no, int px, int py) {
    game::GameState s;
    s.frame_no = frame_no;
    s.players.push_back({"p1", px, py});
    return s;
}

// ============================================================
// 任务1：构造 + 基本属性
// ============================================================

void TestConstruct() {
    game::SnapshotManager sm;
    assert(sm.MaxSnapshots() == 60);
    assert(sm.Count() == 0);
    assert(sm.IsEmpty());
}

void TestCustomMaxSnapshots() {
    game::SnapshotManager sm(30);
    assert(sm.MaxSnapshots() == 30);
}

// ============================================================
// 任务2：SaveSnapshot + GetSnapshot
// ============================================================

void TestSaveAndGet() {
    game::SnapshotManager sm;
    auto s1 = MakeState(1, 0, 0);
    sm.SaveSnapshot(1, s1);

    assert(sm.Count() == 1);
    const game::GameState* got = sm.GetSnapshot(1);
    assert(got != nullptr);
    assert(got->frame_no == 1);
    assert(got->players[0].x == 0 && got->players[0].y == 0);
}

void TestSaveAndGetMultiple() {
    game::SnapshotManager sm;
    for (int i = 1; i <= 5; i++) {
        sm.SaveSnapshot(static_cast<uint32_t>(i), MakeState(static_cast<uint32_t>(i), i, i * 2));
    }

    assert(sm.Count() == 5);
    for (uint32_t i = 1; i <= 5; i++) {
        auto* s = sm.GetSnapshot(i);
        assert(s != nullptr);
        assert(s->frame_no == i);
        assert(s->players[0].x == static_cast<int>(i));
        assert(s->players[0].y == static_cast<int>(i * 2));
    }
}

void TestGetNonExistent() {
    game::SnapshotManager sm;
    sm.SaveSnapshot(1, MakeState(1, 0, 0));

    assert(sm.GetSnapshot(0) == nullptr);     // 不存在的帧
    assert(sm.GetSnapshot(2) == nullptr);     // 未来帧
    assert(sm.GetSnapshot(999) == nullptr);   // 远未来
}

void TestOverwriteSnapshot() {
    game::SnapshotManager sm;
    sm.SaveSnapshot(5, MakeState(5, 0, 0));
    sm.SaveSnapshot(5, MakeState(5, 10, 20));  // 覆盖

    auto* s = sm.GetSnapshot(5);
    assert(s != nullptr);
    assert(s->players[0].x == 10);  // 新值
    assert(sm.Count() == 1);        // 不增加
}

// ============================================================
// 任务3：环形缓冲区 — 容量限制 + 自动淘汰
// ============================================================

void TestRingBufferAutoEvict() {
    game::SnapshotManager sm(3);  // 仅保留 3 帧

    sm.SaveSnapshot(1, MakeState(1, 0, 0));
    sm.SaveSnapshot(2, MakeState(2, 0, 0));
    sm.SaveSnapshot(3, MakeState(3, 0, 0));
    assert(sm.Count() == 3);

    sm.SaveSnapshot(4, MakeState(4, 0, 0));  // 淘汰帧 1
    assert(sm.Count() == 3);

    assert(sm.GetSnapshot(1) == nullptr);  // 已淘汰
    assert(sm.GetSnapshot(2) != nullptr);
    assert(sm.GetSnapshot(3) != nullptr);
    assert(sm.GetSnapshot(4) != nullptr);
}

void TestRingBufferManyFrames() {
    game::SnapshotManager sm(5);

    // 存 20 帧，只保留最后 5 帧
    for (int i = 1; i <= 20; i++) {
        sm.SaveSnapshot(static_cast<uint32_t>(i), MakeState(static_cast<uint32_t>(i), i, 0));
    }

    assert(sm.Count() == 5);
    // 帧 1~15 已淘汰
    for (int i = 1; i <= 15; i++) {
        assert(sm.GetSnapshot(static_cast<uint32_t>(i)) == nullptr);
    }
    // 帧 16~20 仍在
    for (int i = 16; i <= 20; i++) {
        assert(sm.GetSnapshot(static_cast<uint32_t>(i)) != nullptr);
    }
}

void TestRingBufferSizeOne() {
    game::SnapshotManager sm(1);
    sm.SaveSnapshot(1, MakeState(1, 0, 0));
    assert(sm.Count() == 1);

    sm.SaveSnapshot(2, MakeState(2, 0, 0));  // 淘汰帧 1
    assert(sm.Count() == 1);
    assert(sm.GetSnapshot(1) == nullptr);
    assert(sm.GetSnapshot(2) != nullptr);
}

// ============================================================
// 任务4：RestoreFromSnapshot — 基础 + 占位接口
// ============================================================

void TestRestoreFromSnapshot() {
    game::SnapshotManager sm;
    sm.SaveSnapshot(10, MakeState(10, 5, 3));

    game::GameState restored = sm.RestoreFromSnapshot(10);
    assert(restored.frame_no == 10);
    assert(restored.players.size() == 1);
    assert(restored.players[0].x == 5);
    assert(restored.players[0].y == 3);
}

void TestRestoreNonExistentReturnsEmpty() {
    game::SnapshotManager sm;

    // 无快照可恢复 → 返回空状态（frame_no=0）
    game::GameState restored = sm.RestoreFromSnapshot(10);
    assert(restored.frame_no == 0);
    assert(restored.players.empty());
}

void TestRestoreEvictedSnapshot() {
    game::SnapshotManager sm(3);
    sm.SaveSnapshot(1, MakeState(1, 0, 0));
    sm.SaveSnapshot(2, MakeState(2, 1, 1));
    sm.SaveSnapshot(3, MakeState(3, 2, 2));
    sm.SaveSnapshot(4, MakeState(4, 3, 3));  // 淘汰帧 1

    // 帧 1 已被淘汰 → 返回空状态
    game::GameState restored = sm.RestoreFromSnapshot(1);
    assert(restored.frame_no == 0);

    // 帧 4 仍在 → 正常恢复
    game::GameState r2 = sm.RestoreFromSnapshot(4);
    assert(r2.frame_no == 4);
    assert(r2.players[0].x == 3);
}

void TestRestoreSnapshotIsCopy() {
    // 验证 RestoreFromSnapshot 返回的是副本（修改不影响缓冲区）
    game::SnapshotManager sm;
    sm.SaveSnapshot(1, MakeState(1, 0, 0));

    game::GameState restored = sm.RestoreFromSnapshot(1);
    restored.players[0].x = 999;  // 修改副本

    // 缓冲区中的快照不变
    auto* orig = sm.GetSnapshot(1);
    assert(orig != nullptr);
    assert(orig->players[0].x == 0);  // 原值不变
}

// ============================================================
// 任务5：Clear
// ============================================================

void TestClear() {
    game::SnapshotManager sm;
    sm.SaveSnapshot(1, MakeState(1, 0, 0));
    sm.SaveSnapshot(2, MakeState(2, 0, 0));
    sm.SaveSnapshot(3, MakeState(3, 0, 0));
    assert(sm.Count() == 3);

    sm.Clear();
    assert(sm.IsEmpty());
    assert(sm.Count() == 0);
    assert(sm.GetSnapshot(1) == nullptr);
}

// ============================================================
// 任务6：边界用例
// ============================================================

void TestLargeFrameNumbers() {
    game::SnapshotManager sm;
    sm.SaveSnapshot(0xFFFFFFF0, MakeState(0xFFFFFFF0, 1, 1));
    auto* s = sm.GetSnapshot(0xFFFFFFF0);
    assert(s != nullptr);
    assert(s->frame_no == 0xFFFFFFF0);
}

void TestEmptyStateSnapshot() {
    game::SnapshotManager sm;
    game::GameState empty_state;
    empty_state.frame_no = 1;

    sm.SaveSnapshot(1, empty_state);
    auto* s = sm.GetSnapshot(1);
    assert(s != nullptr);
    assert(s->players.empty());
}

void TestMultiPlayerSnapshot() {
    game::SnapshotManager sm;
    game::GameState state;
    state.frame_no = 1;
    state.players.push_back({"p1", 0, 0});
    state.players.push_back({"p2", 10, 20});
    state.players.push_back({"p3", -5, 15});

    sm.SaveSnapshot(1, state);
    auto* s = sm.GetSnapshot(1);
    assert(s != nullptr);
    assert(s->players.size() == 3);
    assert(s->players[0].player_id == "p1");
    assert(s->players[1].player_id == "p2");
    assert(s->players[2].player_id == "p3");
}

// ============================================================
// 入口
// ============================================================

int main() {
    setbuf(stdout, NULL);

    printf("=== SnapshotManager 单元测试 ===\n\n");

    printf("[构造]\n");
    RunTest("构造 + 默认 max=60",          TestConstruct);
    RunTest("自定义 max=30",               TestCustomMaxSnapshots);

    printf("\n[SaveSnapshot + GetSnapshot]\n");
    RunTest("保存并获取单帧快照",          TestSaveAndGet);
    RunTest("多帧快照保存获取",            TestSaveAndGetMultiple);
    RunTest("不存在的帧返回 nullptr",       TestGetNonExistent);
    RunTest("覆盖同帧号快照",              TestOverwriteSnapshot);

    printf("\n[环形缓冲区]\n");
    RunTest("容量满自动淘汰最旧帧",        TestRingBufferAutoEvict);
    RunTest("20帧淘汰保留最后5帧",          TestRingBufferManyFrames);
    RunTest("max=1 边界",                  TestRingBufferSizeOne);

    printf("\n[RestoreFromSnapshot]\n");
    RunTest("从快照恢复状态",              TestRestoreFromSnapshot);
    RunTest("不存在返回空状态",            TestRestoreNonExistentReturnsEmpty);
    RunTest("已淘汰快照返回空状态",        TestRestoreEvictedSnapshot);
    RunTest("恢复的是副本不污染缓冲区",    TestRestoreSnapshotIsCopy);

    printf("\n[Clear]\n");
    RunTest("Clear 清空所有快照",          TestClear);

    printf("\n[边界用例]\n");
    RunTest("大帧号 0xFFFFFFF0",           TestLargeFrameNumbers);
    RunTest("空状态快照",                  TestEmptyStateSnapshot);
    RunTest("多玩家快照",                  TestMultiPlayerSnapshot);

    printf("\nResults: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
