// ============================================================
// test_frame_sync — FrameSyncManager 单元测试
//
// 覆盖：帧号管理 / 输入收集 / 帧广播 / Timer 驱动
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

static std::vector<uint8_t> MakeInput(uint8_t val) {
    return {val};
}

// 记录广播回调收到的数据
struct CallbackRecord {
    uint32_t frame_no = 0;
    std::unordered_map<std::string, std::vector<uint8_t>> inputs;
};

// ============================================================
// 任务1：构造 + 属性
// ============================================================

void TestConstruct() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf, 50);

    assert(fsm.CurrentFrame() == 0);
    assert(!fsm.IsRunning());
    assert(fsm.Fps() == 20);  // 1000/50
}

void TestCustomFps() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm30(&timer, &buf, 33);  // ~30fps
    assert(fsm30.Fps() == 30);  // 1000/33 ≈ 30

    game::FrameSyncManager fsm10(&timer, &buf, 100); // 10fps
    assert(fsm10.Fps() == 10);
}

// ============================================================
// 任务2：Tick — 帧号自增 + 输入收集
// ============================================================

void TestTickIncrementsFrame() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf);

    assert(fsm.CurrentFrame() == 0);

    fsm.Tick();  // frame 1
    assert(fsm.CurrentFrame() == 1);

    fsm.Tick();  // frame 2
    assert(fsm.CurrentFrame() == 2);

    fsm.Tick();  // frame 3
    assert(fsm.CurrentFrame() == 3);
}

void TestTickCollectsInput() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf);

    // 玩家为 frame 1 发送输入
    fsm.OnPlayerInput(1, "p1", MakeInput(0xAA));
    fsm.OnPlayerInput(1, "p2", MakeInput(0xBB));

    size_t count = fsm.Tick();  // frame 1
    assert(count == 2);
    // 消费后 frame 1 已被移除
    assert(buf.FrameCount() == 0);
}

void TestTickEmptyFrame() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf);

    // 没有玩家为 frame 1 发送输入
    size_t count = fsm.Tick();
    assert(count == 0);  // 空帧
    assert(fsm.CurrentFrame() == 1);
}

// ============================================================
// 任务3：帧广播 — 回调接收正确数据
// ============================================================

void TestCallbackReceivesCorrectData() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf);

    std::vector<CallbackRecord> records;
    fsm.SetFrameCallback([&records](uint32_t frame_no, const auto& inputs) {
        CallbackRecord r;
        r.frame_no = frame_no;
        r.inputs = inputs;
        records.push_back(r);
    });

    fsm.OnPlayerInput(1, "p1", MakeInput(0x01));
    fsm.OnPlayerInput(1, "p2", MakeInput(0x02));

    size_t count = fsm.Tick();
    assert(count == 2);
    assert(records.size() == 1);
    assert(records[0].frame_no == 1);
    assert(records[0].inputs.size() == 2);
    assert(records[0].inputs["p1"] == MakeInput(0x01));
    assert(records[0].inputs["p2"] == MakeInput(0x02));
}

void TestCallbackNotCalledForEmptyFrame() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf);

    int call_count = 0;
    fsm.SetFrameCallback([&call_count](uint32_t, const auto&) {
        call_count++;
    });

    fsm.Tick();  // 空帧，不应触发回调
    assert(call_count == 0);
}

void TestCallbackMultipleFrames() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf);

    std::vector<uint32_t> frames;
    fsm.SetFrameCallback([&frames](uint32_t frame_no, const auto&) {
        frames.push_back(frame_no);
    });

    // 准备 3 帧的输入
    fsm.OnPlayerInput(1, "p1", MakeInput(0x01));
    fsm.OnPlayerInput(2, "p1", MakeInput(0x02));
    fsm.OnPlayerInput(3, "p1", MakeInput(0x03));

    fsm.Tick(); assert(frames.size() == 1 && frames[0] == 1);
    fsm.Tick(); assert(frames.size() == 2 && frames[1] == 2);
    fsm.Tick(); assert(frames.size() == 3 && frames[2] == 3);
}

// ============================================================
// 任务4：输入收集 — 多玩家、乱序、覆盖
// ============================================================

void TestInputMultiplePlayersPerFrame() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf);

    // 3 个玩家为同一帧发送输入
    fsm.OnPlayerInput(1, "p1", MakeInput(0x11));
    fsm.OnPlayerInput(1, "p2", MakeInput(0x22));
    fsm.OnPlayerInput(1, "p3", MakeInput(0x33));

    size_t count = fsm.Tick();
    assert(count == 3);
}

void TestInputOutOfOrder() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf);

    // 乱序发送（玩家预测不同帧）
    fsm.OnPlayerInput(3, "p1", MakeInput(0x03));
    fsm.OnPlayerInput(1, "p1", MakeInput(0x01));
    fsm.OnPlayerInput(2, "p1", MakeInput(0x02));

    // Tick 按帧号顺序取出
    assert(fsm.Tick() == 1);  // frame 1
    assert(fsm.Tick() == 1);  // frame 2
    assert(fsm.Tick() == 1);  // frame 3
}

void TestInputOverwrite() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf);

    fsm.OnPlayerInput(1, "p1", MakeInput(0x01));
    fsm.OnPlayerInput(1, "p1", MakeInput(0xFF));  // 覆盖
    fsm.OnPlayerInput(1, "p2", MakeInput(0x02));

    // 验证回调收到覆盖后的值
    CallbackRecord rec;
    fsm.SetFrameCallback([&rec](uint32_t fn, const auto& in) {
        rec.frame_no = fn; rec.inputs = in;
    });

    fsm.Tick();
    assert(rec.inputs["p1"] == MakeInput(0xFF));  // 最新值
    assert(rec.inputs["p2"] == MakeInput(0x02));
}

// ============================================================
// 任务5：Timer 驱动 Tick — Start/Stop
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

void TestTimerDrivenTick() {
    game::TimerManager timer;
    game::InputBuffer buf(120);  // 大缓冲，避免淘汰
    game::FrameSyncManager fsm(&timer, &buf, 30);  // 30ms ≈ 33fps（测试用短间隔）

    std::atomic<int> tick_count{0};
    std::atomic<uint32_t> last_frame{0};
    fsm.SetFrameCallback([&tick_count, &last_frame](uint32_t fn, const auto&) {
        tick_count++;
        last_frame = fn;
    });

    // 为前几帧准备输入
    for (int i = 1; i <= 10; i++) {
        fsm.OnPlayerInput(static_cast<uint32_t>(i), "p1", MakeInput(static_cast<uint8_t>(i)));
    }

    fsm.Start();

    // 等待至少 3 次 tick（30ms * 3 = 90ms，给 200ms 余量）
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    fsm.Stop();

    // Timer 调度是单线程的——tick 在调用 timer.Tick() 时触发
    // 这里没有 EventLoop，timer 不会自动触发
    // 改为手动驱动 timer.Tick() 来模拟时间流逝
}

void TestTimerManualTick() {
    // 更实际的测试：手动驱动 TimerManager::Tick()
    game::TimerManager timer;
    game::InputBuffer buf(120);
    game::FrameSyncManager fsm(&timer, &buf, 10);  // 10ms 间隔

    std::atomic<int> tick_count{0};
    fsm.SetFrameCallback([&tick_count](uint32_t, const auto&) { tick_count++; });

    fsm.OnPlayerInput(1, "p1", MakeInput(0x01));
    fsm.OnPlayerInput(2, "p1", MakeInput(0x02));
    fsm.OnPlayerInput(3, "p1", MakeInput(0x03));

    fsm.Start();
    assert(fsm.IsRunning());

    // 手动推进时间：TimerManager 需要外部 Tick() 来触发到期定时器
    // 等待 10ms → Tick → 检查
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    timer.Tick();  // 触发到期定时器 → 调用 fsm.Tick()

    assert(tick_count.load() >= 1);
    assert(fsm.CurrentFrame() >= 1);

    // 再等一帧
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    timer.Tick();

    assert(tick_count.load() >= 2);
    assert(fsm.CurrentFrame() >= 2);

    fsm.Stop();
    assert(!fsm.IsRunning());

    size_t before = static_cast<size_t>(tick_count.load());

    // Stop 后不再触发
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    timer.Tick();
    assert(static_cast<size_t>(tick_count.load()) == before);
}

// ============================================================
// 任务6：边界用例
// ============================================================

void TestManyPlayers() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf);

    // 10 个玩家同一帧
    for (int i = 0; i < 10; i++) {
        std::string pid = "player_" + std::to_string(i);
        fsm.OnPlayerInput(1, pid, MakeInput(static_cast<uint8_t>(i)));
    }

    assert(fsm.Tick() == 10);
}

void TestRapidTicks() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf);

    // 快速连续 tick 100 帧
    for (uint32_t i = 1; i <= 100; i++) {
        fsm.OnPlayerInput(i, "p1", MakeInput(static_cast<uint8_t>(i % 256)));
    }
    for (uint32_t i = 1; i <= 100; i++) {
        size_t n = fsm.Tick();
        assert(n == 1);  // 每帧一个玩家
    }
    assert(fsm.CurrentFrame() == 100);
}

void TestTickBeyondInputBuffer() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf);

    // 只准备了 frame 1 的输入
    fsm.OnPlayerInput(1, "p1", MakeInput(0x01));

    // Tick 到 frame 10（frame 2~10 无输入）
    size_t total = 0;
    for (int i = 0; i < 10; i++) {
        total += fsm.Tick();
    }
    assert(total == 1);  // 只有第 1 帧有输入
    assert(fsm.CurrentFrame() == 10);
}

void TestCallbackSetAfterStart() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf);

    fsm.OnPlayerInput(1, "p1", MakeInput(0x01));

    // 回调在 Tick 之前设置
    int count = 0;
    fsm.SetFrameCallback([&count](uint32_t, const auto&) { count++; });
    fsm.Tick();
    assert(count == 1);
}

void TestNoCallbackSet() {
    game::TimerManager timer;
    game::InputBuffer buf(60);
    game::FrameSyncManager fsm(&timer, &buf);

    fsm.OnPlayerInput(1, "p1", MakeInput(0x01));

    // 未设置回调，Tick 不崩溃
    size_t n = fsm.Tick();
    assert(n == 1);           // 输入被取出
    assert(buf.IsEmpty());    // 已消费
}

// ============================================================
// 入口
// ============================================================

int main() {
    setbuf(stdout, NULL);

    printf("=== FrameSyncManager 单元测试 ===\n\n");

    printf("[构造]\n");
    RunTest("构造 + 默认属性 (20fps)",    TestConstruct);
    RunTest("自定义帧率 30fps / 10fps",    TestCustomFps);

    printf("\n[Tick 帧号]\n");
    RunTest("Tick 帧号自增",              TestTickIncrementsFrame);
    RunTest("Tick 收集输入",              TestTickCollectsInput);
    RunTest("Tick 空帧（无输入）",        TestTickEmptyFrame);

    printf("\n[帧广播回调]\n");
    RunTest("回调接收正确数据",           TestCallbackReceivesCorrectData);
    RunTest("空帧不触发回调",             TestCallbackNotCalledForEmptyFrame);
    RunTest("多帧顺序回调",               TestCallbackMultipleFrames);

    printf("\n[输入收集]\n");
    RunTest("同一帧多个玩家",             TestInputMultiplePlayersPerFrame);
    RunTest("乱序发送顺序取出",           TestInputOutOfOrder);
    RunTest("同玩家覆盖旧值",             TestInputOverwrite);

    printf("\n[Start/Stop]\n");
    RunTest("Start/Stop 状态切换",        TestStartStop);
    RunTest("Timer 手动驱动 Tick",        TestTimerManualTick);

    printf("\n[边界用例]\n");
    RunTest("10 个玩家同一帧",            TestManyPlayers);
    RunTest("快速连续 100 帧",            TestRapidTicks);
    RunTest("Tick 超过输入范围",          TestTickBeyondInputBuffer);
    RunTest("Tick 前设置回调",            TestCallbackSetAfterStart);
    RunTest("未设置回调不崩溃",           TestNoCallbackSet);

    printf("\nResults: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
