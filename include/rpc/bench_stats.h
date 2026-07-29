#pragma once
// ============================================================
// bench_stats.h — 压测统计工具（可复用于客户端和服务端）
//
// 提供：
// - LatencyHistogram: 延迟直方图 + 分位数计算
// - QpsCounter:       滑动窗口 QPS 计数器
// - StageTimer:       分阶段耗时记录器
// - ErrorCounter:     错误分类计数
// - BenchReport:      汇总报告输出（终端 + Markdown 表格）
// ============================================================

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <atomic>
#include <unordered_map>

namespace rpc {

// ============================================================
// LatencyHistogram — 延迟直方图
//
// 线程安全。收集所有延迟样本，计算分位数。
// ============================================================
class LatencyHistogram {
public:
    LatencyHistogram() { samples_.reserve(1'000'000); }

    // 记录一个延迟样本（微秒）
    void Record(double latency_us) {
        std::lock_guard<std::mutex> lock(mutex_);
        samples_.push_back(latency_us);
        sorted_ = false;
    }

    // 批量记录
    void RecordBatch(const std::vector<double>& latencies_us) {
        std::lock_guard<std::mutex> lock(mutex_);
        samples_.insert(samples_.end(), latencies_us.begin(), latencies_us.end());
        sorted_ = false;
    }

    size_t Count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return samples_.size();
    }

    double Min() const {
        ensure_sorted();
        return samples_.empty() ? 0.0 : samples_.front();
    }

    double Max() const {
        ensure_sorted();
        return samples_.empty() ? 0.0 : samples_.back();
    }

    double Avg() const {
        ensure_sorted();
        if (samples_.empty()) return 0.0;
        double sum = 0.0;
        for (double v : samples_) sum += v;
        return sum / static_cast<double>(samples_.size());
    }

    double Percentile(double p) const {
        // p 取值 0.0 ~ 1.0，例如 p=0.50 即 P50
        ensure_sorted();
        if (samples_.empty()) return 0.0;
        size_t idx = static_cast<size_t>(std::ceil(p * static_cast<double>(samples_.size()))) - 1;
        if (idx >= samples_.size()) idx = samples_.size() - 1;
        return samples_[idx];
    }

    // 一次性获取常用分位数
    struct Percentiles {
        double avg, min, max, p50, p95, p99;
    };
    Percentiles Compute() const {
        return {Avg(), Min(), Max(), Percentile(0.50), Percentile(0.95), Percentile(0.99)};
    }

    void Reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        samples_.clear();
        sorted_ = false;
    }

private:
    void ensure_sorted() const {
        if (sorted_) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (sorted_) return;
        std::sort(samples_.begin(), samples_.end());
        sorted_ = true;
    }

    mutable std::mutex mutex_;
    mutable std::vector<double> samples_;
    mutable bool sorted_ = false;
};

// ============================================================
// QpsCounter — 滑动窗口 QPS 计数器
//
// 线程安全。定期调用 Tick() 推进窗口。
// ============================================================
class QpsCounter {
public:
    explicit QpsCounter(size_t window_sec = 5)
        : window_sec_(window_sec)
        , buckets_(window_sec * 10, 0)  // 每 100ms 一个桶
    {
        last_tick_ = Clock::now();
    }

    // 每次请求完成时调用
    void RecordRequest() {
        requests_since_tick_.fetch_add(1);
    }

    // 定期调用（建议每 100ms 或每次 epoll 循环），推进窗口
    void Tick() {
        auto now = Clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_tick_).count();

        if (elapsed_ms < 100) return;  // 还没到下一个桶

        last_tick_ = now;

        // 推进多少个 100ms 桶
        int steps = static_cast<int>(elapsed_ms / 100);
        if (steps > static_cast<int>(buckets_.size())) steps = static_cast<int>(buckets_.size());

        std::lock_guard<std::mutex> lock(mutex_);
        for (int i = 0; i < steps; i++) {
            bucket_idx_ = (bucket_idx_ + 1) % buckets_.size();
            total_requests_ += buckets_[bucket_idx_];
            buckets_[bucket_idx_] = 0;
        }
        // 填入当前累计
        buckets_[bucket_idx_] += requests_since_tick_.exchange(0);
    }

    // 当前 QPS（最近 window_sec 秒的平均值）
    double CurrentQps() const {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t window_requests = 0;
        for (size_t count : buckets_) window_requests += count;
        return static_cast<double>(window_requests) / static_cast<double>(window_sec_);
    }

    // 总请求数
    uint64_t TotalRequests() const { return total_requests_.load(); }

    void Reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::fill(buckets_.begin(), buckets_.end(), 0);
        total_requests_ = 0;
        requests_since_tick_ = 0;
    }

private:
    using Clock = std::chrono::steady_clock;
    size_t window_sec_;
    std::vector<size_t> buckets_;
    size_t bucket_idx_ = 0;
    Clock::time_point last_tick_;

    mutable std::mutex mutex_;
    std::atomic<uint64_t> total_requests_{0};
    std::atomic<uint64_t> requests_since_tick_{0};
};

// ============================================================
// ErrorCounter — 错误分类计数
//
// 线程安全。
// ============================================================
class ErrorCounter {
public:
    void Record(const std::string& category) {
        std::lock_guard<std::mutex> lock(mutex_);
        counts_[category]++;
    }

    int Get(const std::string& category) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = counts_.find(category);
        return it != counts_.end() ? it->second : 0;
    }

    int Total() const {
        std::lock_guard<std::mutex> lock(mutex_);
        int total = 0;
        for (auto& [k, v] : counts_) total += v;
        return total;
    }

    // 遍历所有错误类别
    void ForEach(std::function<void(const std::string&, int)> fn) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [k, v] : counts_) fn(k, v);
    }

    void Reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        counts_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, int> counts_;
};

// ============================================================
// StageTimer — 分阶段耗时记录器
//
// 记录"连接→登录→匹配→进房→发消息→离开"每个阶段的耗时。
// ============================================================
class StageTimer {
public:
    struct StageResult {
        std::string name;
        double avg_us;
        double min_us;
        double max_us;
        double p50_us;
        int    success_count;
        int    fail_count;
    };

    // 记录一个阶段的耗时
    void Record(const std::string& stage_name, double latency_us, bool success) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& h = histograms_[stage_name];
        h.Record(latency_us);
        if (success)
            success_counts_[stage_name]++;
        else
            fail_counts_[stage_name]++;
    }

    // 获取所有阶段的结果
    std::vector<StageResult> Results() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<StageResult> results;
        for (auto& [name, hist] : histograms_) {
            StageResult r;
            r.name = name;
            auto p = hist.Compute();
            r.avg_us = p.avg;
            r.min_us = p.min;
            r.max_us = p.max;
            r.p50_us = p.p50;
            r.success_count = success_counts_.count(name) ? success_counts_.at(name) : 0;
            r.fail_count = fail_counts_.count(name) ? fail_counts_.at(name) : 0;
            results.push_back(r);
        }
        return results;
    }

    void Reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        histograms_.clear();
        success_counts_.clear();
        fail_counts_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, LatencyHistogram> histograms_;
    std::unordered_map<std::string, int> success_counts_;
    std::unordered_map<std::string, int> fail_counts_;
};

// ============================================================
// BenchReport — 汇总报告输出
//
// 提供终端格式化输出和 Markdown 表格生成。
// ============================================================
class BenchReport {
public:
    // 打印延迟分位数表格（终端）
    static void PrintLatencyTable(const LatencyHistogram::Percentiles& p,
                                   const char* label = "延迟") {
        printf("  %s (us): avg=%.1f  min=%.1f  max=%.1f  p50=%.1f  p95=%.1f  p99=%.1f\n",
               label, p.avg, p.min, p.max, p.p50, p.p95, p.p99);
    }

    // 打印阶段耗时表格
    static void PrintStageTable(const std::vector<StageTimer::StageResult>& stages) {
        printf("\n┌──────────────────────────┬──────────┬──────────┬──────────┬──────────┬──────────┬──────────┐\n");
        printf("│ 阶段                     │   成功率  │  avg(us) │  min(us) │  max(us) │  p50(us) │   样本数  │\n");
        printf("├──────────────────────────┼──────────┼──────────┼──────────┼──────────┼──────────┼──────────┤\n");
        for (auto& s : stages) {
            int total = s.success_count + s.fail_count;
            double rate = total > 0 ? 100.0 * s.success_count / total : 0;
            printf("│ %-24s │ %5.1f%% │ %8.1f │ %8.1f │ %8.1f │ %8.1f │ %8d │\n",
                   s.name.c_str(), rate, s.avg_us, s.min_us, s.max_us, s.p50_us, total);
        }
        printf("└──────────────────────────┴──────────┴──────────┴──────────┴──────────┴──────────┴──────────┘\n");
    }

    // 输出 Markdown 表格行（方便粘贴到报告）
    static void PrintMarkdownRow(const char* scenario, int conn, int duration_sec,
                                  double qps, const LatencyHistogram::Percentiles& p) {
        printf("| %s | %d | %ds | %.0f | %.1f | %.1f | %.1f | %.1f |\n",
               scenario, conn, duration_sec, qps, p.avg, p.p50, p.p95, p.p99);
    }

    // 打印完整的综合报告
    static void PrintFullReport(const char* scenario, int conn_count,
                                 double elapsed_sec, double qps,
                                 const LatencyHistogram::Percentiles& p,
                                 const ErrorCounter& errors,
                                 const std::vector<StageTimer::StageResult>& stages) {
        printf("\n");
        printf("========================================\n");
        printf("  压测报告: %s\n", scenario);
        printf("========================================\n");
        printf("  并发连接数:   %d\n", conn_count);
        printf("  总耗时:       %.3f 秒\n", elapsed_sec);
        printf("  总请求数:     %zu\n", p.avg > 0 ? static_cast<size_t>(qps * elapsed_sec) : 0);
        printf("  QPS:          %.1f\n", qps);
        printf("  错误总数:     %d\n", errors.Total());
        printf("----------------------------------------\n");

        PrintLatencyTable(p);

        // 错误分类
        if (errors.Total() > 0) {
            printf("\n  错误分类:\n");
            errors.ForEach([](const std::string& cat, int count) {
                printf("    %s: %d\n", cat.c_str(), count);
            });
        }

        // 阶段耗时
        if (!stages.empty()) {
            PrintStageTable(stages);
        }

        printf("========================================\n");
    }
};

} // namespace rpc
