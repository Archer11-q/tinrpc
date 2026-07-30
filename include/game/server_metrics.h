#pragma once
/**
 * @brief server_metrics.h — 服务端运行指标采集器
 *
 * 用法：
 *   ServerMetrics metrics;
 *   metrics.OnRequest(latency_us);        // 每次 RPC 调用完成后
 *   metrics.OnConnect();                  // 新连接建立
 *   metrics.OnDisconnect();               // 连接断开
 *   metrics.OnError();                    // 错误发生
 *   metrics.Tick();                       // 定期推进滑动窗口
 *   auto res = metrics.GetSnapshot(...);  // 查询当前指标
 *
 * @note 所有方法在 EventLoop IO 线程调用，单线程无锁。
 */

#include "rpc/bench_stats.h"

#include <cstdint>
#include <chrono>

namespace game {

class ServerMetrics {
public:
    ServerMetrics()
        : start_time_(std::chrono::steady_clock::now())
        , qps_counter_(5) // 5秒滑动窗口
    {
    }

    // ---- 事件上报 ----

    /// @brief 记录单次 RPC 请求的延迟
    /// @param latency_us 延迟（微秒）
    void OnRequest(double latency_us) {
        total_requests_++;
        latency_hist_.Record(latency_us);
        qps_counter_.RecordRequest();
    }

    /// @brief 记录新连接建立
    void OnConnect() {
        active_connections_++;
    }
    /// @brief 记录连接断开
    void OnDisconnect() {
        active_connections_--;
    }
    /// @brief 记录错误发生
    void OnError() {
        error_count_++;
    }

    // ---- 定期维护（在 EventLoop 中每次循环调用）----

    /// @brief 定期维护（在 EventLoop 中每次循环调用，推进滑动窗口）
    void Tick() {
        qps_counter_.Tick();
    }

    // ---- 查询 ----

    /// @brief 指标快照（用于查询当前运行指标）
    struct Snapshot {
        int64_t uptime_sec; ///< 运行时长（秒）
        int32_t active_connections; ///< 活跃连接数
        int32_t total_rooms; ///< 房间总数
        int64_t total_requests; ///< 总请求数
        double current_qps; ///< 当前 QPS（滑动窗口）
        double avg_latency_us; ///< 平均延迟（微秒）
        double p50_latency_us; ///< P50 延迟（微秒）
        double p99_latency_us; ///< P99 延迟（微秒）
        int32_t match_queue_size; ///< 匹配队列长度
        int64_t error_count; ///< 错误总数
    };

    /** @brief 获取当前指标快照
     *  @param total_rooms       当前房间总数
     *  @param match_queue_size  当前匹配队列长度
     *  @return 指标快照
     */
    Snapshot GetSnapshot(int total_rooms, int match_queue_size) const {
        auto p = latency_hist_.Compute();
        int64_t uptime = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::steady_clock::now() - start_time_)
                             .count();

        return Snapshot{
            uptime,
            active_connections_,
            total_rooms,
            total_requests_,
            qps_counter_.CurrentQps(),
            p.avg,
            p.p50,
            p.p99,
            match_queue_size,
            error_count_,
        };
    }

    // ---- 重置（可选，用于压测前清零）----

    /// @brief 重置全部指标（可选，用于压测前清零）
    void Reset() {
        total_requests_ = 0;
        error_count_ = 0;
        active_connections_ = 0;
        latency_hist_.Reset();
        qps_counter_.Reset();
        start_time_ = std::chrono::steady_clock::now();
    }

private:
    std::chrono::steady_clock::time_point start_time_;
    int32_t active_connections_ = 0;
    int64_t total_requests_ = 0;
    int64_t error_count_ = 0;

    rpc::QpsCounter qps_counter_;
    rpc::LatencyHistogram latency_hist_; // mutable 支持 const 查询
};

} // namespace game
