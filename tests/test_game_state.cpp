// ============================================================
// test_game_state — 游戏状态 + tickLogic 确定性验证
//
// 覆盖：GameState / tickLogic 确定性 / 3玩家多帧模拟
// ============================================================

#include "game/game_state.h"

#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <string>
#include <vector>
#include <unordered_map>

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

static std::vector<uint8_t> In(uint8_t dir) { return {dir}; }

static game::PlayerPos FindPlayer(const game::GameState& s, const std::string& id) {
    for (auto& p : s.players) {
        if (p.player_id == id) return p;
    }
    return {id, 0, 0};  // 未找到
}

// ============================================================
// 任务1：ParseMoveDir / ApplyMove / EnsurePlayerExists
// ============================================================

void TestParseMoveDir() {
    assert(game::ParseMoveDir({0x01}) == game::MoveDir::UP);
    assert(game::ParseMoveDir({0x02}) == game::MoveDir::DOWN);
    assert(game::ParseMoveDir({0x03}) == game::MoveDir::LEFT);
    assert(game::ParseMoveDir({0x04}) == game::MoveDir::RIGHT);
    assert(game::ParseMoveDir({0x00}) == game::MoveDir::NONE);
    assert(game::ParseMoveDir({})    == game::MoveDir::NONE);
    assert(game::ParseMoveDir({0xFF}) == game::MoveDir::NONE);  // 非法值
}

void TestApplyMove() {
    game::PlayerPos p{"p1", 0, 0};

    game::ApplyMove(p, game::MoveDir::RIGHT, 1);
    assert(p.x == 1 && p.y == 0);

    game::ApplyMove(p, game::MoveDir::DOWN, 2);
    assert(p.x == 1 && p.y == 2);

    game::ApplyMove(p, game::MoveDir::LEFT, 3);
    assert(p.x == -2 && p.y == 2);

    game::ApplyMove(p, game::MoveDir::UP, 1);
    assert(p.x == -2 && p.y == 1);

    game::ApplyMove(p, game::MoveDir::NONE, 1);
    assert(p.x == -2 && p.y == 1);  // 不动
}

void TestEnsurePlayerExists() {
    game::GameState s;
    s.frame_no = 0;

    game::EnsurePlayerExists(s, "p1");
    assert(s.players.size() == 1);
    assert(s.players[0].player_id == "p1");

    // 重复调用不重复添加
    game::EnsurePlayerExists(s, "p1");
    assert(s.players.size() == 1);

    // 添加第二个
    game::EnsurePlayerExists(s, "p2");
    assert(s.players.size() == 2);
}

// ============================================================
// 任务2：tickLogic — 单元测试
// ============================================================

void TestTickLogicSinglePlayer() {
    game::GameState s;
    s.frame_no = 0;
    s.players.push_back({"p1", 0, 0});

    std::unordered_map<std::string, std::vector<uint8_t>> inputs;
    inputs["p1"] = In(0x04);  // RIGHT

    game::GameState next = game::tickLogic(inputs, s);
    assert(next.frame_no == 1);
    assert(next.players.size() == 1);
    assert(FindPlayer(next, "p1").x == 1);
    assert(FindPlayer(next, "p1").y == 0);
}

void TestTickLogicNoInput() {
    game::GameState s;
    s.frame_no = 5;
    s.players.push_back({"p1", 3, 4});

    std::unordered_map<std::string, std::vector<uint8_t>> inputs;  // 空

    game::GameState next = game::tickLogic(inputs, s);
    assert(next.frame_no == 6);
    // 位置不变
    assert(FindPlayer(next, "p1").x == 3);
    assert(FindPlayer(next, "p1").y == 4);
}

void TestTickLogicNewPlayerJoins() {
    game::GameState s;
    s.frame_no = 0;
    s.players.push_back({"p1", 0, 0});

    // p2 有输入但不在 players_ 中
    std::unordered_map<std::string, std::vector<uint8_t>> inputs;
    inputs["p2"] = In(0x01);  // UP
    inputs["p1"] = In(0x04);  // RIGHT

    game::GameState next = game::tickLogic(inputs, s);
    assert(next.players.size() == 2);  // p2 被自动加入
    assert(FindPlayer(next, "p2").y == -1);  // 从 (0,0) 向上
}

void TestTickLogicDeterminism() {
    // 相同输入 → 相同结果
    game::GameState s;
    s.frame_no = 0;
    s.players.push_back({"p1", 0, 0});
    s.players.push_back({"p2", 0, 0});

    std::unordered_map<std::string, std::vector<uint8_t>> inputs;
    inputs["p1"] = In(0x04);  // RIGHT
    inputs["p2"] = In(0x02);  // DOWN

    game::GameState r1 = game::tickLogic(inputs, s);
    game::GameState r2 = game::tickLogic(inputs, s);

    // 两次结果完全一致
    assert(r1.frame_no == r2.frame_no);
    assert(r1.players.size() == r2.players.size());
    assert(FindPlayer(r1, "p1").x == FindPlayer(r2, "p1").x);
    assert(FindPlayer(r1, "p1").y == FindPlayer(r2, "p1").y);
    assert(FindPlayer(r1, "p2").x == FindPlayer(r2, "p2").x);
    assert(FindPlayer(r1, "p2").y == FindPlayer(r2, "p2").y);
}

void TestTickLogicMultiFrameSeq() {
    // 连续多帧：p1 画一个正方形
    // 起点 (0,0) → RIGHT(1,0) → DOWN(1,1) → LEFT(0,1) → UP(0,0)
    game::GameState s;
    s.frame_no = 0;
    s.players.push_back({"p1", 0, 0});

    // Frame 1: RIGHT
    auto s1 = game::tickLogic({{"p1", In(0x04)}}, s);
    assert(s1.frame_no == 1);
    assert(FindPlayer(s1, "p1").x == 1 && FindPlayer(s1, "p1").y == 0);

    // Frame 2: DOWN
    auto s2 = game::tickLogic({{"p1", In(0x02)}}, s1);
    assert(s2.frame_no == 2);
    assert(FindPlayer(s2, "p1").x == 1 && FindPlayer(s2, "p1").y == 1);

    // Frame 3: LEFT
    auto s3 = game::tickLogic({{"p1", In(0x03)}}, s2);
    assert(s3.frame_no == 3);
    assert(FindPlayer(s3, "p1").x == 0 && FindPlayer(s3, "p1").y == 1);

    // Frame 4: UP
    auto s4 = game::tickLogic({{"p1", In(0x01)}}, s3);
    assert(s4.frame_no == 4);
    assert(FindPlayer(s4, "p1").x == 0 && FindPlayer(s4, "p1").y == 0);
}

// ============================================================
// 任务3：3 个模拟玩家 — 多帧一致性
// ============================================================

void TestThreePlayersMultiFrame() {
    // 3 个玩家，每人走 10 帧不同方向
    // 验证：位置由输入唯一确定（确定性）
    //
    // p1: 一直向右 (RIGHT × 10) → 预期 (10, 0)
    // p2: 一直向下 (DOWN × 10)  → 预期 (0, 10)
    // p3: 交替 (RIGHT/DOWN...)  → 预期 (5, 5)

    game::GameState s;
    s.frame_no = 0;

    // 预期位置
    int p1_x = 0, p1_y = 0;
    int p2_x = 0, p2_y = 0;
    int p3_x = 0, p3_y = 0;

    for (int f = 1; f <= 10; f++) {
        std::unordered_map<std::string, std::vector<uint8_t>> inputs;
        inputs["p1"] = In(0x04);  // RIGHT
        inputs["p2"] = In(0x02);  // DOWN
        inputs["p3"] = In((f % 2 == 1) ? 0x04 : 0x02);  // RIGHT / DOWN 交替

        // 手动计算预期
        p1_x += 1;                          // RIGHT
        p2_y += 1;                          // DOWN
        if (f % 2 == 1) p3_x += 1;           // 奇数帧 RIGHT
        else            p3_y += 1;           // 偶数帧 DOWN

        s = game::tickLogic(inputs, s);

        assert(s.frame_no == static_cast<uint32_t>(f));
        assert(FindPlayer(s, "p1").x == p1_x && FindPlayer(s, "p1").y == p1_y);
        assert(FindPlayer(s, "p2").x == p2_x && FindPlayer(s, "p2").y == p2_y);
        assert(FindPlayer(s, "p3").x == p3_x && FindPlayer(s, "p3").y == p3_y);
    }

    // 最终位置验证
    assert(FindPlayer(s, "p1").x == 10 && FindPlayer(s, "p1").y == 0);
    assert(FindPlayer(s, "p2").x == 0  && FindPlayer(s, "p2").y == 10);
    assert(FindPlayer(s, "p3").x == 5  && FindPlayer(s, "p3").y == 5);

    printf("\n    p1=(10,0) p2=(0,10) p3=(5,5)\n");
}

void TestDeterministicReplay() {
    // 跑两次完全相同的 3 玩家 10 帧模拟，最终状态应完全一致
    auto RunSim = []() -> game::GameState {
        game::GameState s; s.frame_no = 0;
        for (int f = 1; f <= 10; f++) {
            std::unordered_map<std::string, std::vector<uint8_t>> in;
            in["p1"] = In(0x04);  // RIGHT
            in["p2"] = In(0x02);  // DOWN
            in["p3"] = In(0x01);  // UP
            s = game::tickLogic(in, s);
        }
        return s;
    };

    game::GameState a = RunSim();
    game::GameState b = RunSim();

    assert(a.frame_no == b.frame_no);
    assert(a.players.size() == b.players.size());

    for (auto& pa : a.players) {
        auto pb = FindPlayer(b, pa.player_id);
        assert(pa.x == pb.x && pa.y == pb.y);
    }
}

void TestThreePlayersDifferentMoves() {
    // 3 个玩家每帧走不同方向，6 帧后验证
    game::GameState s; s.frame_no = 0;

    // Frame 1: p1→RIGHT  p2→DOWN   p3→NONE
    s = game::tickLogic({{"p1", In(0x04)}, {"p2", In(0x02)}, {"p3", In(0x00)}}, s);
    assert(FindPlayer(s, "p1").x == 1);
    assert(FindPlayer(s, "p2").y == 1);
    assert(FindPlayer(s, "p3").x == 0 && FindPlayer(s, "p3").y == 0);

    // Frame 2: p1→UP  p2→LEFT  p3→RIGHT
    s = game::tickLogic({{"p1", In(0x01)}, {"p2", In(0x03)}, {"p3", In(0x04)}}, s);
    assert(FindPlayer(s, "p1").y == -1);
    assert(FindPlayer(s, "p2").x == -1);
    assert(FindPlayer(s, "p3").x == 1);

    // Frame 3: 全部不动
    s = game::tickLogic({}, s);
    assert(FindPlayer(s, "p1").y == -1);  // 位置不变
}

// ============================================================
// 入口
// ============================================================

int main() {
    setbuf(stdout, NULL);

    printf("=== GameState / tickLogic 测试 ===\n\n");

    printf("[基础函数]\n");
    RunTest("ParseMoveDir 方向解析",      TestParseMoveDir);
    RunTest("ApplyMove 坐标更新",         TestApplyMove);
    RunTest("EnsurePlayerExists",         TestEnsurePlayerExists);

    printf("\n[tickLogic 单元]\n");
    RunTest("单玩家移动",                 TestTickLogicSinglePlayer);
    RunTest("无输入 → 位置不变",          TestTickLogicNoInput);
    RunTest("新玩家自动加入状态",         TestTickLogicNewPlayerJoins);
    RunTest("确定性：相同输入→相同状态",  TestTickLogicDeterminism);
    RunTest("多帧序列：正方形路径",       TestTickLogicMultiFrameSeq);

    printf("\n[3 玩家多帧模拟]\n");
    RunTest("3 玩家 10 帧位置验证",       TestThreePlayersMultiFrame);
    RunTest("确定性回放验证",             TestDeterministicReplay);
    RunTest("3 玩家不同方向 3 帧",        TestThreePlayersDifferentMoves);

    printf("\nResults: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
