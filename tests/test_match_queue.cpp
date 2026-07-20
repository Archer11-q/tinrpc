// ============================================================
// test_match_queue — EloCalculator + MatchQueue 单元测试
//
// 覆盖：ELO计算 / 入队离队 / 匹配查找 / 分差放宽
// ============================================================

#include "game/elo_calculator.h"
#include "game/match_queue.h"

#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <string>
#include <thread>
#include <chrono>

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
// 任务1: EloCalculator
// ============================================================

void TestEloEqualRatings() {
    game::EloCalculator elo;
    // 同分 → 期望胜率 0.5
    double e = elo.CalcExpected(1500, 1500);
    assert(e >= 0.49 && e <= 0.51);
}

void TestEloHigherWins() {
    game::EloCalculator elo;
    // 1600 vs 1400 → 高分的期望胜率 > 0.5
    double e = elo.CalcExpected(1600, 1400);
    assert(e > 0.7);  // 约 0.76
}

void TestEloUpdateRatingWin() {
    game::EloCalculator elo(32);
    // A(1500) 胜 B(1500) → A 加分
    double new_a = elo.UpdateRating(1500, 1500, 1.0);
    assert(new_a > 1500);  // 1500 + 32*(1-0.5) = 1516
    assert(new_a >= 1515 && new_a <= 1517);
}

void TestEloUpdateRatingLose() {
    game::EloCalculator elo(32);
    // A(1500) 负 B(1500) → A 扣分
    double new_a = elo.UpdateRating(1500, 1500, 0.0);
    assert(new_a < 1500);  // 1500 + 32*(0-0.5) = 1484
    assert(new_a >= 1483 && new_a <= 1485);
}

void TestEloUpdateRatingDraw() {
    game::EloCalculator elo(32);
    // 平局 → 向期望值靠近
    double new_a = elo.UpdateRating(1500, 1600, 0.5);
    // E_A ≈ 0.36, new = 1500 + 32*(0.5-0.36) = 1504.5
    assert(new_a > 1500 && new_a < 1510);
}

void TestEloUpset() {
    game::EloCalculator elo(32);
    // 弱胜强 → 加分更多
    double new_a = elo.UpdateRating(1400, 1600, 1.0);
    // E_A ≈ 0.24, new = 1400 + 32*(1-0.24) = 1424.3
    assert(new_a > 1420);
    // 强胜弱 → 加分很少
    double new_b = elo.UpdateRating(1600, 1400, 1.0);
    // E_B ≈ 0.76, new = 1600 + 32*(1-0.76) = 1607.7
    assert(new_b < 1610);
}

void TestEloCustomK() {
    game::EloCalculator elo(16);  // 小 K → 变化小
    double new_a = elo.UpdateRating(1500, 1500, 1.0);
    assert(new_a == 1508);  // 1500 + 16*0.5 = 1508
}

// ============================================================
// 任务2: MatchQueue 入队/离队
// ============================================================

void TestEnterQueue() {
    game::MatchQueue mq;
    assert(mq.IsEmpty());

    mq.EnterQueue("p1", 1500);
    assert(mq.QueueSize() == 1);
    assert(mq.IsInQueue("p1"));
}

void TestEnterQueueMultiple() {
    game::MatchQueue mq;
    mq.EnterQueue("p1", 1500);
    mq.EnterQueue("p2", 1600);
    mq.EnterQueue("p3", 1400);
    assert(mq.QueueSize() == 3);
}

void TestEnterQueueDuplicate() {
    game::MatchQueue mq;
    mq.EnterQueue("p1", 1500);
    mq.EnterQueue("p1", 1550);  // 更新分数
    assert(mq.QueueSize() == 1);
    assert(mq.GetScore("p1") == 1550);
}

void TestLeaveQueue() {
    game::MatchQueue mq;
    mq.EnterQueue("p1", 1500);
    mq.EnterQueue("p2", 1600);

    mq.LeaveQueue("p1");
    assert(mq.QueueSize() == 1);
    assert(!mq.IsInQueue("p1"));
    assert(mq.IsInQueue("p2"));
}

void TestLeaveQueueNonExistent() {
    game::MatchQueue mq;
    mq.EnterQueue("p1", 1500);
    mq.LeaveQueue("p999");  // 不崩溃
    assert(mq.QueueSize() == 1);
}

// ============================================================
// 任务3: FindMatch 匹配查找
// ============================================================

void TestFindMatchClosest() {
    game::MatchQueue mq(30, 200, 0);  // 不随时间放宽
    mq.EnterQueue("p1", 1500);
    mq.EnterQueue("p2", 1520);
    mq.EnterQueue("p3", 1800);  // 分差太大

    std::string opp = mq.FindMatch("p1");
    assert(opp == "p2");  // 最近对手

    // 匹配后双方离队
    assert(!mq.IsInQueue("p1"));
    assert(!mq.IsInQueue("p2"));
    assert(mq.IsInQueue("p3"));  // p3 未被匹配
    assert(mq.QueueSize() == 1);
}

void TestFindMatchNoSuitable() {
    game::MatchQueue mq(30, 50, 0);  // 分差仅 50
    mq.EnterQueue("p1", 1500);
    mq.EnterQueue("p2", 1600);  // 差 100 > 50

    std::string opp = mq.FindMatch("p1");
    assert(opp.empty());  // 无合适对手
    assert(mq.QueueSize() == 2);  // 双方仍在队列
}

void TestFindMatchNotInQueue() {
    game::MatchQueue mq;
    mq.EnterQueue("p1", 1500);

    assert(mq.FindMatch("p999").empty());
}

// ============================================================
// 任务4: 超时分差放宽
// ============================================================

void TestEloRangeExpands() {
    // 初始分差 50，每秒放宽 100
    game::MatchQueue mq(30, 50, 100);

    mq.EnterQueue("p1", 1500);
    mq.EnterQueue("p2", 1600);  // 分差 100 > 初始 50

    // 刚入队时找不到匹配
    assert(mq.FindMatch("p1").empty());

    // 等待 1 秒后，分差放宽到 150 → 应能找到
    // 注：CurrentEloRange 基于真实时间，这里依赖系统时钟
    // 通过 EnterQueue 重新进入来更新入队时间
    mq.EnterQueue("p1", 1500);
    mq.EnterQueue("p2", 1600);
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    mq.EnterQueue("p1", 1500);  // 刷新入队时间（但分数不变）
    // 600ms → waited=0.6s → range = 50 + 100*0.6 = 110 > 100 → 匹配成功
    // 但 p2 的入队时间还是旧的？不，EnterQueue("p1") 只更新 p1 的入队时间
    // p2 的入队时间没变，所以 p2 的 range 可能不同
    // 简化：直接验证 600ms 后的匹配逻辑
    // 由于时间测试不稳定，改为验证：刚入队找不到，等一会儿能找到
    std::string opp = mq.FindMatch("p1");
    // p2 还在队列中（入队时间也较旧），分差 100
    // p1 的 range: 50 + 100*0.6 = 110 > 100 → 应该找到
    // 但 p2 的 range 还没计算... 不对，FindMatch 用的是 p1 的 range
    assert(!opp.empty());  // 时间放宽后应能匹配
    assert(opp == "p2");
}

void TestMatchRemovesBothPlayers() {
    game::MatchQueue mq(30, 200, 0);
    mq.EnterQueue("p1", 1500);
    mq.EnterQueue("p2", 1510);

    mq.FindMatch("p1");
    assert(mq.QueueSize() == 0);
    assert(!mq.IsInQueue("p1"));
    assert(!mq.IsInQueue("p2"));
}

// ============================================================
// 任务5: 边界用例
// ============================================================

void TestSinglePlayerNoMatch() {
    game::MatchQueue mq;
    mq.EnterQueue("p1", 1500);
    assert(mq.FindMatch("p1").empty());
}

void TestManyPlayersSorted() {
    game::MatchQueue mq(30, 200, 0);
    // 插入后应保持 ELO 升序
    mq.EnterQueue("p3", 1700);
    mq.EnterQueue("p1", 1500);
    mq.EnterQueue("p2", 1600);

    // p1(1500) → 最近是 p2(1600)
    assert(mq.FindMatch("p1") == "p2");
}

void TestEmptyQueue() {
    game::MatchQueue mq;
    assert(mq.IsEmpty());
    assert(mq.QueueSize() == 0);
    mq.LeaveQueue("nobody");  // 不崩溃
}

// ============================================================
// 入口
// ============================================================

int main() {
    setbuf(stdout, NULL);

    printf("=== EloCalculator + MatchQueue 测试 ===\n\n");

    printf("[EloCalculator]\n");
    RunTest("同分期望胜率 0.5",            TestEloEqualRatings);
    RunTest("高分期望胜率 > 0.7",           TestEloHigherWins);
    RunTest("胜方加分",                    TestEloUpdateRatingWin);
    RunTest("负方扣分",                    TestEloUpdateRatingLose);
    RunTest("平局向期望值靠近",            TestEloUpdateRatingDraw);
    RunTest("弱胜强加分更多",              TestEloUpset);
    RunTest("自定义 K 因子",               TestEloCustomK);

    printf("\n[MatchQueue 入队/离队]\n");
    RunTest("入队",                        TestEnterQueue);
    RunTest("多人入队",                    TestEnterQueueMultiple);
    RunTest("重复入队更新分数",            TestEnterQueueDuplicate);
    RunTest("离队",                        TestLeaveQueue);
    RunTest("离队不存在的玩家不崩溃",      TestLeaveQueueNonExistent);

    printf("\n[MatchQueue 匹配]\n");
    RunTest("找到最近对手",                TestFindMatchClosest);
    RunTest("分差过大无匹配",              TestFindMatchNoSuitable);
    RunTest("不在队列返回空",              TestFindMatchNotInQueue);
    RunTest("匹配后双方离队",              TestMatchRemovesBothPlayers);

    printf("\n[超时分差放宽]\n");
    RunTest("等待后分差放宽匹配成功",      TestEloRangeExpands);

    printf("\n[边界]\n");
    RunTest("单人无匹配",                  TestSinglePlayerNoMatch);
    RunTest("乱序插入后有序匹配",          TestManyPlayersSorted);
    RunTest("空队列操作",                  TestEmptyQueue);

    printf("\nResults: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
