// ============================================================
// test_timer_manager — TimerManager 小顶堆定时器单元测试
//
// 验证：Schedule / Cancel / Tick / 到期触发 / 惰性删除 / 空堆安全
// ============================================================

#include "game/timer_manager.h"

#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <thread>
#include <chrono>
#include <vector>
#include <atomic>

// ============================================================
// 简易测试框架
// ============================================================

static int g_passed = 0;
static int g_failed = 0;

void RunTest(const char* name, void (*fn)()) {
    printf("  %-50s ... ", name);
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
// 测试用例
// ============================================================

// 1. 空 TimerManager Tick 安全
void TestEmptyTick() {
    game::TimerManager tm;
    size_t fired = tm.Tick();
    assert(fired == 0);
    assert(tm.PendingCount() == 0);
}

// 2. 单次到期
void TestSingleFire() {
    game::TimerManager tm;

    int counter = 0;
    tm.Schedule(10, [&counter]() { counter++; });

    assert(tm.PendingCount() == 1);

    // 等待到期
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    size_t fired = tm.Tick();

    assert(fired == 1);
    assert(counter == 1);
    assert(tm.PendingCount() == 0);
}

// 3. 多次到期（同一次 Tick 中触发多个）
void TestMultipleFires() {
    game::TimerManager tm;

    int a = 0, b = 0, c = 0;
    tm.Schedule(10, [&a]() { a = 1; });
    tm.Schedule(20, [&b]() { b = 2; });
    tm.Schedule(30, [&c]() { c = 3; });

    assert(tm.PendingCount() == 3);

    // 等 35ms，三个都应该到期
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    size_t fired = tm.Tick();

    assert(fired == 3);
    assert(a == 1);
    assert(b == 2);
    assert(c == 3);
    assert(tm.PendingCount() == 0);
}

// 4. 部分到期
void TestPartialFires() {
    game::TimerManager tm;

    int a = 0, b = 0;
    tm.Schedule(10, [&a]() { a = 1; });
    tm.Schedule(5000, [&b]() { b = 2; });  // 5 秒后才到

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    size_t fired = tm.Tick();

    assert(fired == 1);
    assert(a == 1);
    assert(b == 0);  // 没到期
    assert(tm.PendingCount() == 1);  // 还有一个在堆里
}

// 5. Cancel（惰性删除）
void TestCancel() {
    game::TimerManager tm;

    int counter = 0;
    auto id = tm.Schedule(10, [&counter]() { counter++; });
    tm.Schedule(20, [&counter]() { counter++; });

    tm.Cancel(id);
    assert(tm.PendingCount() == 2);  // 惰性删除，还在堆里

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    size_t fired = tm.Tick();

    assert(fired == 1);       // 只有没 cancel 的那个触发了
    assert(counter == 1);
    assert(tm.PendingCount() == 0);  // Tick 清理了 cancelled 的
}

// 6. Cancel 不存在的 ID（不应崩溃）
void TestCancelNonExistent() {
    game::TimerManager tm;
    tm.Schedule(10, []() {});
    tm.Cancel(999);  // 不存在
    assert(tm.PendingCount() == 1);
}

// 7. 未到期 Tick 不触发
void TestTickBeforeDue() {
    game::TimerManager tm;

    int counter = 0;
    tm.Schedule(5000, [&counter]() { counter++; });  // 5 秒

    size_t fired = tm.Tick();  // 立即 Tick

    assert(fired == 0);
    assert(counter == 0);
    assert(tm.PendingCount() == 1);
}

// 8. 回调中注册新定时器
void TestScheduleInCallback() {
    game::TimerManager tm;

    int phase = 0;
    tm.Schedule(10, [&]() {
        phase = 1;
        tm.Schedule(10, [&]() { phase = 2; });
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    tm.Tick();  // 触发第一个回调 → phase=1，并注册第二个

    assert(phase == 1);
    assert(tm.PendingCount() == 1);  // 第二个 timer 在堆里

    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    tm.Tick();  // 触发第二个

    assert(phase == 2);
    assert(tm.PendingCount() == 0);
}

// ============================================================
// 入口
// ============================================================

int main() {
    printf("=== TimerManager 小顶堆定时器单元测试 ===\n\n");

    RunTest("空堆 Tick 安全",              TestEmptyTick);
    RunTest("单次到期触发",                 TestSingleFire);
    RunTest("同次 Tick 触发多个",           TestMultipleFires);
    RunTest("部分到期",                    TestPartialFires);
    RunTest("Cancel 惰性删除",             TestCancel);
    RunTest("Cancel 不存在的 ID",           TestCancelNonExistent);
    RunTest("未到期不触发",                 TestTickBeforeDue);
    RunTest("回调中注册新定时器",           TestScheduleInCallback);

    printf("\nResults: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
