#pragma once
// ============================================================
// server_metrics.h — 服务端运行指标采集器
//
// 线程模型：所有方法在 EventLoop IO 线程调用，单线程无锁。
//
// 用法：
//   ServerMetrics metrics;
//   metrics.OnRequest(latency_us);        // 每次 RPC 调用完成后
//   metrics.OnConnect();                  // 新连接建立
//   metrics.OnDisconnect();               // 连接断开
//   metrics.OnError();                    // 错误发生
//   metrics.Tick();                       // 定期推进滑动窗口
//   auto res = metrics.Snapshot();        // 查询当前指标
// ============================================================

#include "rpc/bench_stats.h"

#include <cstdint>
#include <chrono>

namespace game {

class ServerMetrics {
public:
    ServerMetrics()
        : start_time_(std::chrono::steady_clock::now())
        , qps_counter_(5)  // 5秒滑动窗口
    {}

    // ---- 事件上报 ----

    void OnRequest(double latency_us) {
        total_requests_++;
        latency_hist_.Record(latency_us);
        qps_counter_.RecordRequest();
    }

    void OnConnect()   { active_connections_++;   }
    void OnDisconnect(){ active_connections_--;   }
    void OnError()     { error_count_++;          }

    // ---- 定期维护（在 EventLoop 中每次循环调用）----

    void Tick() {
        qps_counter_.Tick();
    }

    // ---- 查询 ----

    struct Snapshot {
        int64_t  uptime_sec;
        int32_t  active_connections;
        int32_t  total_rooms;
        int64_t  total_requests;
        double   current_qps;
        double   avg_latency_us;
        double   p50_latency_us;
        double   p99_latency_us;
        int32_t  match_queue_size;
        int64_t  error_count;
    };

    Snapshot GetSnapshot(int total_rooms, int match_queue_size) const {
        auto p = latency_hist_.Compute();
        int64_t uptime = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_time_).count();

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
    int32_t  active_connections_ = 0;
    int64_t  total_requests_     = 0;
    int64_t  error_count_        = 0;

    rpc::QpsCounter       qps_counter_;
    rpc::LatencyHistogram  latency_hist_;  // mutable 支持 const 查询
};

} // namespace game
