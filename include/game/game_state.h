#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>

namespace game {

// ============================================================
// 简单游戏状态定义
//
// 用于帧同步的确定性 tick 验证——相同输入永远产生相同状态。
// ============================================================

// 移动方向（1 字节输入）
enum class MoveDir : uint8_t {
    NONE = 0x00, // 不动
    UP = 0x01, // ↑
    DOWN = 0x02, // ↓
    LEFT = 0x03, // ←
    RIGHT = 0x04, // →
};

// 玩家坐标
struct PlayerPos {
    std::string player_id;
    int32_t x = 0;
    int32_t y = 0;
};

// 游戏状态
struct GameState {
    uint32_t frame_no = 0; // 当前帧号
    std::vector<PlayerPos> players; // 所有玩家坐标（有序，保证确定性）
};

/**
 * @brief tickLogic — 确定性状态更新函数
 *
 * 输入：当前帧所有玩家的操作 (player_id → input_data)
 *       currentState（上一帧状态）
 * 输出：新状态（frame_no + 1，玩家坐标根据输入更新）
 *
 * 确定性保证：
 * - 玩家按 player_id 字典序处理（消除 map 遍历顺序不确定性）
 * - 输入数据第一个字节为 MoveDir，其余保留
 * - 无随机数、无外部依赖
 *
 * @param inputs         当前帧所有玩家的输入
 * @param current_state  上一帧状态
 * @return 新状态（frame_no + 1，玩家坐标根据输入更新）
 */
GameState tickLogic(const std::unordered_map<std::string, std::vector<uint8_t>>& inputs,
                    const GameState& current_state);

/** @brief 从 bytes 解析 MoveDir（默认 NONE）
 *  @param input 序列化后的输入数据
 *  @return 解析出的移动方向
 */
MoveDir ParseMoveDir(const std::vector<uint8_t>& input);

/** @brief 按 MoveDir 更新坐标
 *  @param pos  要更新的玩家坐标
 *  @param dir  移动方向
 *  @param step 步长，默认 1
 */
void ApplyMove(PlayerPos& pos, MoveDir dir, int32_t step = 1);

/** @brief 确保玩家存在于状态中（新玩家加入时初始化为 (0,0)）
 *  @param state     游戏状态
 *  @param player_id 玩家 ID
 */
void EnsurePlayerExists(GameState& state, const std::string& player_id);

// ============================================================
// 位置纠错数据（用于服务端权威 → 客户端和解）
// ============================================================

/// @brief 单玩家位置偏差（用于服务端权威 → 客户端和解）
struct PlayerCorrection {
    std::string player_id;
    int32_t server_x = 0, server_y = 0; ///< 服务端权威坐标
    int32_t client_x = 0, client_y = 0; ///< 客户端预测坐标
    int32_t delta_x = 0, delta_y = 0; ///< 偏差量（server - client）
};

/** @brief 计算服务端权威状态与客户端预测状态之间的偏差
 *  @param authoritative 服务端权威状态
 *  @param predicted     客户端预测状态
 *  @return 有偏差的玩家列表（位置完全一致的玩家不返回）
 */
std::vector<PlayerCorrection> CompareStates(const GameState& authoritative,
                                            const GameState& predicted);

/** @brief 将客户端状态向服务端权威状态插值一步（用于平滑和解）
 *  @param client_state  客户端状态（会被修改）
 *  @param authoritative 服务端权威状态
 *  @param alpha         插值系数（0=保持预测, 1=跳到权威, 建议 0.2~0.5）
 */
void ReconcileState(GameState& client_state, const GameState& authoritative, float alpha = 0.3f);

} // namespace game
