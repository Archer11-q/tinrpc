// ============================================================
// test_frame_sync — FrameSyncManager 单元测试
//
// 覆盖：帧号管理 / 输入收集 / 帧广播 / Timer 驱动 / 追帧
// ============================================================

#include "game/frame_sync.h"
#include "game/timer_manager.h"
#include "game/input_buffer.h"

#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <set>

// ============================================================
// 简易测试框架
// ============================================================

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

static std::vector<uint8_t> In(uint8_t val) {
    return {val};
}

// ============================================================
// 任务1：构造 + 属性
// ============================================================

void TestConstruct() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf, 50);

    assert(fsm.CurrentFrame() == 0);
    assert(!fsm.IsRunning());
    assert(fsm.Fps() == 20);
    assert(fsm.HistorySize() == 0);
    assert(fsm.MaxHistorySize() == 120);
}

void TestCustomFps() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm30(&timer, &buf, 33);
    assert(fsm30.Fps() == 30);

    game::FrameSyncManager fsm10(&timer, &buf, 100);
    assert(fsm10.Fps() == 10);
}

void TestCustomHistorySize() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf, 50, 30);
    assert(fsm.MaxHistorySize() == 30);
}

// ============================================================
// 任务2：Tick — 帧号 + 输入 + 历史
// ============================================================

void TestTickIncrementsFrame() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf);

    fsm.Tick();
    assert(fsm.CurrentFrame() == 1);
    fsm.Tick();
    assert(fsm.CurrentFrame() == 2);
    fsm.Tick();
    assert(fsm.CurrentFrame() == 3);
}

void TestTickCollectsInput() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf);

    fsm.OnPlayerInput(1, "p1", In(0xAA));
    fsm.OnPlayerInput(1, "p2", In(0xBB));

    size_t count = fsm.Tick();
    assert(count == 2);
    assert(buf.FrameCount() == 0);
}

void TestTickEmptyFrame() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf);

    size_t count = fsm.Tick();
    assert(count == 0);
    assert(fsm.CurrentFrame() == 1);
}

void TestTickStoresHistory() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf, 50, 30);

    fsm.OnPlayerInput(1, "p1", In(0x01));
    fsm.Tick();
    assert(fsm.HistorySize() == 1);

    fsm.OnPlayerInput(2, "p1", In(0x02));
    fsm.Tick();
    assert(fsm.HistorySize() == 2);

    fsm.Tick(); // 空帧也记录
    assert(fsm.HistorySize() == 3);
}

void TestHistoryOverflow() {
    game::TimerManager timer;
    game::InputBuffer buf(120);
    game::FrameSyncManager fsm(&timer, &buf, 50, 3); // 仅保留 3 帧历史

    for (int i = 1; i <= 5; i++) {
        fsm.OnPlayerInput(static_cast<uint32_t>(i), "p1", In(static_cast<uint8_t>(i)));
        fsm.Tick();
    }

    // 历史已满，只保留最近 3 帧（帧 3, 4, 5）
    assert(fsm.HistorySize() == 3);
    assert(fsm.CurrentFrame() == 5);
}

// ============================================================
// 任务3：帧广播回调
// ============================================================

struct CallbackRecord {
    uint32_t frame_no = 0;
    game::FrameSyncManager::FrameInputs inputs;
};

void TestCallbackReceivesCorrectData() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf);

    std::vector<CallbackRecord> records;
    fsm.SetFrameCallback([&records](uint32_t frame_no, const auto& inputs) {
        records.push_back({frame_no, inputs});
    });

    fsm.OnPlayerInput(1, "p1", In(0x01));
    fsm.OnPlayerInput(1, "p2", In(0x02));
    fsm.Tick();

    assert(records.size() == 1);
    assert(records[0].frame_no == 1);
    assert(records[0].inputs.size() == 2);
    assert(records[0].inputs["p1"] == In(0x01));
}

void TestCallbackNotCalledForEmptyFrame() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf);

    int call_count = 0;
    fsm.SetFrameCallback([&call_count](uint32_t, const auto&) { call_count++; });
    fsm.Tick();
    assert(call_count == 0);
}

void TestCallbackMultipleFrames() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf);

    std::vector<uint32_t> frames;
    fsm.SetFrameCallback([&frames](uint32_t fn, const auto&) { frames.push_back(fn); });

    fsm.OnPlayerInput(1, "p1", In(0x01));
    fsm.OnPlayerInput(2, "p1", In(0x02));
    fsm.OnPlayerInput(3, "p1", In(0x03));

    fsm.Tick();
    assert(frames == std::vector<uint32_t>{1});
    fsm.Tick();
    assert((frames == std::vector<uint32_t>{1, 2}));
    fsm.Tick();
    assert((frames == std::vector<uint32_t>{1, 2, 3}));
}

// ============================================================
// 任务4：输入收集 — 多玩家 / 乱序 / 覆盖
// ============================================================

void TestInputMultiplePlayersPerFrame() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf);

    fsm.OnPlayerInput(1, "p1", In(0x11));
    fsm.OnPlayerInput(1, "p2", In(0x22));
    fsm.OnPlayerInput(1, "p3", In(0x33));
    assert(fsm.Tick() == 3);
}

void TestInputOutOfOrder() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf);

    fsm.OnPlayerInput(3, "p1", In(0x03));
    fsm.OnPlayerInput(1, "p1", In(0x01));
    fsm.OnPlayerInput(2, "p1", In(0x02));

    assert(fsm.Tick() == 1);
    assert(fsm.Tick() == 1);
    assert(fsm.Tick() == 1);
}

void TestInputOverwrite() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf);

    fsm.OnPlayerInput(1, "p1", In(0x01));
    fsm.OnPlayerInput(1, "p1", In(0xFF)); // 覆盖
    fsm.OnPlayerInput(1, "p2", In(0x02));

    game::FrameSyncManager::FrameInputs rec;
    fsm.SetFrameCallback([&rec](uint32_t, const auto& in) { rec = in; });
    fsm.Tick();

    assert(rec["p1"] == In(0xFF));
    assert(rec["p2"] == In(0x02));
}

// ============================================================
// 任务5：Start/Stop + Timer 驱动
// ============================================================

void TestStartStop() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf);

    assert(!fsm.IsRunning());
    fsm.Start();
    assert(fsm.IsRunning());
    fsm.Stop();
    assert(!fsm.IsRunning());
}

void TestTimerManualTick() {
    game::TimerManager timer;
    game::InputBuffer buf(120);
    game::FrameSyncManager fsm(&timer, &buf, 10);

    std::atomic<int> tick_count{0};
    fsm.SetFrameCallback([&tick_count](uint32_t, const auto&) { tick_count++; });

    fsm.OnPlayerInput(1, "p1", In(0x01));
    fsm.OnPlayerInput(2, "p1", In(0x02));
    fsm.OnPlayerInput(3, "p1", In(0x03));

    fsm.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    timer.Tick();
    assert(tick_count.load() >= 1);
    assert(fsm.CurrentFrame() >= 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    timer.Tick();
    assert(tick_count.load() >= 2);

    fsm.Stop();
    size_t before = static_cast<size_t>(tick_count.load());
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    timer.Tick();
    assert(static_cast<size_t>(tick_count.load()) == before);
}

// ============================================================
// 任务6：追帧 GetCatchUpFrames
// ============================================================

void TestCatchUpBasic() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf, 50, 120);

    // 跑 10 帧
    for (int i = 1; i <= 10; i++) {
        fsm.OnPlayerInput(static_cast<uint32_t>(i), "p1", In(static_cast<uint8_t>(i)));
        fsm.Tick();
    }
    assert(fsm.CurrentFrame() == 10);

    // 客户端在帧 5，落后 5 帧 → 每次补 2 帧（帧 6, 7）
    auto catch1 = fsm.GetCatchUpFrames(5);
    assert(catch1.size() == 2);
    assert(catch1[0].frame_no == 6);
    assert(catch1[1].frame_no == 7);
    assert(catch1[0].inputs["p1"] == In(0x06));
    assert(catch1[1].inputs["p1"] == In(0x07));

    printf("\n    第一次追帧: 帧%d, %d\n", catch1[0].frame_no, catch1[1].frame_no);
}

void TestCatchUpClientCaughtUp() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf, 50, 120);

    for (int i = 1; i <= 5; i++) {
        fsm.OnPlayerInput(static_cast<uint32_t>(i), "p1", In(0x01));
        fsm.Tick();
    }

    // 客户端已在帧 5（追上或超前）
    auto frames = fsm.GetCatchUpFrames(5);
    assert(frames.empty());

    // 客户端超前（预测帧超过了 server）
    auto frames2 = fsm.GetCatchUpFrames(10);
    assert(frames2.empty());
}

void TestCatchUpExactlyTwoFrames() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf, 50, 120);

    for (int i = 1; i <= 3; i++) {
        fsm.OnPlayerInput(static_cast<uint32_t>(i), "p1", In(0x01));
        fsm.Tick();
    }

    // 客户端在帧 1，落后 2 帧 → 补帧 2, 3（恰好 2 帧）
    auto frames = fsm.GetCatchUpFrames(1);
    assert(frames.size() == 2);
    assert(frames[0].frame_no == 2);
    assert(frames[1].frame_no == 3);
}

void TestCatchUpOneFrameOnly() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf, 50, 120);

    for (int i = 1; i <= 2; i++) {
        fsm.OnPlayerInput(static_cast<uint32_t>(i), "p1", In(0x01));
        fsm.Tick();
    }

    // 客户端在帧 1，落后 1 帧 → 补帧 2（1 帧）
    auto frames = fsm.GetCatchUpFrames(1);
    assert(frames.size() == 1);
    assert(frames[0].frame_no == 2);
}

void TestCatchUpStepByStep() {
    // 模拟追帧全过程：客户端落后 5 帧，分 3 次追完
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf, 50, 120);

    // 服务端跑到帧 10
    for (int i = 1; i <= 10; i++) {
        fsm.OnPlayerInput(static_cast<uint32_t>(i), "p1", In(static_cast<uint8_t>(i)));
        fsm.Tick();
    }

    uint32_t client_frame = 5;

    // 第 1 次追帧：帧 6, 7
    auto batch1 = fsm.GetCatchUpFrames(client_frame);
    assert(batch1.size() == 2);
    assert(batch1[0].frame_no == 6);
    assert(batch1[1].frame_no == 7);
    client_frame = batch1[1].frame_no; // 更新到帧 7

    // 第 2 次追帧：帧 8, 9
    auto batch2 = fsm.GetCatchUpFrames(client_frame);
    assert(batch2.size() == 2);
    assert(batch2[0].frame_no == 8);
    assert(batch2[1].frame_no == 9);
    client_frame = batch2[1].frame_no; // 更新到帧 9

    // 第 3 次追帧：帧 10（只剩 1 帧）
    auto batch3 = fsm.GetCatchUpFrames(client_frame);
    assert(batch3.size() == 1);
    assert(batch3[0].frame_no == 10);
    client_frame = batch3[0].frame_no; // 追上

    // 已追上
    assert(client_frame == 10);
    auto batch4 = fsm.GetCatchUpFrames(client_frame);
    assert(batch4.empty());

    printf("\n    客户端从帧5追上帧10: 6,7 → 8,9 → 10 (3次完成)\n");
}

void TestCatchUpHistoryEvicted() {
    // 历史溢出后，太旧的帧无法追帧
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf, 50, 5); // 仅保留 5 帧历史

    for (int i = 1; i <= 10; i++) {
        fsm.OnPlayerInput(static_cast<uint32_t>(i), "p1", In(static_cast<uint8_t>(i)));
        fsm.Tick();
    }
    assert(fsm.CurrentFrame() == 10);
    assert(fsm.HistorySize() <= 5);

    // 客户端落后 8 帧（在帧 2），但历史只有帧 6~10
    // 只能追到帧 3, 4（但历史中已无帧 3, 4）
    auto frames = fsm.GetCatchUpFrames(2);
    // 帧 3, 4, 5 可能已淘汰；实际返回能找到的帧
    // 只验证返回的帧号都在历史范围内
    for (auto& f : frames) {
        assert(f.frame_no >= 6); // 历史最早是帧 6
        assert(f.frame_no <= 10);
    }
}

// ============================================================
// 任务7：边界用例
// ============================================================

void TestManyPlayers() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf);

    for (int i = 0; i < 10; i++) {
        std::string pid = "player_" + std::to_string(i);
        fsm.OnPlayerInput(1, pid, In(static_cast<uint8_t>(i)));
    }
    assert(fsm.Tick() == 10);
}

void TestRapidTicks() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf);

    for (uint32_t i = 1; i <= 100; i++) {
        fsm.OnPlayerInput(i, "p1", In(static_cast<uint8_t>(i % 256)));
    }
    for (uint32_t i = 1; i <= 100; i++) {
        assert(fsm.Tick() == 1);
    }
    assert(fsm.CurrentFrame() == 100);
}

void TestTickBeyondInputBuffer() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf);

    fsm.OnPlayerInput(1, "p1", In(0x01));
    size_t total = 0;
    for (int i = 0; i < 10; i++)
        total += fsm.Tick();
    assert(total == 1);
    assert(fsm.CurrentFrame() == 10);
}

void TestCallbackSetAfterStart() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf);

    fsm.OnPlayerInput(1, "p1", In(0x01));
    int count = 0;
    fsm.SetFrameCallback([&count](uint32_t, const auto&) { count++; });
    fsm.Tick();
    assert(count == 1);
}

void TestNoCallbackSet() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf);

    fsm.OnPlayerInput(1, "p1", In(0x01));
    size_t n = fsm.Tick();
    assert(n == 1);
    assert(buf.IsEmpty());
}

// ============================================================
// 入口
// ============================================================

int main() {
    setbuf(stdout, NULL);

    printf("=== FrameSyncManager 单元测试 ===\n\n");

    printf("[构造]\n");
    RunTest("构造 + 默认属性 (20fps)", TestConstruct);
    RunTest("自定义帧率 30fps / 10fps", TestCustomFps);
    RunTest("自定义历史缓冲区大小", TestCustomHistorySize);

    printf("\n[Tick 帧号]\n");
    RunTest("Tick 帧号自增", TestTickIncrementsFrame);
    RunTest("Tick 收集输入", TestTickCollectsInput);
    RunTest("Tick 空帧（无输入）", TestTickEmptyFrame);
    RunTest("Tick 存入帧历史", TestTickStoresHistory);
    RunTest("历史溢出自动淘汰旧帧", TestHistoryOverflow);

    printf("\n[帧广播回调]\n");
    RunTest("回调接收正确数据", TestCallbackReceivesCorrectData);
    RunTest("空帧不触发回调", TestCallbackNotCalledForEmptyFrame);
    RunTest("多帧顺序回调", TestCallbackMultipleFrames);

    printf("\n[输入收集]\n");
    RunTest("同一帧多个玩家", TestInputMultiplePlayersPerFrame);
    RunTest("乱序发送顺序取出", TestInputOutOfOrder);
    RunTest("同玩家覆盖旧值", TestInputOverwrite);

    printf("\n[Start/Stop]\n");
    RunTest("Start/Stop 状态切换", TestStartStop);
    RunTest("Timer 手动驱动 Tick", TestTimerManualTick);

    printf("\n[追帧 CatchUp]\n");
    RunTest("基础追帧：落后5帧补2帧", TestCatchUpBasic);
    RunTest("已追上时不返回帧", TestCatchUpClientCaughtUp);
    RunTest("恰好落后2帧全补", TestCatchUpExactlyTwoFrames);
    RunTest("落后1帧只补1帧", TestCatchUpOneFrameOnly);
    RunTest("分步追帧：3次追上", TestCatchUpStepByStep);
    RunTest("历史淘汰后追不到旧帧", TestCatchUpHistoryEvicted);

    printf("\n[边界用例]\n");
    RunTest("10 个玩家同一帧", TestManyPlayers);
    RunTest("快速连续 100 帧", TestRapidTicks);
    RunTest("Tick 超过输入范围", TestTickBeyondInputBuffer);
    RunTest("Tick 前设置回调", TestCallbackSetAfterStart);
    RunTest("未设置回调不崩溃", TestNoCallbackSet);

    printf("\nResults: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
