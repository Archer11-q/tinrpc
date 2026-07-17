#include "game/game_state.h"

#include <algorithm>

namespace game {

MoveDir ParseMoveDir(const std::vector<uint8_t>& input) {
    if (input.empty()) return MoveDir::NONE;
    uint8_t val = input[0];
    if (val >= 1 && val <= 4) return static_cast<MoveDir>(val);
    return MoveDir::NONE;
}

void ApplyMove(PlayerPos& pos, MoveDir dir, int32_t step) {
    switch (dir) {
        case MoveDir::UP:    pos.y -= step; break;
        case MoveDir::DOWN:  pos.y += step; break;
        case MoveDir::LEFT:  pos.x -= step; break;
        case MoveDir::RIGHT: pos.x += step; break;
        case MoveDir::NONE:  break;
    }
}

void EnsurePlayerExists(GameState& state, const std::string& player_id) {
    for (auto& p : state.players) {
        if (p.player_id == player_id) return;
    }
    state.players.push_back({player_id, 0, 0});
}

GameState tickLogic(
    const std::unordered_map<std::string, std::vector<uint8_t>>& inputs,
    const GameState& current_state) {

    // 1. 复制当前状态，帧号 +1
    GameState next = current_state;
    next.frame_no = current_state.frame_no + 1;

    // 2. 确保所有有输入的玩家都在状态中（新玩家初始化为原点）
    for (const auto& [player_id, input] : inputs) {
        EnsurePlayerExists(next, player_id);
    }

    // 3. 按 player_id 字典序排序，保证处理顺序确定性
    std::sort(next.players.begin(), next.players.end(),
              [](const PlayerPos& a, const PlayerPos& b) {
                  return a.player_id < b.player_id;
              });

    // 4. 对每个玩家应用输入
    for (auto& p : next.players) {
        auto it = inputs.find(p.player_id);
        if (it == inputs.end()) continue;  // 该玩家本帧无输入

        MoveDir dir = ParseMoveDir(it->second);
        ApplyMove(p, dir, 1);  // 每帧移动 1 格
    }

    return next;
}

} // namespace game
