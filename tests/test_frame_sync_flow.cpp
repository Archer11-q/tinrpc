// ============================================================
// test_frame_sync_flow — 帧同步全流程模拟 + 耗时记录
//
// 模拟 3 个客户端走完整帧同步流程，记录各阶段耗时。
// ============================================================

#include "game/game_room.h"
#include "game/room_manager.h"
#include "game/frame_sync.h"
#include "game/input_buffer.h"
#include "game/game_state.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

// ============================================================
// 计时辅助
// ============================================================

struct Timer {
    using Clock = std::chrono::high_resolution_clock;
    using Ms = std::chrono::duration<double, std::milli>;

    Clock::time_point start = Clock::now();

    double ElapsedMs() const {
        return Ms(Clock::now() - start).count();
    }

    void Reset() {
        start = Clock::now();
    }
};

// ============================================================
// 模拟客户端
// ============================================================

struct SimClient {
    std::string player_id;
    uint8_t move_dir; // MoveDir 编码
    uint32_t last_frame = 0; // 客户端本地帧号
};

// ============================================================
// 主测试
// ============================================================

int main() {
    setbuf(stdout, NULL);
    printf("=== 帧同步全流程模拟测试 ===\n\n");

    const int kPlayerCount = 3;
    const int kTotalFrames = 60; // 60 帧 ≈ 3 秒 @20fps
    const int kFps = 20;

    // ---- 配置模拟客户端 ----
    std::vector<SimClient> clients;
    clients.push_back({"player_a", 0x04}); // 一直向右
    clients.push_back({"player_b", 0x02}); // 一直向下
    clients.push_back({"player_c", 0x01}); // 一直向上

    // ============================================================
    // Phase 1: 匹配/房间创建
    // ============================================================
    Timer phase1;

    game::RoomManager room_mgr;
    game::GameRoom::Config cfg;
    cfg.max_players = 4;

    // 创建房间
    auto cr = room_mgr.CreateRoom("player_a", cfg);
    if (!cr.ok) {
        printf("[FAIL] CreateRoom\n");
        return 1;
    }
    std::string room_id = cr.room_id;
    auto* room = room_mgr.GetRoom(room_id);
    room->SetState(game::ROOM_STATE_WAITING);

    // 其他玩家加入
    for (int i = 1; i < kPlayerCount; i++) {
        if (!room_mgr.JoinRoom(room_id, clients[i].player_id).ok) {
            printf("[FAIL] JoinRoom %s\n", clients[i].player_id.c_str());
            return 1;
        }
    }

    // 开始游戏
    if (!room_mgr.StartGame(room_id, "player_a").ok) {
        printf("[FAIL] StartGame\n");
        return 1;
    }

    double create_join_ms = phase1.ElapsedMs();

    // ============================================================
    // Phase 2: 帧同步初始化
    // ============================================================
    Timer phase2;

    room->InitFrameSync(kFps, 120, 60);
    if (!room->HasFrameSync()) {
        printf("[FAIL] InitFrameSync\n");
        return 1;
    }

    auto* fsm = room->GetFrameSync();

    double init_ms = phase2.ElapsedMs();

    // ============================================================
    // Phase 3: 帧同步运行 — 60 帧
    // ============================================================
    Timer phase3;

    double max_tick_us = 0;
    double total_tick_us = 0;
    double max_input_us = 0;
    double total_input_us = 0;

    for (int f = 1; f <= kTotalFrames; f++) {
        // ---- 输入阶段 ----
        Timer input_timer;
        for (auto& c : clients) {
            room->OnPlayerFrameInput(static_cast<uint32_t>(f), c.player_id, {c.move_dir});
        }
        double input_us = input_timer.ElapsedMs() * 1000.0; // ms → μs

        // ---- Tick 阶段 ----
        Timer tick_timer;
        size_t n = fsm->Tick();
        double tick_us = tick_timer.ElapsedMs() * 1000.0;

        if (n != static_cast<size_t>(kPlayerCount)) {
            printf("[FAIL] f=%d input=%zu\n", f, n);
            return 1;
        }

        // 统计
        if (input_us > max_input_us)
            max_input_us = input_us;
        if (tick_us > max_tick_us)
            max_tick_us = tick_us;
        total_input_us += input_us;
        total_tick_us += tick_us;
    }

    double sync_time_ms = phase3.ElapsedMs();

    // ============================================================
    // Phase 4: 游戏结束
    // ============================================================
    Timer phase4;

    room->StopFrameSync();
    room->SetState(game::ROOM_STATE_FINISHED);

    double stop_ms = phase4.ElapsedMs();

    // ============================================================
    // 输出报告
    // ============================================================

    printf("========== 全流程耗时报告 ==========\n\n");

    printf("┌─────────────────────────────┬──────────┐\n");
    printf("│ 阶段                           │ 耗时        │\n");
    printf("├─────────────────────────────┼──────────┤\n");
    printf("│ 1. 匹配/创建/加入/开始        │ %7.3f ms │\n", create_join_ms);
    printf("│ 2. 帧同步初始化               │ %7.3f ms │\n", init_ms);
    printf("│ 3. 帧同步运行 (%d帧)           │ %7.3f ms │\n", kTotalFrames, sync_time_ms);
    printf("│ 4. 停止游戏                   │ %7.3f ms │\n", stop_ms);
    printf("├─────────────────────────────┼──────────┤\n");
    printf("│ 总耗时                       │ %7.3f ms │\n",
           create_join_ms + init_ms + sync_time_ms + stop_ms);
    printf("└─────────────────────────────┴──────────┘\n\n");

    printf("========== 帧同步详细指标 ==========\n\n");

    printf("  帧率: %d fps (间隔 %d ms)\n", kFps, 1000 / kFps);
    printf("  总帧数: %d\n", kTotalFrames);
    printf("  玩家数: %d\n", kPlayerCount);
    printf("\n");
    printf("  Tick 耗时:\n");
    printf("    总计: %.1f μs (%.1f ms)\n", total_tick_us, total_tick_us / 1000.0);
    printf("    平均: %.1f μs/帧\n", total_tick_us / kTotalFrames);
    printf("    最大: %.1f μs\n", max_tick_us);
    printf("\n");
    printf("  输入收集耗时:\n");
    printf("    总计: %.1f μs (%.1f ms)\n", total_input_us, total_input_us / 1000.0);
    printf("    平均: %.1f μs/帧 (%d玩家)\n", total_input_us / kTotalFrames, kPlayerCount);
    printf("    最大: %.1f μs\n", max_input_us);
    printf("\n");
    printf("  帧同步运行总时间: %.3f ms\n", sync_time_ms);
    printf("  理论最小时间: %.1f ms (%d帧 × %dms)\n", kTotalFrames * (1000.0 / kFps), kTotalFrames,
           1000 / kFps);
    printf("  实际/理论比: %.1fx (%.3f ms / %.1f ms)\n",
           sync_time_ms / (kTotalFrames * (1000.0 / kFps)), sync_time_ms,
           kTotalFrames * (1000.0 / kFps));

    printf("\n========================================\n");

    return 0;
}
