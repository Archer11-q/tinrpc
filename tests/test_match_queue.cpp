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
    // p1(1500) vs p2(1600)：分差 100 > 初始 50，刚入队时无匹配
    // 等待 600ms 后，范围 = 50 + 100*0.6 = 110 > 100 → 匹配成功
    game::MatchQueue mq(30, 50, 100);

    mq.EnterQueue("p1", 1500);
    mq.EnterQueue("p2", 1600);

    // 刚入队：分差 100 > 范围 50 → 无匹配，双方留在队列
    assert(mq.FindMatch("p1").empty());
    assert(mq.IsInQueue("p1"));
    assert(mq.IsInQueue("p2"));

    // 等待 600ms：p1 的放宽范围 = 50 + 100*0.6 = 110 > 分差 100
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    // 范围放宽后匹配成功，双方自动离队
    assert(mq.FindMatch("p1") == "p2");
    assert(mq.IsEmpty());
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
// 任务5: TryMatch 批量匹配
// ============================================================

void TestTryMatchExactPair() {
    game::MatchQueue mq(30, 200, 0);
    mq.EnterQueue("p1", 1500);
    mq.EnterQueue("p2", 1510);  // 分差 10 ≤ 200 → 匹配
    mq.EnterQueue("p3", 1800);
    mq.EnterQueue("p4", 1810);  // 分差 10 ≤ 200 → 匹配

    auto pairs = mq.TryMatch();
    assert(pairs.size() == 2);
    // 验证配对是相邻的
    bool has_12 = (pairs[0].first == "p1" && pairs[0].second == "p2") ||
                  (pairs[1].first == "p1" && pairs[1].second == "p2");
    bool has_34 = (pairs[0].first == "p3" && pairs[0].second == "p4") ||
                  (pairs[1].first == "p3" && pairs[1].second == "p4");
    assert(has_12 && has_34);

    assert(mq.IsEmpty());  // 全部配对出队
}

void TestTryMatchNoPair() {
    game::MatchQueue mq(30, 50, 0);
    mq.EnterQueue("p1", 1500);
    mq.EnterQueue("p2", 1600);  // 分差 100 > 50
    mq.EnterQueue("p3", 1700);  // 分差 100 > 50

    auto pairs = mq.TryMatch();
    assert(pairs.empty());
    assert(mq.QueueSize() == 3);  // 无人离队
}

void TestTryMatchPartial() {
    game::MatchQueue mq(30, 100, 0);
    mq.EnterQueue("p1", 1500);
    mq.EnterQueue("p2", 1550);  // 分差 50 ≤ 100 → 匹配
    mq.EnterQueue("p3", 1700);  // 分差 150 > 100 → 不匹配

    auto pairs = mq.TryMatch();
    assert(pairs.size() == 1);
    assert(mq.QueueSize() == 1);    // p3 留在队列
    assert(mq.IsInQueue("p3"));
}

void TestTryMatchEmptyOrSingle() {
    game::MatchQueue mq;
    assert(mq.TryMatch().empty());

    mq.EnterQueue("p1", 1500);
    assert(mq.TryMatch().empty());
    assert(mq.QueueSize() == 1);
}

void TestTryMatchWithExpandedRange() {
    // 分差 100，初始范围 50，等 700ms 后范围 50+100*0.7=120 > 100
    game::MatchQueue mq(30, 50, 100);
    mq.EnterQueue("p1", 1500);
    mq.EnterQueue("p2", 1600);

    // 刚入队：范围 50 < 分差 100 → 不匹配
    assert(mq.TryMatch().empty());

    // 等待后重新入队刷新时间，模拟定时扫描
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    mq.EnterQueue("p1", 1500);  // 刷新入队时间让范围生效
    mq.EnterQueue("p2", 1600);

    auto pairs = mq.TryMatch();
    assert(pairs.size() == 1);  // 范围放宽后匹配成功
}

void TestTryMatchThreePlayers() {
    // p1(1500), p2(1520), p3(1540): p1-p2 配对, p3 剩余
    game::MatchQueue mq(30, 50, 0);
    mq.EnterQueue("p1", 1500);
    mq.EnterQueue("p2", 1520);
    mq.EnterQueue("p3", 1540);

    auto pairs = mq.TryMatch();
    assert(pairs.size() == 1);   // p1 配 p2（不是 p2 配 p3，因 p2 已被消费）
    assert(mq.QueueSize() == 1); // p3 剩余
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

    printf("\n[TryMatch 批量匹配]\n");
    RunTest("两对精准匹配全部出队",        TestTryMatchExactPair);
    RunTest("分差过大无配对",              TestTryMatchNoPair);
    RunTest("部分配对剩余留队",            TestTryMatchPartial);
    RunTest("空队列/单人返回空",           TestTryMatchEmptyOrSingle);
    RunTest("范围放宽后批量配对",          TestTryMatchWithExpandedRange);
    RunTest("三人只配最近一对",            TestTryMatchThreePlayers);

    printf("\nResults: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
