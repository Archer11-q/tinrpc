#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <cstddef>

namespace game {

/**
 * @brief InputBuffer — 帧同步输入缓冲（Jitter Buffer）
 *
 * 职责：
 * - 缓存最近 N 帧的玩家输入，应对网络抖动导致的乱序到达
 * - 服务端按帧号收集所有玩家的输入，转发给帧同步逻辑
 * - 自动淘汰过期帧，控制内存占用
 *
 * 内部用 std::deque 按帧号升序存储，AddInput 时二分查找插入位置。
 *
 * @note 所有方法必须在 EventLoop IO 线程调用，无锁。
 */
class InputBuffer {
public:
    /** @brief 构造函数
     *  @param max_frames 最大缓冲帧数，默认 60 帧（局域网约 1 秒 @60fps）
     */
    explicit InputBuffer(size_t max_frames = 60);

    /** @brief 添加玩家在指定帧的输入
     *  @param frame_no  帧号（从 1 开始递增）
     *  @param player_id 玩家 ID
     *  @param input     序列化后的输入数据（由上层协议定义格式）
     */
    void AddInput(uint32_t frame_no, const std::string& player_id, const std::vector<uint8_t>& input);

    /** @brief 获取指定帧的所有玩家输入（消费后移除该帧）
     *  @param frame_no 帧号
     *  @return player_id → input_data 映射，帧不存在或已过期返回空 map
     */
    std::unordered_map<std::string, std::vector<uint8_t>> GetInput(uint32_t frame_no);

    /** @brief 清理指定帧之前（不含）的所有输入
     *  @param frame_no 保留此帧及之后的所有输入
     */
    void ClearUpTo(uint32_t frame_no);

    /// @brief 清空全部缓冲
    void Clear();

    // 查询
    size_t FrameCount() const {
        return buffer_.size();
    } ///< 当前缓冲帧数
    size_t MaxFrames() const {
        return max_frames_;
    } ///< 最大缓冲帧数
    bool IsEmpty() const {
        return buffer_.empty();
    } ///< 是否为空

private:
    /// @brief 单帧输入结构：帧号 + 该帧所有玩家的输入
    struct FrameInput {
        uint32_t frame_no;
        std::unordered_map<std::string, std::vector<uint8_t>> players;
    };

    std::deque<FrameInput> buffer_;
    size_t max_frames_;

    /** @brief 在 deque 中二分查找 frame_no
     *  @param frame_no 帧号
     *  @return 迭代器，未找到返回 end()
     *  @note deque 按 frame_no 升序排列
     */
    auto FindFrame(uint32_t frame_no) -> decltype(buffer_.begin());
};

} // namespace game
