// ============================================================
// bench_client — 多线程压测客户端
//
// 用法：
//   ./bench_client --mode rpc  --port 8080 --threads 8 --requests 10000
//   ./bench_client --mode http --port 8080 --threads 8 --requests 10000
//
// 每个线程独立建立连接，发送请求，记录每条延迟。
// 所有线程完成后汇总统计 avg / p50 / p95 / p99 / QPS。
// ============================================================

#include "rpc/rpc_client.h"
#include "rpc/serializer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <cmath>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

// ============================================================
// 命令行参数
// ============================================================
static const char* g_mode = "rpc";
static uint16_t g_port = 8080;
static int g_threads = 1;
static int g_requests = 10000; // 每线程请求数
static int g_warmup = 1000; // 每线程预热请求数
static int g_timeout_s = 5; // 单请求超时秒数

// ============================================================
// Benchmark 统计结果
// ============================================================
struct BenchStats {
    int total_requests = 0;
    int failed_requests = 0;
    double total_time_sec = 0.0; // 墙上时钟总耗时
    double avg_us = 0.0;
    double min_us = 0.0;
    double max_us = 0.0;
    double p50_us = 0.0;
    double p95_us = 0.0;
    double p99_us = 0.0;
    double qps = 0.0;
};

// ============================================================
// HTTP 请求/响应辅助
// ============================================================

// 从 HTTP 响应头中提取 Content-Length
static int HttpResponseContentLength(const std::string& header) {
    auto pos = header.find("Content-Length:");
    if (pos == std::string::npos)
        pos = header.find("content-length:");
    if (pos == std::string::npos)
        return -1;
    pos += 15;
    while (pos < header.size() && header[pos] == ' ')
        pos++;
    size_t end = pos;
    while (end < header.size() && header[end] >= '0' && header[end] <= '9')
        end++;
    if (end == pos)
        return -1;
    return std::stoi(header.substr(pos, end - pos));
}

// 发送一个 HTTP 请求并接收完整响应，返回 true 表示成功
static bool HttpSendRecv(int fd, const std::string& request, char* recv_buf, size_t buf_size) {
    // 发送请求
    ssize_t sent = send(fd, request.data(), request.size(), MSG_NOSIGNAL);
    if (sent < 0)
        return false;

    // 接收响应 — 读头部直到 \r\n\r\n
    std::string response;
    size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        ssize_t n = recv(fd, recv_buf, buf_size, 0);
        if (n <= 0)
            return false;
        response.append(recv_buf, static_cast<size_t>(n));
        header_end = response.find("\r\n\r\n");
    }

    // 提取 Content-Length
    int body_len = HttpResponseContentLength(response);
    if (body_len < 0)
        return false;

    // 确保 body 完整接收
    size_t needed = header_end + 4 + static_cast<size_t>(body_len);
    while (response.size() < needed) {
        ssize_t n = recv(fd, recv_buf, buf_size, 0);
        if (n <= 0)
            return false;
        response.append(recv_buf, static_cast<size_t>(n));
    }
    return true;
}

// 构造 HTTP POST 请求
static std::string BuildHttpRequest(const std::string& method, const std::string& json_body) {
    std::string req;
    req += "POST /rpc/" + method + " HTTP/1.1\r\n";
    req += "Host: 127.0.0.1:" + std::to_string(g_port) + "\r\n";
    req += "Content-Type: application/json\r\n";
    req += "Content-Length: " + std::to_string(json_body.size()) + "\r\n";
    req += "Connection: keep-alive\r\n";
    req += "\r\n";
    req += json_body;
    return req;
}

// ============================================================
// RPC 模式：单线程压测
// ============================================================
static void RunRpcThread(int thread_id, int num_warmup, int num_requests,
                         std::vector<double>& out_latencies_us, int& out_failed) {
    rpc::RpcClient client;
    if (!client.Connect("127.0.0.1", g_port)) {
        fprintf(stderr, "[线程 %d] 连接失败\n", thread_id);
        out_failed = num_requests;
        return;
    }

    // 构造请求 body（固定：Add(3, 5)）
    rpc::Serializer ser;
    ser.WriteInt32(3);
    ser.WriteInt32(5);
    auto body = ser.GetBuffer();

    // ---- 预热阶段 ----
    for (int i = 0; i < num_warmup; i++) {
        auto future = client.Call("Add", body);
        future.wait_for(std::chrono::seconds(g_timeout_s));
    }

    // ---- 正式测量 ----
    out_latencies_us.reserve(static_cast<size_t>(num_requests));

    for (int i = 0; i < num_requests; i++) {
        auto t0 = std::chrono::steady_clock::now();

        auto future = client.Call("Add", body);
        auto status = future.wait_for(std::chrono::seconds(g_timeout_s));

        auto t1 = std::chrono::steady_clock::now();

        if (status != std::future_status::ready) {
            out_failed++;
            continue;
        }

        auto rsp = future.get();
        if (rsp.empty()) {
            out_failed++;
            continue;
        }

        double us = static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
        out_latencies_us.push_back(us);
    }

    client.Disconnect();
}

// ============================================================
// HTTP 模式：单线程压测
// ============================================================
static void RunHttpThread(int thread_id, int num_warmup, int num_requests,
                          std::vector<double>& out_latencies_us, int& out_failed) {
    // 建立 TCP 连接
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "[线程 %d] socket() 失败\n", thread_id);
        out_failed = num_requests;
        return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(g_port);
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) <= 0) {
        fprintf(stderr, "[线程 %d] inet_pton() 失败\n", thread_id);
        close(fd);
        out_failed = num_requests;
        return;
    }

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        fprintf(stderr, "[线程 %d] connect() 失败\n", thread_id);
        close(fd);
        out_failed = num_requests;
        return;
    }

    std::string request = BuildHttpRequest("Add", "{\"a\":3,\"b\":5}");
    char recv_buf[8192];

    // ---- 预热阶段 ----
    for (int i = 0; i < num_warmup; i++) {
        HttpSendRecv(fd, request, recv_buf, sizeof(recv_buf));
    }

    // ---- 正式测量 ----
    out_latencies_us.reserve(static_cast<size_t>(num_requests));

    for (int i = 0; i < num_requests; i++) {
        auto t0 = std::chrono::steady_clock::now();

        if (!HttpSendRecv(fd, request, recv_buf, sizeof(recv_buf))) {
            out_failed++;
            continue;
        }

        auto t1 = std::chrono::steady_clock::now();
        double us = static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
        out_latencies_us.push_back(us);
    }

    close(fd);
}

// ============================================================
// 统计计算
// ============================================================
static BenchStats ComputeStats(const std::vector<double>& all_latencies, int total, int failed,
                               double elapsed_sec) {
    BenchStats s;
    s.total_requests = total;
    s.failed_requests = failed;

    if (all_latencies.empty())
        return s;

    // 排序以计算分位数
    auto sorted = all_latencies;
    std::sort(sorted.begin(), sorted.end());

    size_t n = sorted.size();
    s.min_us = sorted.front();
    s.max_us = sorted.back();
    s.avg_us = 0.0;
    for (double v : sorted)
        s.avg_us += v;
    s.avg_us /= static_cast<double>(n);

    // 分位数索引（使用最近秩方法）
    auto percentile = [&](double p) -> double {
        size_t idx = static_cast<size_t>(std::ceil(p * static_cast<double>(n))) - 1;
        if (idx >= n)
            idx = n - 1;
        return sorted[idx];
    };

    s.p50_us = percentile(0.50);
    s.p95_us = percentile(0.95);
    s.p99_us = percentile(0.99);

    s.total_time_sec = elapsed_sec;
    if (elapsed_sec > 0.0) {
        s.qps = static_cast<double>(s.total_requests - s.failed_requests) / elapsed_sec;
    }

    return s;
}

// ============================================================
// 打印报告
// ============================================================
static void PrintReport(const BenchStats& s) {
    printf("\n");
    printf("========================================\n");
    printf("         Benchmark 报告\n");
    printf("========================================\n");
    printf("  模式:         %s\n", g_mode);
    printf("  端口:         %u\n", g_port);
    printf("  并发线程:     %d\n", g_threads);
    printf("  每线程请求:   %d\n", g_requests);
    printf("  总请求数:     %d\n", s.total_requests);
    printf("  成功:         %d\n", s.total_requests - s.failed_requests);
    printf("  失败:         %d\n", s.failed_requests);
    printf("  总耗时:       %.3f 秒\n", s.total_time_sec);
    printf("----------------------------------------\n");
    printf("  QPS:          %.0f\n", s.qps);
    printf("----------------------------------------\n");
    printf("  延迟 (us):\n");
    printf("    avg:        %.1f\n", s.avg_us);
    printf("    min:        %.1f\n", s.min_us);
    printf("    max:        %.1f\n", s.max_us);
    printf("    p50:        %.1f\n", s.p50_us);
    printf("    p95:        %.1f\n", s.p95_us);
    printf("    p99:        %.1f\n", s.p99_us);
    printf("========================================\n");

    // 输出 Markdown 表格行（方便直接粘贴到报告）
    printf("\n--- Markdown 表格行 ---\n");
    printf("| %s | %d | %d | %.0f | %.1f | %.1f | %.1f | %.1f |\n", g_mode, g_threads, g_requests,
           s.qps, s.avg_us, s.p50_us, s.p95_us, s.p99_us);
}

// ============================================================
// 入口
// ============================================================
int main(int argc, char* argv[]) {
    // 命令行解析
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            g_mode = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            g_port = static_cast<uint16_t>(atoi(argv[++i]));
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            g_threads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--requests") == 0 && i + 1 < argc) {
            g_requests = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--warmup") == 0 && i + 1 < argc) {
            g_warmup = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            g_timeout_s = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("用法: bench_client --mode rpc|http [选项]\n");
            printf("  --mode       rpc = TinyRPC, http = HTTP+JSON\n");
            printf("  --port PORT  服务端端口 (默认 8080)\n");
            printf("  --threads N  并发线程数 (默认 1)\n");
            printf("  --requests N 每线程请求数 (默认 10000)\n");
            printf("  --warmup N   每线程预热数 (默认 1000)\n");
            printf("  --timeout S  单请求超时秒数 (默认 5)\n");
            return 0;
        }
    }

    printf("=== TinyRPC Benchmark Client ===\n");
    printf("模式: %s | 端口: %u | 线程: %d | 请求/线程: %d | 预热: %d\n", g_mode, g_port, g_threads,
           g_requests, g_warmup);
    fflush(stdout);

    // ---- 启动所有线程 ----
    std::vector<std::thread> threads;
    std::vector<std::vector<double>> all_latencies(static_cast<size_t>(g_threads));
    std::vector<int> all_failed(static_cast<size_t>(g_threads), 0);

    auto t_start = std::chrono::steady_clock::now();

    for (int i = 0; i < g_threads; i++) {
        threads.emplace_back([i, &all_latencies, &all_failed]() {
            if (strcmp(g_mode, "rpc") == 0) {
                RunRpcThread(i, g_warmup, g_requests, all_latencies[static_cast<size_t>(i)],
                             all_failed[static_cast<size_t>(i)]);
            } else if (strcmp(g_mode, "http") == 0) {
                RunHttpThread(i, g_warmup, g_requests, all_latencies[static_cast<size_t>(i)],
                              all_failed[static_cast<size_t>(i)]);
            }
        });
    }

    // 等待所有线程完成
    for (auto& t : threads) {
        t.join();
    }

    auto t_end = std::chrono::steady_clock::now();
    double elapsed_sec =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_start).count()) /
        1e6;

    // ---- 汇总统计 ----
    int total_req = 0, total_failed = 0;
    std::vector<double> merged;
    merged.reserve(static_cast<size_t>(g_threads * g_requests));

    for (int i = 0; i < g_threads; i++) {
        total_req += g_requests;
        total_failed += all_failed[static_cast<size_t>(i)];
        merged.insert(merged.end(), all_latencies[static_cast<size_t>(i)].begin(),
                      all_latencies[static_cast<size_t>(i)].end());
    }

    // ---- 输出报告 ----
    auto stats = ComputeStats(merged, total_req, total_failed, elapsed_sec);
    PrintReport(stats);

    return 0;
}