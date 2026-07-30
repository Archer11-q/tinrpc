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

static std::vector<uint8_t> In(uint8_t dir) {
    return {dir};
}

static game::PlayerPos FindPlayer(const game::GameState& s, const std::string& id) {
    for (auto& p : s.players) {
        if (p.player_id == id)
            return p;
    }
    return {id, 0, 0}; // 未找到
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
    assert(game::ParseMoveDir({}) == game::MoveDir::NONE);
    assert(game::ParseMoveDir({0xFF}) == game::MoveDir::NONE); // 非法值
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
    assert(p.x == -2 && p.y == 1); // 不动
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
    inputs["p1"] = In(0x04); // RIGHT

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

    std::unordered_map<std::string, std::vector<uint8_t>> inputs; // 空

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
    inputs["p2"] = In(0x01); // UP
    inputs["p1"] = In(0x04); // RIGHT

    game::GameState next = game::tickLogic(inputs, s);
    assert(next.players.size() == 2); // p2 被自动加入
    assert(FindPlayer(next, "p2").y == -1); // 从 (0,0) 向上
}

void TestTickLogicDeterminism() {
    // 相同输入 → 相同结果
    game::GameState s;
    s.frame_no = 0;
    s.players.push_back({"p1", 0, 0});
    s.players.push_back({"p2", 0, 0});

    std::unordered_map<std::string, std::vector<uint8_t>> inputs;
    inputs["p1"] = In(0x04); // RIGHT
    inputs["p2"] = In(0x02); // DOWN

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
        inputs["p1"] = In(0x04); // RIGHT
        inputs["p2"] = In(0x02); // DOWN
        inputs["p3"] = In((f % 2 == 1) ? 0x04 : 0x02); // RIGHT / DOWN 交替

        // 手动计算预期
        p1_x += 1; // RIGHT
        p2_y += 1; // DOWN
        if (f % 2 == 1)
            p3_x += 1; // 奇数帧 RIGHT
        else
            p3_y += 1; // 偶数帧 DOWN

        s = game::tickLogic(inputs, s);

        assert(s.frame_no == static_cast<uint32_t>(f));
        assert(FindPlayer(s, "p1").x == p1_x && FindPlayer(s, "p1").y == p1_y);
        assert(FindPlayer(s, "p2").x == p2_x && FindPlayer(s, "p2").y == p2_y);
        assert(FindPlayer(s, "p3").x == p3_x && FindPlayer(s, "p3").y == p3_y);
    }

    // 最终位置验证
    assert(FindPlayer(s, "p1").x == 10 && FindPlayer(s, "p1").y == 0);
    assert(FindPlayer(s, "p2").x == 0 && FindPlayer(s, "p2").y == 10);
    assert(FindPlayer(s, "p3").x == 5 && FindPlayer(s, "p3").y == 5);

    printf("\n    p1=(10,0) p2=(0,10) p3=(5,5)\n");
}

void TestDeterministicReplay() {
    // 跑两次完全相同的 3 玩家 10 帧模拟，最终状态应完全一致
    auto RunSim = []() -> game::GameState {
        game::GameState s;
        s.frame_no = 0;
        for (int f = 1; f <= 10; f++) {
            std::unordered_map<std::string, std::vector<uint8_t>> in;
            in["p1"] = In(0x04); // RIGHT
            in["p2"] = In(0x02); // DOWN
            in["p3"] = In(0x01); // UP
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
    game::GameState s;
    s.frame_no = 0;

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
    assert(FindPlayer(s, "p1").y == -1); // 位置不变
}

// ============================================================
// 任务4：CompareStates — 服务端权威 vs 客户端预测偏差
// ============================================================

void TestCompareStatesIdentical() {
    // 完全相同 → 无偏差
    game::GameState srv, cli;
    srv.frame_no = 5;
    cli.frame_no = 5;
    srv.players.push_back({"p1", 0, 0});
    cli.players.push_back({"p1", 0, 0});

    auto corrs = game::CompareStates(srv, cli);
    assert(corrs.empty());
}

void TestCompareStatesDetectsDeviation() {
    // 客户端预测错误
    game::GameState srv, cli;
    srv.players.push_back({"p1", 5, 3}); // 服务端权威：p1 在 (5,3)
    cli.players.push_back({"p1", 4, 3}); // 客户端预测：p1 在 (4,3)
    srv.frame_no = 10;
    cli.frame_no = 10;

    auto corrs = game::CompareStates(srv, cli);
    assert(corrs.size() == 1);
    assert(corrs[0].player_id == "p1");
    assert(corrs[0].server_x == 5 && corrs[0].server_y == 3);
    assert(corrs[0].client_x == 4 && corrs[0].client_y == 3);
    assert(corrs[0].delta_x == 1 && corrs[0].delta_y == 0);
}

void TestCompareStatesMultiPlayer() {
    game::GameState srv, cli;
    srv.players.push_back({"p1", 10, 0});
    srv.players.push_back({"p2", 0, 10});
    srv.players.push_back({"p3", 5, 5});
    cli.players.push_back({"p1", 10, 0}); // p1 正确
    cli.players.push_back({"p2", 0, 8}); // p2 偏移
    cli.players.push_back({"p3", 6, 4}); // p3 偏移

    auto corrs = game::CompareStates(srv, cli);
    assert(corrs.size() == 2); // 只有 p2 和 p3
    // p1 不在修正列表
    bool has_p1 = false;
    for (auto& c : corrs)
        if (c.player_id == "p1")
            has_p1 = true;
    assert(!has_p1);
}

void TestCompareStatesMissingPlayer() {
    // 客户端还没收到新玩家的状态
    game::GameState srv, cli;
    srv.players.push_back({"p1", 0, 0});
    srv.players.push_back({"p2", 5, 5}); // 新玩家
    cli.players.push_back({"p1", 0, 0});
    // cli 没有 p2

    auto corrs = game::CompareStates(srv, cli);
    assert(corrs.size() == 1);
    assert(corrs[0].player_id == "p2");
    assert(corrs[0].client_x == 0 && corrs[0].client_y == 0); // 未找到 → (0,0)
    assert(corrs[0].delta_x == 5 && corrs[0].delta_y == 5);
}

// ============================================================
// 任务5：ReconcileState — 客户端插值平滑到权威位置
// ============================================================

void TestReconcileFullJump() {
    // alpha=1.0 → 直接跳到权威位置
    game::GameState srv, cli;
    srv.players.push_back({"p1", 10, 10});
    cli.players.push_back({"p1", 0, 0});
    srv.frame_no = 1;
    cli.frame_no = 0;

    game::ReconcileState(cli, srv, 1.0f);
    assert(cli.players[0].x == 10);
    assert(cli.players[0].y == 10);
    assert(cli.frame_no == 1);
}

void TestReconcileInterpolate() {
    // alpha=0.5 → 走到中间
    game::GameState srv, cli;
    srv.players.push_back({"p1", 10, 0});
    cli.players.push_back({"p1", 0, 0});

    game::ReconcileState(cli, srv, 0.5f);
    assert(cli.players[0].x == 5); // (0 + (10-0)*0.5)
    assert(cli.players[0].y == 0);
}

void TestReconcileSmallStep() {
    // alpha=0.3 → 小步逼近
    game::GameState srv, cli;
    srv.players.push_back({"p1", 100, 100});
    cli.players.push_back({"p1", 40, -20});

    game::ReconcileState(cli, srv, 0.3f);
    // x: 40 + (100-40)*0.3 = 40 + 18 = 58
    // y:-20 + (100-(-20))*0.3 = -20 + 36 = 16
    assert(cli.players[0].x == 58);
    assert(cli.players[0].y == 16);
}

void TestReconcileNewPlayer() {
    // 客户端状态中没有该玩家 → 直接加入
    game::GameState srv, cli;
    srv.players.push_back({"p_new", 7, 3});

    game::ReconcileState(cli, srv);
    assert(cli.players.size() == 1);
    assert(cli.players[0].player_id == "p_new");
    assert(cli.players[0].x == 7 && cli.players[0].y == 3);
}

// ============================================================
// 任务6：完整预测→和解流程模拟
// ============================================================

void TestPredictionReconciliationLoop() {
    // 模拟：客户端预测位置与服务器权威位置不一致时的和解流程
    //
    // 场景：p1 发出 RIGHT 指令
    // - 服务端：收到 input → tickLogic → p1 在 (1, 0)
    // - 客户端：立即预测移动 → p1 也在 (1, 0) ← 正常情况下一致
    // - 第 2 帧：服务端输入丢失（网络丢包），客户端继续预测
    //   - 服务端：p1 留在 (1, 0)（没收到输入）
    //   - 客户端：p1 预测到 (2, 0)（本地已执行 RIGHT）
    //   - 偏差产生！服务端 CompareStates 检测到 delta_x=1
    //   - 和解：客户端 ReconcileState alpha=0.5 → p1 到 (2+(-1)*0.5) ≈ (1, 0)
    //   - 逐步逼近权威位置

    // Step 1: 初始状态一致
    game::GameState srv, cli;
    cli.players.push_back({"p1", 0, 0});
    srv.players.push_back({"p1", 0, 0});
    assert(game::CompareStates(srv, cli).empty());

    // Step 2: 第一帧 — 服务端和客户端都收到 RIGHT
    cli = game::tickLogic({{"p1", {0x04}}}, cli);
    srv = game::tickLogic({{"p1", {0x04}}}, srv);
    assert(game::CompareStates(srv, cli).empty()); // 一致
    assert(cli.players[0].x == 1);

    // Step 3: 第二帧 — 服务端丢包，客户端继续预测
    cli = game::tickLogic({{"p1", {0x04}}}, cli); // 客户端预测 RIGHT
    // 服务端没收到输入 → 不更新
    srv = game::tickLogic({}, srv);

    // 偏差检测
    auto corrs = game::CompareStates(srv, cli);
    assert(corrs.size() == 1);
    assert(corrs[0].delta_x == -1); // 客户端超前 1 格

    // Step 4: 和解 — 客户端向权威位置插值
    game::ReconcileState(cli, srv, 0.5f);
    // 客户端: x=2, 服务端: x=1, alpha=0.5 → x = 2 + (1-2)*0.5 = 1.5 → int32=1
    assert(cli.players[0].x == 1); // 已纠正
    assert(game::CompareStates(srv, cli).empty()); // 一致

    printf("\n    丢包偏差2→1→和解恢复: 完成\n");
}

void TestReconciliationSmoothConvergence() {
    // 多步和解：偏差较大时逐步平滑逼近
    game::GameState srv, cli;
    srv.players.push_back({"p1", 0, 0});
    cli.players.push_back({"p1", 20, 0}); // 客户端严重偏离

    // 3 步和解后应接近权威位置
    game::ReconcileState(cli, srv, 0.3f); // 20→14
    assert(cli.players[0].x == 14);
    game::ReconcileState(cli, srv, 0.3f); // 14→9
    assert(cli.players[0].x == 9);
    game::ReconcileState(cli, srv, 0.3f); // 9→6
    assert(cli.players[0].x == 6);
    // 继续趋近...
    game::ReconcileState(cli, srv, 0.3f); // 6→4
    game::ReconcileState(cli, srv, 0.3f); // 4→2
    game::ReconcileState(cli, srv, 0.3f); // 2→1
    assert(cli.players[0].x == 1);

    printf("\n    20→14→9→6→4→2→1: 6步平滑收敛\n");
}

// ============================================================
// 入口
// ============================================================

int main() {
    setbuf(stdout, NULL);

    printf("=== GameState / tickLogic 测试 ===\n\n");

    printf("[基础函数]\n");
    RunTest("ParseMoveDir 方向解析", TestParseMoveDir);
    RunTest("ApplyMove 坐标更新", TestApplyMove);
    RunTest("EnsurePlayerExists", TestEnsurePlayerExists);

    printf("\n[tickLogic 单元]\n");
    RunTest("单玩家移动", TestTickLogicSinglePlayer);
    RunTest("无输入 → 位置不变", TestTickLogicNoInput);
    RunTest("新玩家自动加入状态", TestTickLogicNewPlayerJoins);
    RunTest("确定性：相同输入→相同状态", TestTickLogicDeterminism);
    RunTest("多帧序列：正方形路径", TestTickLogicMultiFrameSeq);

    printf("\n[3 玩家多帧模拟]\n");
    RunTest("3 玩家 10 帧位置验证", TestThreePlayersMultiFrame);
    RunTest("确定性回放验证", TestDeterministicReplay);
    RunTest("3 玩家不同方向 3 帧", TestThreePlayersDifferentMoves);

    printf("\n[CompareStates 服务端vs客户端偏差]\n");
    RunTest("相同状态无偏差", TestCompareStatesIdentical);
    RunTest("检测单玩家偏差", TestCompareStatesDetectsDeviation);
    RunTest("多玩家部分偏差", TestCompareStatesMultiPlayer);
    RunTest("客户端缺失玩家", TestCompareStatesMissingPlayer);

    printf("\n[ReconcileState 和解插值]\n");
    RunTest("alpha=1.0 直接跳到权威", TestReconcileFullJump);
    RunTest("alpha=0.5 插值到中间", TestReconcileInterpolate);
    RunTest("alpha=0.3 小步逼近", TestReconcileSmallStep);
    RunTest("新玩家直接加入客户端状态", TestReconcileNewPlayer);

    printf("\n[完整预测→和解流程]\n");
    RunTest("丢包偏差→和解恢复", TestPredictionReconciliationLoop);
    RunTest("严重偏离6步平滑收敛", TestReconciliationSmoothConvergence);

    printf("\nResults: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
