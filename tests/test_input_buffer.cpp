// ============================================================
// test_input_buffer — InputBuffer 单元测试
//
// 覆盖：AddInput / GetInput / ClearUpTo / Clear / 边界
// ============================================================

#include "game/input_buffer.h"

#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <string>
#include <vector>
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

static std::vector<uint8_t> MakeInput(uint8_t val) {
    return {val};
}

// ============================================================
// 任务1：构造 + 基本属性
// ============================================================

void TestConstruct() {
    game::InputBuffer buf(30);
    assert(buf.MaxFrames() == 30);
    assert(buf.FrameCount() == 0);
    assert(buf.IsEmpty());
}

void TestDefaultMaxFrames() {
    game::InputBuffer buf;
    assert(buf.MaxFrames() == 60);
}

// ============================================================
// 任务2：AddInput — 单帧单玩家
// ============================================================

void TestAddSingleInput() {
    game::InputBuffer buf;
    buf.AddInput(1, "p1", MakeInput(0xAA));
    assert(buf.FrameCount() == 1);
    assert(!buf.IsEmpty());
}

void TestAddMultipleFrames() {
    game::InputBuffer buf;
    buf.AddInput(1, "p1", MakeInput(0x01));
    buf.AddInput(2, "p1", MakeInput(0x02));
    buf.AddInput(3, "p1", MakeInput(0x03));
    assert(buf.FrameCount() == 3);
}

void TestAddMultiplePlayersSameFrame() {
    game::InputBuffer buf;
    buf.AddInput(5, "p1", MakeInput(0x11));
    buf.AddInput(5, "p2", MakeInput(0x22));
    buf.AddInput(5, "p3", MakeInput(0x33));
    assert(buf.FrameCount() == 1);  // 同一帧不增加帧数
}

// ============================================================
// 任务3：GetInput — 获取并消费
// ============================================================

void TestGetInputSinglePlayer() {
    game::InputBuffer buf;
    buf.AddInput(10, "p1", MakeInput(0xAA));

    auto inputs = buf.GetInput(10);
    assert(inputs.size() == 1);
    assert(inputs["p1"] == MakeInput(0xAA));

    // 消费后帧被移除
    assert(buf.FrameCount() == 0);
    assert(buf.IsEmpty());
}

void TestGetInputMultiplePlayers() {
    game::InputBuffer buf;
    buf.AddInput(3, "p1", MakeInput(0x01));
    buf.AddInput(3, "p2", MakeInput(0x02));
    buf.AddInput(3, "p3", MakeInput(0x03));

    auto inputs = buf.GetInput(3);
    assert(inputs.size() == 3);
    assert(inputs["p1"] == MakeInput(0x01));
    assert(inputs["p2"] == MakeInput(0x02));
    assert(inputs["p3"] == MakeInput(0x03));

    assert(buf.FrameCount() == 0);
}

void TestGetInputNonExistentFrame() {
    game::InputBuffer buf;
    buf.AddInput(1, "p1", MakeInput(0x01));

    auto inputs = buf.GetInput(999);  // 不存在的帧
    assert(inputs.empty());
    assert(buf.FrameCount() == 1);    // 已有帧不受影响
}

void TestGetInputTwiceSameFrame() {
    game::InputBuffer buf;
    buf.AddInput(7, "p1", MakeInput(0x77));

    auto first = buf.GetInput(7);
    assert(first.size() == 1);

    auto second = buf.GetInput(7);   // 帧已被消费
    assert(second.empty());
}

// ============================================================
// 任务4：ClearUpTo — 清理过期帧
// ============================================================

void TestClearUpToMiddle() {
    game::InputBuffer buf;
    buf.AddInput(1, "p1", MakeInput(0x01));
    buf.AddInput(2, "p1", MakeInput(0x02));
    buf.AddInput(3, "p1", MakeInput(0x03));
    buf.AddInput(4, "p1", MakeInput(0x04));
    buf.AddInput(5, "p1", MakeInput(0x05));

    buf.ClearUpTo(3);  // 保留帧3及之后
    assert(buf.FrameCount() == 3);  // 帧3, 4, 5

    // 帧1, 2 已清理
    assert(buf.GetInput(1).empty());
    assert(buf.GetInput(2).empty());

    // 帧3, 4, 5 仍在
    assert(buf.GetInput(3).size() == 1);
    assert(buf.GetInput(4).size() == 1);
    assert(buf.GetInput(5).size() == 1);
    assert(buf.IsEmpty());
}

void TestClearUpToAll() {
    game::InputBuffer buf;
    buf.AddInput(1, "p1", MakeInput(0x01));
    buf.AddInput(2, "p1", MakeInput(0x02));
    buf.AddInput(3, "p1", MakeInput(0x03));

    buf.ClearUpTo(100);  // 全部清理
    assert(buf.IsEmpty());
    assert(buf.FrameCount() == 0);
}

void TestClearUpToNone() {
    game::InputBuffer buf;
    buf.AddInput(10, "p1", MakeInput(0x10));
    buf.AddInput(20, "p1", MakeInput(0x20));

    buf.ClearUpTo(1);  // 阈值比所有帧都小，不清理
    assert(buf.FrameCount() == 2);
}

// ============================================================
// 任务5：容量限制 — 超过 max_frames 自动淘汰
// ============================================================

void TestMaxFramesAutoEvict() {
    game::InputBuffer buf(3);  // 仅保留 3 帧
    buf.AddInput(1, "p1", MakeInput(0x01));
    buf.AddInput(2, "p1", MakeInput(0x02));
    buf.AddInput(3, "p1", MakeInput(0x03));
    assert(buf.FrameCount() == 3);

    buf.AddInput(4, "p1", MakeInput(0x04));  // 插入帧4 → 帧1 被淘汰
    assert(buf.FrameCount() == 3);

    // 帧1 已被淘汰
    assert(buf.GetInput(1).empty());
    assert(buf.GetInput(2).size() == 1);
    assert(buf.GetInput(3).size() == 1);
    assert(buf.GetInput(4).size() == 1);
}

void TestMaxFramesOne() {
    game::InputBuffer buf(1);
    buf.AddInput(1, "p1", MakeInput(0x01));
    assert(buf.FrameCount() == 1);

    buf.AddInput(2, "p1", MakeInput(0x02));  // 淘汰帧1
    assert(buf.FrameCount() == 1);
    assert(buf.GetInput(1).empty());
    assert(buf.GetInput(2).size() == 1);
}

// ============================================================
// 任务6：乱序插入
// ============================================================

void TestInsertOutOfOrder() {
    game::InputBuffer buf;
    buf.AddInput(5, "p1", MakeInput(0x05));
    buf.AddInput(3, "p1", MakeInput(0x03));  // 插入到中间
    buf.AddInput(4, "p1", MakeInput(0x04));  // 插入到 3 和 5 之间
    buf.AddInput(1, "p1", MakeInput(0x01));  // 插入到最前

    assert(buf.FrameCount() == 4);

    // 按帧号顺序取出
    auto r1 = buf.GetInput(1); assert(r1["p1"] == MakeInput(0x01));
    auto r3 = buf.GetInput(3); assert(r3["p1"] == MakeInput(0x03));
    auto r4 = buf.GetInput(4); assert(r4["p1"] == MakeInput(0x04));
    auto r5 = buf.GetInput(5); assert(r5["p1"] == MakeInput(0x05));
}

// ============================================================
// 任务7：Clear — 全量清空
// ============================================================

void TestClear() {
    game::InputBuffer buf;
    buf.AddInput(1, "p1", MakeInput(0x01));
    buf.AddInput(2, "p1", MakeInput(0x02));
    buf.AddInput(3, "p2", MakeInput(0x03));
    assert(buf.FrameCount() == 3);

    buf.Clear();
    assert(buf.IsEmpty());
    assert(buf.FrameCount() == 0);

    // 清空后可以重新使用
    buf.AddInput(10, "p1", MakeInput(0x10));
    assert(buf.FrameCount() == 1);
}

// ============================================================
// 任务8：边界用例
// ============================================================

void TestLargeFrameNumber() {
    game::InputBuffer buf;
    buf.AddInput(0xFFFFFFFF, "p1", MakeInput(0xFF));
    auto inputs = buf.GetInput(0xFFFFFFFF);
    assert(inputs["p1"] == MakeInput(0xFF));
}

void TestOverwriteSamePlayerSameFrame() {
    game::InputBuffer buf;
    buf.AddInput(1, "p1", MakeInput(0x01));
    buf.AddInput(1, "p1", MakeInput(0x99));  // 覆盖
    buf.AddInput(1, "p2", MakeInput(0x02));

    auto inputs = buf.GetInput(1);
    assert(inputs.size() == 2);         // p1 覆盖 + p2
    assert(inputs["p1"] == MakeInput(0x99));  // 取最新值
    assert(inputs["p2"] == MakeInput(0x02));
}

void TestEmptyInputData() {
    game::InputBuffer buf;
    buf.AddInput(1, "p1", {});  // 空输入（玩家没操作）
    auto inputs = buf.GetInput(1);
    assert(inputs.size() == 1);
    assert(inputs["p1"].empty());
}

void TestClearUpToEdge() {
    game::InputBuffer buf;
    buf.AddInput(5, "p1", MakeInput(0x05));
    buf.AddInput(6, "p1", MakeInput(0x06));

    buf.ClearUpTo(5);  // 帧5 < 5? No. 帧5 >=5, 保留
    assert(buf.FrameCount() == 2);

    buf.ClearUpTo(6);  // 帧5 < 6? Yes, 清理. 帧6 保留
    assert(buf.FrameCount() == 1);
    assert(!buf.GetInput(6).empty());
}

// ============================================================
// 入口
// ============================================================

int main() {
    setbuf(stdout, NULL);

    printf("=== InputBuffer 单元测试 ===\n\n");

    printf("[构造]\n");
    RunTest("构造 + 基本属性",           TestConstruct);
    RunTest("默认 max_frames=60",        TestDefaultMaxFrames);

    printf("\n[AddInput]\n");
    RunTest("添加单个输入",              TestAddSingleInput);
    RunTest("添加多个帧",                TestAddMultipleFrames);
    RunTest("同一帧多个玩家",            TestAddMultiplePlayersSameFrame);

    printf("\n[GetInput]\n");
    RunTest("获取单玩家输入",            TestGetInputSinglePlayer);
    RunTest("获取多玩家输入",            TestGetInputMultiplePlayers);
    RunTest("获取不存在的帧",            TestGetInputNonExistentFrame);
    RunTest("同一帧获取两次",            TestGetInputTwiceSameFrame);

    printf("\n[ClearUpTo]\n");
    RunTest("清理中间帧",                TestClearUpToMiddle);
    RunTest("清理全部帧",                TestClearUpToAll);
    RunTest("阈值过小不清理",            TestClearUpToNone);

    printf("\n[容量限制]\n");
    RunTest("超量自动淘汰最旧帧",        TestMaxFramesAutoEvict);
    RunTest("max_frames=1 边界",         TestMaxFramesOne);

    printf("\n[乱序]\n");
    RunTest("乱序插入后顺序取出",        TestInsertOutOfOrder);

    printf("\n[Clear]\n");
    RunTest("Clear 全量清空",            TestClear);

    printf("\n[边界用例]\n");
    RunTest("大帧号 0xFFFFFFFF",         TestLargeFrameNumber);
    RunTest("同玩家同帧覆盖旧值",        TestOverwriteSamePlayerSameFrame);
    RunTest("空输入数据",                TestEmptyInputData);
    RunTest("ClearUpTo 边界",            TestClearUpToEdge);

    printf("\nResults: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
