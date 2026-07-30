// ============================================================
// bench_game_client — 游戏层压测客户端
//
// 用法：
//   ./bench_game_client --mode single   # 单连接基线
//   ./bench_game_client --mode ramp     # 渐进加压
//   ./bench_game_client --mode steady   # 稳态压测
//   ./bench_game_client --mode chaos    # 异常测试
//
// 完整链路：连接 → 登录 → 创建房间 → 发消息(循环) → 离开 → 断连
//
// 依赖：
//   - rpc::RpcClient         已有 TCP 连接 + RPC 调用
//   - game::RoomServiceStub  已有 Protobuf 房间 RPC Stub
//   - rpc::BenchStats        新增统计模块 (bench_stats.h)
//   - game.pb.h              Protobuf 生成的消息类
// ============================================================

#include "rpc/rpc_client.h"
#include "rpc/bench_stats.h"
#include "game.pb.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <random>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <numeric>

using namespace game; // Protobuf 消息类型都在 game:: 命名空间中

// ============================================================
// 命令行参数
// ============================================================
static const char* g_mode = "single"; // single | ramp | steady | chaos
static const char* g_server_ip = "127.0.0.1";
static uint16_t g_server_port = 8080;
static int g_connections = 1; // 总连接数
static int g_duration_sec = 60; // 稳态持续时间
static int g_ramp_rate = 10; // 每秒新建连接数（ramp 模式）
static double g_think_mean_ms = 200.0; // think time 泊松均值（ms）
static int g_warmup_sec = 10; // 预热时间（秒）
static int g_timeout_ms = 5000; // 单请求超时（ms）
static int g_repeat = 1; // 重复次数（single 模式）
static bool g_verbose = false; // 详细输出

// chaos 模式参数
static int g_chaos_disconnect_pct = 50; // 断连百分比
static int g_chaos_bad_msg_count = 100; // 恶意消息数量
static int g_chaos_oversize_bytes = 65536; // 超大包字节数

// ============================================================
// Poisson 分布 think time 生成器
// ============================================================
class PoissonTimer {
public:
    explicit PoissonTimer(double mean_ms)
        : mean_ms_(mean_ms)
        , dist_(mean_ms > 0 ? 1.0 / mean_ms : 1000.0) // lambda = 1/mean for exponential
        , rng_(std::random_device{}()) {
    }

    // 返回下一次等待的毫秒数（指数分布 = Poisson 过程的间隔时间）
    double NextMs() {
        return std::max(1.0, dist_(rng_)); // 至少 1ms
    }

private:
    double mean_ms_;
    std::exponential_distribution<double> dist_;
    std::mt19937_64 rng_;
};

// ============================================================
// 全局统计和同步
// ============================================================
static rpc::LatencyHistogram g_latency_hist; // 全局延迟直方图
static rpc::QpsCounter g_qps_counter(5); // 5秒滑动窗口
static rpc::ErrorCounter g_error_counter; // 错误分类计数
static rpc::StageTimer g_stage_timer; // 阶段耗时
static std::atomic<bool> g_stop_flag{false};
static std::atomic<int> g_active_connections{0};

// ============================================================
// 辅助函数
// ============================================================

// 当前时间戳（微秒）
static int64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// 将 Protobuf 消息序列化为 vector<uint8_t>
template <typename ProtoMsg> static std::vector<uint8_t> SerializeProto(const ProtoMsg& msg) {
    std::string buf;
    msg.SerializeToString(&buf);
    return std::vector<uint8_t>(buf.begin(), buf.end());
}

// 生成随机玩家 ID
static std::string PlayerId(int index) {
    std::ostringstream oss;
    oss << "bench_player_" << index;
    return oss.str();
}

// ============================================================
// GameClient — 单个游戏客户端
//
// 封装完整的客户端生命周期：
//   连接 → 登录 → 创建房间 → [发消息循环] → 离开房间 → 断连
//
// 每次 RPC 调用都记录延迟和阶段耗时。
// ============================================================
class GameClient {
public:
    GameClient(int id, const char* server_ip, uint16_t server_port)
        : id_(id)
        , player_id_(PlayerId(id))
        , server_ip_(server_ip)
        , server_port_(server_port)
        , client_(new rpc::RpcClient())
        , think_timer_(g_think_mean_ms) {
    }

    ~GameClient() {
        Disconnect();
    }

    // 禁止拷贝
    GameClient(const GameClient&) = delete;
    GameClient& operator=(const GameClient&) = delete;

    // ---- 完整流程 ----

    bool RunFullFlow(int duration_sec, bool record_stages = true) {
        if (g_stop_flag.load())
            return false;

        // 1. 连接
        if (record_stages) {
            int64_t t0 = NowUs();
            bool ok = Connect();
            int64_t t1 = NowUs();
            g_stage_timer.Record("connect", (t1 - t0), ok);
            if (!ok)
                return false;
        } else {
            if (!Connect())
                return false;
        }

        // 2. 登录
        if (record_stages) {
            int64_t t0 = NowUs();
            bool ok = Login();
            int64_t t1 = NowUs();
            g_stage_timer.Record("login", (t1 - t0), ok);
            if (!ok)
                return false;
        } else {
            if (!Login())
                return false;
        }

        // 3. 创建房间
        if (record_stages) {
            int64_t t0 = NowUs();
            bool ok = CreateGameRoom();
            int64_t t1 = NowUs();
            g_stage_timer.Record("create_room", (t1 - t0), ok);
            if (!ok)
                return false;
        } else {
            if (!CreateGameRoom())
                return false;
        }

        // 4. 开始游戏
        if (record_stages) {
            int64_t t0 = NowUs();
            bool ok = StartGameRoom();
            int64_t t1 = NowUs();
            g_stage_timer.Record("start_game", (t1 - t0), ok);
            // start_game failure is non-fatal (room may already be playing)
        } else {
            StartGameRoom();
        }

        // 5. 发消息循环（持续 duration_sec 或直到 stop）
        printf("[client %d] 开始发消息循环 (%d 秒，think time %.0f ms)...\n", id_, duration_sec,
               g_think_mean_ms);
        fflush(stdout);
        if (record_stages) {
            int64_t t0 = NowUs();
            MessageLoop(duration_sec);
            int64_t t1 = NowUs();
            g_stage_timer.Record("message_loop", (t1 - t0), true);
        } else {
            MessageLoop(duration_sec);
        }

        // 6. 离开房间
        if (record_stages) {
            int64_t t0 = NowUs();
            bool ok = LeaveGameRoom();
            int64_t t1 = NowUs();
            g_stage_timer.Record("leave_room", (t1 - t0), ok);
        } else {
            LeaveGameRoom();
        }

        // 7. 断连
        Disconnect();
        return true;
    }

    // ---- 单步操作（供外部细粒度控制） ----

    bool Connect() {
        if (!client_->Connect(server_ip_, server_port_)) {
            g_error_counter.Record("connect_failed");
            if (g_verbose)
                fprintf(stderr, "[client %d] connect failed\n", id_);
            return false;
        }
        connected_ = true;
        g_active_connections.fetch_add(1);
        return true;
    }

    // 带超时的 RPC 调用辅助模板
    template <typename ResProto>
    bool TimedCall(const std::string& method_name, const std::vector<uint8_t>& req_body,
                   ResProto& res, const char* err_label) {
        auto future = client_->Call(method_name, req_body);
        auto status = future.wait_for(std::chrono::milliseconds(g_timeout_ms));
        if (status != std::future_status::ready) {
            g_error_counter.Record(std::string(err_label) + "_timeout");
            return false;
        }
        auto rsp_body = future.get();
        if (!res.ParseFromArray(rsp_body.data(), static_cast<int>(rsp_body.size()))) {
            g_error_counter.Record(std::string(err_label) + "_parse_error");
            return false;
        }
        if (!res.success()) {
            g_error_counter.Record(std::string(err_label) + "_failed");
            return false;
        }
        return true;
    }

    bool Login() {
        LoginReq req;
        req.set_token(player_id_);

        int64_t t0 = NowUs();
        LoginRes res;
        bool ok = TimedCall("Login", SerializeProto(req), res, "login");
        int64_t t1 = NowUs();

        g_latency_hist.Record(static_cast<double>(t1 - t0));
        g_qps_counter.RecordRequest();

        if (!ok)
            return false;
        logged_in_ = true;
        return true;
    }

    bool CreateGameRoom() {
        CreateRoomReq req;
        req.set_player_id(player_id_);
        req.set_room_name("bench_room_" + std::to_string(id_));
        req.set_max_players(2);

        int64_t t0 = NowUs();
        CreateRoomRes res;
        bool ok = TimedCall("CreateRoom", SerializeProto(req), res, "create_room");
        int64_t t1 = NowUs();

        double latency_us = static_cast<double>(t1 - t0);
        g_latency_hist.Record(latency_us);
        g_qps_counter.RecordRequest();

        if (!ok) {
            if (g_verbose)
                fprintf(stderr, "[client %d] create room failed\n", id_);
            return false;
        }

        room_id_ = res.room_info().room_id();
        return true;
    }

    bool StartGameRoom() {
        StartGameReq req;
        req.set_room_id(room_id_);
        req.set_player_id(player_id_);

        int64_t t0 = NowUs();
        StartGameRes res;
        bool ok = TimedCall("StartGame", SerializeProto(req), res, "start_game");
        int64_t t1 = NowUs();

        double latency_us = static_cast<double>(t1 - t0);
        g_latency_hist.Record(latency_us);
        g_qps_counter.RecordRequest();

        if (!ok) {
            g_error_counter.Record("start_game_failed");
            return false;
        }
        return true;
    }

    bool SendOneMessage() {
        SendMessageReq req;
        req.set_room_id(room_id_);
        req.set_sender_id(player_id_);
        req.set_content("bench_msg_" + std::to_string(msg_seq_++));

        int64_t t0 = NowUs();
        SendMessageRes res;
        bool ok = TimedCall("SendMessage", SerializeProto(req), res, "send_msg");
        int64_t t1 = NowUs();

        double latency_us = static_cast<double>(t1 - t0);
        g_latency_hist.Record(latency_us);
        g_qps_counter.RecordRequest();

        if (!ok)
            return false;
        return true;
    }

    // ---- 帧同步 SendInput ----

    bool SendInput(uint32_t frame_no, const std::string& input_data) {
        PlayerInputReq req;
        req.set_frame_no(frame_no);
        req.set_player_id(player_id_);
        req.set_input_data(input_data);

        int64_t t0 = NowUs();
        SendInputRes res;
        bool ok = TimedCall("SendInput", SerializeProto(req), res, "send_input");
        int64_t t1 = NowUs();

        double latency_us = static_cast<double>(t1 - t0);
        g_latency_hist.Record(latency_us);
        g_qps_counter.RecordRequest();

        if (!ok)
            return false;
        return true;
    }

    // ---- 匹配 ----

    bool EnterMatch(double elo_score) {
        MatchPlayerReq req;
        req.set_player_id(player_id_);
        req.set_elo_score(elo_score);

        int64_t t0 = NowUs();
        MatchPlayerRes res;
        bool ok = TimedCall("EnterMatch", SerializeProto(req), res, "enter_match");
        int64_t t1 = NowUs();

        double latency_us = static_cast<double>(t1 - t0);
        g_latency_hist.Record(latency_us);
        g_qps_counter.RecordRequest();

        if (!ok)
            return false;
        return true;
    }

    bool CancelMatch() {
        CancelMatchReq req;
        req.set_player_id(player_id_);

        int64_t t0 = NowUs();
        CancelMatchRes res;
        bool ok = TimedCall("CancelMatch", SerializeProto(req), res, "cancel_match");
        int64_t t1 = NowUs();

        double latency_us = static_cast<double>(t1 - t0);
        g_latency_hist.Record(latency_us);
        g_qps_counter.RecordRequest();

        if (!ok)
            return false;
        return true;
    }

    // ---- 加入房间（帧同步配对用） ----

    bool JoinRoom(const std::string& target_room_id) {
        JoinRoomReq req;
        req.set_player_id(player_id_);
        req.set_room_id(target_room_id);

        int64_t t0 = NowUs();
        JoinRoomRes res;
        bool ok = TimedCall("JoinRoom", SerializeProto(req), res, "join_room");
        int64_t t1 = NowUs();

        double latency_us = static_cast<double>(t1 - t0);
        g_latency_hist.Record(latency_us);
        g_qps_counter.RecordRequest();

        if (!ok)
            return false;
        room_id_ = target_room_id;
        return true;
    }

    bool LeaveGameRoom() {
        LeaveRoomReq req;
        req.set_player_id(player_id_);
        req.set_room_id(room_id_);

        int64_t t0 = NowUs();
        LeaveRoomRes res;
        bool ok = TimedCall("LeaveRoom", SerializeProto(req), res, "leave_room");
        int64_t t1 = NowUs();

        double latency_us = static_cast<double>(t1 - t0);
        g_latency_hist.Record(latency_us);
        g_qps_counter.RecordRequest();

        if (!ok)
            return false;
        return true;
    }

    void Disconnect() {
        if (connected_) {
            // 仅关闭 fd，不触 EventLoop handlers_ map（~RpcClient 会安全清理）
            client_->CloseFd();
            g_active_connections.fetch_sub(1);
            connected_ = false;
        }
    }

    // ---- chaos 模式专用 ----

    void SendOversizedPacket() {
        // 发送超大 Protobuf 消息
        SendMessageReq req;
        req.set_room_id(room_id_);
        req.set_sender_id(player_id_);
        req.set_content(std::string(g_chaos_oversize_bytes, 'X'));

        auto body = SerializeProto(req);
        auto future = client_->Call("SendMessage", body);
        future.wait_for(std::chrono::milliseconds(g_timeout_ms));
        // 不记录统计 — 这是恶意测试
    }

    void SendInvalidProtobuf() {
        // 发送无法解析的二进制数据
        std::vector<uint8_t> garbage(256, 0xFF);
        auto future = client_->Call("CreateRoom", garbage);
        future.wait_for(std::chrono::milliseconds(g_timeout_ms));
    }

    void SendNonExistentRoom() {
        SendMessageReq req;
        req.set_room_id("non_existent_room_99999");
        req.set_sender_id(player_id_);
        req.set_content("bad_room_msg");

        auto body = SerializeProto(req);
        auto future = client_->Call("SendMessage", body);
        future.wait_for(std::chrono::milliseconds(g_timeout_ms));
    }

    // 查询服务端指标
    bool QueryServerMetrics() {
        GetMetricsRes res;
        bool ok = TimedCall("GetMetrics", {}, res, "get_metrics");
        if (ok) {
            printf("[metrics] 服务端: 运行%lds | 连接=%d | 房间=%d | QPS≈%.0f | "
                   "延迟(us): avg=%.0f p50=%.0f p99=%.0f | 匹配队列=%d | 错误=%ld\n",
                   res.uptime_sec(), res.active_connections(), res.total_rooms(), res.current_qps(),
                   res.avg_latency_us(), res.p50_latency_us(), res.p99_latency_us(),
                   res.match_queue_size(), res.error_count());
            fflush(stdout);
        }
        return ok;
    }

    const std::string& player_id() const {
        return player_id_;
    }
    const std::string& room_id() const {
        return room_id_;
    }
    int id() const {
        return id_;
    }

private:
    void MessageLoop(int duration_sec) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration_sec);
        auto last_report = std::chrono::steady_clock::now();

        while (!g_stop_flag.load() && std::chrono::steady_clock::now() < deadline) {
            if (!SendOneMessage())
                break;

            // 每 5 秒输出一次进度（仅 single 模式且 verbose 时）
            auto now = std::chrono::steady_clock::now();
            if (g_verbose && now - last_report > std::chrono::seconds(5)) {
                auto remaining =
                    std::chrono::duration_cast<std::chrono::seconds>(deadline - now).count();
                printf("  [进度] 已发送 %d 条消息 | 剩余 %ld 秒\n", msg_seq_, remaining);
                fflush(stdout);
                last_report = now;
            }

            // think time（泊松间隔）
            double wait_ms = think_timer_.NextMs();
            if (wait_ms > 1000.0)
                wait_ms = 1000.0;

            std::this_thread::sleep_for(
                std::chrono::microseconds(static_cast<int64_t>(wait_ms * 1000.0)));
        }
    }

    void FramesyncInputLoop(int duration_sec) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration_sec);
        uint32_t frame_no = 0;
        // 帧同步 tick 间隔 = 50ms (20 tick/s)，匹配服务端帧率
        constexpr int kTickIntervalUs = 50'000;

        while (!g_stop_flag.load() && std::chrono::steady_clock::now() < deadline) {
            // 构造模拟输入数据（方向 + 按键，每次随机变化）
            std::ostringstream oss;
            oss << "input_" << frame_no << "_" << (rand() % 256);
            std::string input = oss.str();

            if (!SendInput(frame_no, input))
                break;

            frame_no++;
            std::this_thread::sleep_for(std::chrono::microseconds(kTickIntervalUs));
        }
    }

    void MatchLoop(int duration_sec) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration_sec);
        std::mt19937 rng(std::random_device{}());
        // 随机 ELO 分数 800~2200
        std::uniform_real_distribution<double> elo_dist(800.0, 2200.0);

        int cycle = 0;
        while (!g_stop_flag.load() && std::chrono::steady_clock::now() < deadline) {
            double elo = elo_dist(rng);
            cycle++;

            // 入队
            if (!EnterMatch(elo))
                break;
            // 短暂等待让服务端 TryMatch 有机会配对
            std::this_thread::sleep_for(std::chrono::milliseconds(100 + (rand() % 200)));
            // 取消匹配（清理队列状态，准备下一轮）
            CancelMatch();
            // 间隔 200~500ms 后下一轮
            std::this_thread::sleep_for(std::chrono::milliseconds(200 + (rand() % 300)));
        }
    }

    int id_;
    std::string player_id_;
    std::string room_id_;
    const char* server_ip_;
    uint16_t server_port_;
    std::unique_ptr<rpc::RpcClient> client_;
    PoissonTimer think_timer_;
    int msg_seq_ = 0;
    bool connected_ = false;
    bool logged_in_ = false;
};

// ============================================================
// 统计报告线程（定期输出中间数据）
// ============================================================
// 全局 metrics 查询客户端（StatsReporterLoop 用它查服务端指标）
// 注意：必须使用独立连接，不能复用消息循环线程的 RpcClient，
// 否则两个线程同时 send() 同一个 fd → TCP 字节流损坏 → 服务端崩溃
static std::unique_ptr<GameClient> g_metrics_client;

static void StatsReporterLoop(int interval_sec) {
    while (!g_stop_flag.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(interval_sec));
        if (g_stop_flag.load())
            break;

        g_qps_counter.Tick(); // 推进滑动窗口，否则 CurrentQps() 始终返回 0
        auto p = g_latency_hist.Compute();
        double qps = g_qps_counter.CurrentQps();
        int active = g_active_connections.load();

        printf("[stats] conn=%d | QPS≈%.0f | lat(us): avg=%.0f p50=%.0f p95=%.0f p99=%.0f | "
               "errors=%d\n",
               active, qps, p.avg, p.p50, p.p95, p.p99, g_error_counter.Total());
        fflush(stdout);

        // 查询服务端指标 — 使用独立连接，不干扰消息发送
        if (g_metrics_client) {
            g_metrics_client->QueryServerMetrics();
        }
    }
}

// ============================================================
// MODE: single — 单连接基线
// ============================================================
static void RunSingleMode() {
    printf("\n=== MODE: single — 单连接基线 ===\n");
    printf("重复次数: %d | think time 均值: %.0f ms | 超时: %d ms\n", g_repeat, g_think_mean_ms,
           g_timeout_ms);
    printf("----------------------------------------\n");

    for (int round = 0; round < g_repeat; round++) {
        printf("\n--- 第 %d/%d 轮 ---\n", round + 1, g_repeat);

        GameClient client(round, g_server_ip, g_server_port);
        bool ok = client.RunFullFlow(g_duration_sec, /*record_stages=*/true);

        printf("  结果: %s | 活跃连接: %d\n", ok ? "成功" : "失败", g_active_connections.load());
    }

    // 输出阶段耗时汇总
    auto stages = g_stage_timer.Results();
    rpc::BenchReport::PrintStageTable(stages);

    auto p = g_latency_hist.Compute();
    rpc::BenchReport::PrintLatencyTable(p);

    // Markdown 表格行
    g_qps_counter.Tick();
    printf("\n--- Markdown ---\n");
    printf("| 场景 | 连接数 | 时长 | QPS | avg(us) | p50(us) | p95(us) | p99(us) |\n");
    printf("|------|--------|------|-----|---------|---------|---------|----------|\n");
    rpc::BenchReport::PrintMarkdownRow("single-baseline", g_connections, g_duration_sec,
                                       g_qps_counter.CurrentQps(), p);
}

// ============================================================
// MODE: ramp — 渐进加压
// ============================================================
static void RunRampMode() {
    printf("\n=== MODE: ramp — 渐进加压 ===\n");
    printf("目标连接数: %d | ramp 速率: %d conn/s | 每连接持续: %d s\n", g_connections, g_ramp_rate,
           g_duration_sec);
    printf("think time 均值: %.0f ms\n", g_think_mean_ms);
    printf("----------------------------------------\n");

    // 启动统计报告线程
    g_stop_flag.store(false);
    std::thread reporter(StatsReporterLoop, 5);

    std::vector<std::thread> client_threads;
    int ramp_interval_us = (g_ramp_rate > 0) ? (1'000'000 / g_ramp_rate) : 100'000;

    auto ramp_start = std::chrono::steady_clock::now();

    for (int i = 0; i < g_connections; i++) {
        if (g_stop_flag.load())
            break;

        client_threads.emplace_back([i]() {
            GameClient client(i, g_server_ip, g_server_port);
            client.RunFullFlow(g_duration_sec, /*record_stages=*/(i == 0));
        });

        // 按照 ramp rate 间隔创建连接
        if (i < g_connections - 1) {
            std::this_thread::sleep_for(std::chrono::microseconds(ramp_interval_us));
        }

        // 每 50 个连接报告一次进度
        if ((i + 1) % 50 == 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now() - ramp_start)
                               .count();
            printf("[ramp] %d/%d 连接已创建 (%.0fs) | 活跃: %d\n", i + 1, g_connections,
                   static_cast<double>(elapsed), g_active_connections.load());
            fflush(stdout);
        }
    }

    printf("[ramp] 全部 %d 连接已创建，等待完成...\n", g_connections);

    // 等待所有线程完成
    for (auto& t : client_threads) {
        t.join();
    }

    g_stop_flag.store(true);
    reporter.join();

    // 输出报告
    auto p = g_latency_hist.Compute();
    rpc::BenchReport::PrintFullReport("ramp-up", g_connections, g_duration_sec,
                                      g_qps_counter.CurrentQps(), p, g_error_counter,
                                      g_stage_timer.Results());
}

// ============================================================
// MODE: steady — 稳态压测
// ============================================================
static void RunSteadyMode() {
    printf("\n=== MODE: steady — 稳态压测 ===\n");
    printf("并发连接数: %d | 持续时间: %d s | 预热: %d s\n", g_connections, g_duration_sec,
           g_warmup_sec);
    printf("think time 均值: %.0f ms\n", g_think_mean_ms);
    printf("----------------------------------------\n");

    g_stop_flag.store(false);

    // 统计报告线程
    std::thread reporter(StatsReporterLoop, 5);

    // 第一步：创建所有连接（ramp-up）
    printf("[steady] 正在建立 %d 个连接...\n", g_connections);
    std::vector<std::unique_ptr<GameClient>> clients;
    std::vector<std::thread> threads;

    int ramp_interval_us = (g_ramp_rate > 0) ? (1'000'000 / g_ramp_rate) : 50'000;

    for (int i = 0; i < g_connections; i++) {
        auto client = std::make_unique<GameClient>(i, g_server_ip, g_server_port);
        if (!client->Connect()) {
            fprintf(stderr, "[steady] 客户端 %d 连接失败，跳过\n", i);
            continue;
        }
        if (!client->Login()) {
            fprintf(stderr, "[steady] 客户端 %d 登录失败，跳过\n", i);
            client->Disconnect();
            continue;
        }
        if (!client->CreateGameRoom()) {
            fprintf(stderr, "[steady] 客户端 %d 创建房间失败，跳过\n", i);
            client->Disconnect();
            continue;
        }
        clients.push_back(std::move(client));

        if (i < g_connections - 1) {
            std::this_thread::sleep_for(std::chrono::microseconds(ramp_interval_us));
        }
    }

    int connected = static_cast<int>(clients.size());
    printf("[steady] 成功建立 %d/%d 连接\n", connected, g_connections);

    // 创建独立的 metrics 查询连接（不复用消息线程的 RpcClient，避免并发 send() 损坏 TCP 流）
    g_metrics_client = std::make_unique<GameClient>(-1, g_server_ip, g_server_port);
    if (g_metrics_client->Connect() && g_metrics_client->Login()) {
        printf("[steady] metrics 查询连接已建立 (独立连接)\n");
    } else {
        printf("[steady] metrics 查询连接失败，将跳过服务端指标查询\n");
        g_metrics_client.reset();
    }

    // 第二步：预热
    if (g_warmup_sec > 0 && connected > 0) {
        printf("[steady] 预热 %d 秒...\n", g_warmup_sec);
        auto warmup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(g_warmup_sec);

        // 用第一个客户端发送预热消息
        while (std::chrono::steady_clock::now() < warmup_deadline && !g_stop_flag.load()) {
            clients[0]->SendOneMessage();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        // 重置统计（不计入预热数据）
        g_latency_hist.Reset();
        g_qps_counter.Reset();
        g_error_counter.Reset();
        printf("[steady] 预热完成，开始正式采集...\n");
    }

    // 第三步：稳态运行 — 所有客户端并发发消息
    auto steady_start = std::chrono::steady_clock::now();
    auto steady_deadline = steady_start + std::chrono::seconds(g_duration_sec);

    for (int i = 0; i < connected; i++) {
        threads.emplace_back([&clients, i, steady_deadline]() {
            auto& client = clients[i];
            while (!g_stop_flag.load() && std::chrono::steady_clock::now() < steady_deadline) {
                if (!client->SendOneMessage())
                    break;

                // think time
                double wait_ms = g_think_mean_ms > 0 ? PoissonTimer(g_think_mean_ms).NextMs() : 0;
                if (wait_ms > 1000.0)
                    wait_ms = 1000.0;
                if (wait_ms > 0) {
                    std::this_thread::sleep_for(
                        std::chrono::microseconds(static_cast<int64_t>(wait_ms * 1000.0)));
                }
            }
        });
    }

    // 等待稳态结束
    for (auto& t : threads) {
        t.join();
    }

    auto steady_end = std::chrono::steady_clock::now();
    double elapsed =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(steady_end - steady_start).count()) /
        1e6;

    g_stop_flag.store(true);
    reporter.join();

    // 第四步：清理
    if (g_metrics_client) {
        g_metrics_client->Disconnect();
        g_metrics_client.reset();
    }
    printf("[steady] 正在清理 %d 个客户端...\n", connected);
    for (auto& client : clients) {
        client->LeaveGameRoom();
        client->Disconnect();
    }

    // 输出报告
    auto p = g_latency_hist.Compute();
    printf("\n");
    printf("========================================\n");
    printf("  稳态压测报告\n");
    printf("========================================\n");
    printf("  目标连接数:   %d\n", g_connections);
    printf("  成功连接数:   %d\n", connected);
    printf("  成功建立率:   %.1f%%\n", 100.0 * connected / g_connections);
    printf("  采集时长:     %.1f 秒\n", elapsed);
    printf("  总请求数:     %zu\n", static_cast<size_t>(g_qps_counter.CurrentQps() * elapsed));
    printf("  QPS:          %.1f\n", g_qps_counter.CurrentQps());
    printf("  错误总数:     %d\n", g_error_counter.Total());
    printf("----------------------------------------\n");
    rpc::BenchReport::PrintLatencyTable(p);

    // 错误分类
    if (g_error_counter.Total() > 0) {
        printf("\n  错误分类:\n");
        g_error_counter.ForEach(
            [](const std::string& cat, int count) { printf("    %s: %d\n", cat.c_str(), count); });
    }

    // Markdown
    printf("\n--- Markdown ---\n");
    printf("| 场景 | 连接数 | 时长 | QPS | avg(us) | p50(us) | p95(us) | p99(us) | 成功率 |\n");
    printf("|------|--------|------|-----|---------|---------|---------|----------|--------|\n");
    printf("| steady-%d | %d | %.0fs | %.0f | %.1f | %.1f | %.1f | %.1f | %.1f%% |\n",
           g_connections, connected, elapsed, g_qps_counter.CurrentQps(), p.avg, p.p50, p.p95,
           p.p99, 100.0 * connected / g_connections);
    printf("========================================\n");
}

// ============================================================
// MODE: chaos — 异常测试
// ============================================================
static void RunChaosMode() {
    printf("\n=== MODE: chaos — 异常测试 ===\n");
    printf("连接数: %d | 断连%%: %d | 恶意消息数: %d | 超大包: %d B\n", g_connections,
           g_chaos_disconnect_pct, g_chaos_bad_msg_count, g_chaos_oversize_bytes);
    printf("----------------------------------------\n");

    g_stop_flag.store(false);

    // 第一阶段：建立连接并进入稳态
    printf("[chaos] 阶段1: 建立 %d 个连接...\n", g_connections);
    std::vector<std::unique_ptr<GameClient>> clients;

    for (int i = 0; i < g_connections; i++) {
        auto client = std::make_unique<GameClient>(i, g_server_ip, g_server_port);
        if (!client->Connect() || !client->Login() || !client->CreateGameRoom()) {
            client->Disconnect();
            continue;
        }
        clients.push_back(std::move(client));
    }

    int connected = static_cast<int>(clients.size());
    printf("[chaos] 成功建立 %d/%d 连接\n", connected, g_connections);

    // 发几轮消息，确认稳态
    printf("[chaos] 稳态运行 5 秒...\n");
    for (int round = 0; round < 5; round++) {
        for (auto& c : clients) {
            c->SendOneMessage();
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // 第二阶段：大量断连
    int disconnect_count = connected * g_chaos_disconnect_pct / 100;
    printf("[chaos] 阶段2: 瞬间断开 %d 个连接...\n", disconnect_count);

    // 用随机采样决定断开哪些
    std::vector<int> indices(connected);
    std::iota(indices.begin(), indices.end(), 0);
    std::shuffle(indices.begin(), indices.end(), std::mt19937(std::random_device{}()));

    auto disco_start = std::chrono::steady_clock::now();
    for (int i = 0; i < disconnect_count; i++) {
        // reset() 析构 GameClient → ~RpcClient 先 Stop+join EventLoop 再安全清理
        clients[indices[i]].reset();
    }
    auto disco_end = std::chrono::steady_clock::now();
    double disco_elapsed_ms =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(disco_end - disco_start).count()) /
        1000.0;

    printf("[chaos] 断开完成: %d 个连接 / %.1f ms\n", disconnect_count, disco_elapsed_ms);
    printf("[chaos] 剩余活跃: %d\n", g_active_connections.load());

    // 等待 3 秒，观察 server 是否能正确处理
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // 第三阶段：恶意消息
    printf("[chaos] 阶段3: 发送恶意消息...\n");

    // 找一个还活着的客户端发送恶意消息
    GameClient* survivor = nullptr;
    for (auto& c : clients) {
        if (c) {
            survivor = c.get();
            break;
        }
    }
    if (!survivor) {
        printf("[chaos] 所有客户端已断开，跳过恶意消息测试\n");
        g_stop_flag.store(true);
        return;
    }

    // 超大包
    printf("[chaos]   发送 %d 个超大包 (%d B)...\n", g_chaos_bad_msg_count, g_chaos_oversize_bytes);
    for (int i = 0; i < g_chaos_bad_msg_count; i++) {
        survivor->SendOversizedPacket();
    }

    // 非法 Protobuf
    printf("[chaos]   发送 %d 个非法 Protobuf...\n", g_chaos_bad_msg_count);
    for (int i = 0; i < g_chaos_bad_msg_count; i++) {
        survivor->SendInvalidProtobuf();
    }

    // 不存在的房间
    printf("[chaos]   发送 %d 个不存在房间的消息...\n", g_chaos_bad_msg_count);
    for (int i = 0; i < g_chaos_bad_msg_count; i++) {
        survivor->SendNonExistentRoom();
    }

    printf("[chaos] 恶意消息发送完成\n");

    // 第四阶段：验证 server 还活着（发正常消息）
    printf("[chaos] 阶段4: 验证 server 仍然可用...\n");
    for (auto& c : clients) {
        if (c && c->SendOneMessage()) {
            // OK
        } else if (c) {
            printf("[chaos]   客户端 %d 发送失败（server 可能已崩溃？）\n", c->id());
        }
    }

    // 清理
    printf("[chaos] 清理剩余连接...\n");
    for (auto& c : clients) {
        if (c) {
            c->LeaveGameRoom();
            c->Disconnect();
        }
    }

    g_stop_flag.store(true);

    printf("\n[chaos] 异常测试完成\n");
    printf("  总错误数: %d\n", g_error_counter.Total());
    printf("  断连时间: %.1f ms\n", disco_elapsed_ms);
}

// ============================================================
// MODE: framesync — 帧同步压测
// ============================================================
static void RunFramesyncMode() {
    printf("\n=== MODE: framesync — 帧同步压测 ===\n");
    printf("客户端总数: %d | 房间数: %d (2人/房) | 持续: %d s | ramp: %d conn/s\n", g_connections,
           g_connections / 2, g_duration_sec, g_ramp_rate);
    printf("帧同步 tick: 20 fps (50ms 间隔)\n");
    printf("----------------------------------------\n");

    g_stop_flag.store(false);
    std::thread reporter(StatsReporterLoop, 5);

    // 共享的 room_id 数组：索引 i 对应客户端 i
    std::vector<std::string> room_ids(g_connections);
    std::vector<std::unique_ptr<GameClient>> clients(g_connections);
    std::mutex room_ids_mutex;

    int ramp_interval_us = (g_ramp_rate > 0) ? (1'000'000 / g_ramp_rate) : 50'000;

    // ---- 阶段 1: 偶数号客户端创建房间 ----
    printf("[framesync] 阶段1: 创建 %d 个房间...\n", g_connections / 2);
    std::vector<std::thread> phase1_threads;

    for (int i = 0; i < g_connections; i += 2) {
        phase1_threads.emplace_back([i, &clients, &room_ids, &room_ids_mutex, ramp_interval_us]() {
            auto client = std::make_unique<GameClient>(i, g_server_ip, g_server_port);
            if (!client->Connect() || !client->Login()) {
                client->Disconnect();
                return;
            }
            if (!client->CreateGameRoom()) {
                client->Disconnect();
                return;
            }
            {
                std::lock_guard<std::mutex> lock(room_ids_mutex);
                room_ids[i] = client->room_id();
            }
            clients[i] = std::move(client);
        });

        if (i + 2 < g_connections) {
            std::this_thread::sleep_for(std::chrono::microseconds(ramp_interval_us));
        }
    }

    for (auto& t : phase1_threads)
        t.join();

    int rooms_created = 0;
    for (int i = 0; i < g_connections; i += 2) {
        if (clients[i])
            rooms_created++;
    }
    printf("[framesync] 房间创建完成: %d/%d\n", rooms_created, g_connections / 2);

    // ---- 阶段 2: 奇数号客户端加入房间 ----
    printf("[framesync] 阶段2: 加入房间...\n");
    std::vector<std::thread> phase2_threads;

    for (int i = 1; i < g_connections; i += 2) {
        phase2_threads.emplace_back([i, &clients, &room_ids, &room_ids_mutex, ramp_interval_us]() {
            int partner = i - 1;
            // 等待 partner 的房间创建完成
            std::string target_room;
            for (int wait = 0; wait < 50; wait++) {
                {
                    std::lock_guard<std::mutex> lock(room_ids_mutex);
                    target_room = room_ids[partner];
                }
                if (!target_room.empty())
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (target_room.empty()) {
                fprintf(stderr, "[framesync] 客户端 %d 等待 partner %d 房间超时\n", i, partner);
                return;
            }

            auto client = std::make_unique<GameClient>(i, g_server_ip, g_server_port);
            if (!client->Connect() || !client->Login()) {
                client->Disconnect();
                return;
            }
            if (!client->JoinRoom(target_room)) {
                fprintf(stderr, "[framesync] 客户端 %d 加入房间 %s 失败\n", i, target_room.c_str());
                client->Disconnect();
                return;
            }
            clients[i] = std::move(client);
        });

        if (i + 2 < g_connections) {
            std::this_thread::sleep_for(std::chrono::microseconds(ramp_interval_us));
        }
    }

    for (auto& t : phase2_threads)
        t.join();

    // ---- 阶段 3: 房主启动游戏（只有偶数号客户端是房主） ----
    printf("[framesync] 阶段3: 房主启动游戏...\n");
    for (int i = 0; i < g_connections; i += 2) {
        if (clients[i]) {
            clients[i]->StartGameRoom();
        }
    }
    // 等待帧同步启动
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // ---- 阶段 4: 帧同步输入循环 ----
    printf("[framesync] 阶段4: 帧同步输入循环 (%d 秒)...\n", g_duration_sec);
    auto sync_start = std::chrono::steady_clock::now();
    auto sync_deadline = sync_start + std::chrono::seconds(g_duration_sec);

    std::vector<std::thread> input_threads;
    for (int i = 0; i < g_connections; i++) {
        input_threads.emplace_back([i, &clients, sync_deadline]() {
            auto& client = clients[i];
            if (!client)
                return;

            uint32_t frame_no = 0;
            constexpr int kTickUs = 50'000; // 20 fps

            while (!g_stop_flag.load() && std::chrono::steady_clock::now() < sync_deadline) {
                // 构造模拟输入
                std::string input = "f" + std::to_string(frame_no) + "_p" + std::to_string(i);
                if (!client->SendInput(frame_no, input))
                    break;
                frame_no++;
                std::this_thread::sleep_for(std::chrono::microseconds(kTickUs));
            }
        });
    }

    for (auto& t : input_threads)
        t.join();

    auto sync_end = std::chrono::steady_clock::now();
    double elapsed =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(sync_end - sync_start).count()) /
        1e6;

    g_stop_flag.store(true);
    reporter.join();

    // ---- 阶段 5: 清理 ----
    printf("[framesync] 清理 %d 个客户端...\n", g_connections);
    for (auto& client : clients) {
        if (client) {
            client->LeaveGameRoom();
            client->Disconnect();
        }
    }

    // 输出报告
    auto p = g_latency_hist.Compute();
    printf("\n");
    printf("========================================\n");
    printf("  帧同步压测报告\n");
    printf("========================================\n");
    printf("  客户端总数:    %d\n", g_connections);
    printf("  房间数:        %d\n", rooms_created);
    printf("  帧率:          20 fps\n");
    printf("  采集时长:      %.1f 秒\n", elapsed);
    printf("  总请求数:      %zu\n", static_cast<size_t>(g_qps_counter.CurrentQps() * elapsed));
    printf("  QPS:           %.1f\n", g_qps_counter.CurrentQps());
    printf("  错误总数:      %d\n", g_error_counter.Total());
    printf("----------------------------------------\n");
    rpc::BenchReport::PrintLatencyTable(p);

    if (g_error_counter.Total() > 0) {
        printf("\n  错误分类:\n");
        g_error_counter.ForEach(
            [](const std::string& cat, int count) { printf("    %s: %d\n", cat.c_str(), count); });
    }

    printf("\n--- Markdown ---\n");
    printf("| 场景 | 连接数 | 时长 | QPS | avg(us) | p50(us) | p95(us) | p99(us) | 错误数 |\n");
    printf("|------|--------|------|-----|---------|---------|---------|----------|--------|\n");
    printf("| framesync-%d | %d | %.0fs | %.0f | %.1f | %.1f | %.1f | %.1f | %d |\n", g_connections,
           g_connections, elapsed, g_qps_counter.CurrentQps(), p.avg, p.p50, p.p95, p.p99,
           g_error_counter.Total());
    printf("========================================\n");
}

// ============================================================
// MODE: match — 匹配系统压测
// ============================================================
static void RunMatchMode() {
    printf("\n=== MODE: match — 匹配系统压测 ===\n");
    printf("客户端总数: %d | 持续: %d s | ramp: %d conn/s\n", g_connections, g_duration_sec,
           g_ramp_rate);
    printf("流程: 连接→登录→EnterMatch→等待→CancelMatch→循环\n");
    printf("----------------------------------------\n");

    g_stop_flag.store(false);
    std::thread reporter(StatsReporterLoop, 5);

    // ---- 阶段 1: 建立所有连接并登录 ----
    printf("[match] 阶段1: 建立 %d 个连接...\n", g_connections);
    std::vector<std::unique_ptr<GameClient>> clients;
    int ramp_interval_us = (g_ramp_rate > 0) ? (1'000'000 / g_ramp_rate) : 50'000;

    for (int i = 0; i < g_connections; i++) {
        auto client = std::make_unique<GameClient>(i, g_server_ip, g_server_port);
        if (!client->Connect() || !client->Login()) {
            client->Disconnect();
            continue;
        }
        clients.push_back(std::move(client));

        if (i < g_connections - 1) {
            std::this_thread::sleep_for(std::chrono::microseconds(ramp_interval_us));
        }
    }

    int connected = static_cast<int>(clients.size());
    printf("[match] 成功建立 %d/%d 连接\n", connected, g_connections);

    // ---- 阶段 2: 匹配循环 ----
    printf("[match] 阶段2: 匹配循环 (%d 秒)...\n", g_duration_sec);
    auto match_start = std::chrono::steady_clock::now();
    auto match_deadline = match_start + std::chrono::seconds(g_duration_sec);

    std::vector<std::thread> match_threads;
    for (int i = 0; i < connected; i++) {
        match_threads.emplace_back([i, &clients, match_deadline]() {
            auto& client = clients[i];
            std::mt19937 rng(std::random_device{}());
            // 随机 ELO 分数 800~2200
            std::uniform_real_distribution<double> elo_dist(800.0, 2200.0);

            int cycle = 0;
            while (!g_stop_flag.load() && std::chrono::steady_clock::now() < match_deadline) {
                double elo = elo_dist(rng);
                cycle++;

                // EnterMatch → 服务端 EnterQueue + TryMatch
                if (!client->EnterMatch(elo))
                    break;
                // 短暂等待让服务端有机会配对其他客户端
                std::this_thread::sleep_for(std::chrono::milliseconds(100 + (rand() % 200)));
                // CancelMatch 清理队列状态
                client->CancelMatch();
                // 间隔后下一轮
                std::this_thread::sleep_for(std::chrono::milliseconds(200 + (rand() % 300)));
            }
        });
    }

    for (auto& t : match_threads)
        t.join();

    auto match_end = std::chrono::steady_clock::now();
    double elapsed =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(match_end - match_start).count()) /
        1e6;

    g_stop_flag.store(true);
    reporter.join();

    // ---- 阶段 3: 清理 ----
    printf("[match] 清理 %d 个客户端...\n", connected);
    for (auto& client : clients) {
        client->Disconnect();
    }

    // 输出报告
    auto p = g_latency_hist.Compute();
    printf("\n");
    printf("========================================\n");
    printf("  匹配系统压测报告\n");
    printf("========================================\n");
    printf("  客户端总数:    %d\n", connected);
    printf("  成功连接数:    %d\n", connected);
    printf("  采集时长:      %.1f 秒\n", elapsed);
    printf("  总请求数:      %zu\n", static_cast<size_t>(g_qps_counter.CurrentQps() * elapsed));
    printf("  QPS:           %.1f\n", g_qps_counter.CurrentQps());
    printf("  错误总数:      %d\n", g_error_counter.Total());
    printf("----------------------------------------\n");
    rpc::BenchReport::PrintLatencyTable(p);

    if (g_error_counter.Total() > 0) {
        printf("\n  错误分类:\n");
        g_error_counter.ForEach(
            [](const std::string& cat, int count) { printf("    %s: %d\n", cat.c_str(), count); });
    }

    printf("\n--- Markdown ---\n");
    printf("| 场景 | 连接数 | 时长 | QPS | avg(us) | p50(us) | p95(us) | p99(us) | 错误数 |\n");
    printf("|------|--------|------|-----|---------|---------|---------|----------|--------|\n");
    printf("| match-%d | %d | %.0fs | %.0f | %.1f | %.1f | %.1f | %.1f | %d |\n", g_connections,
           connected, elapsed, g_qps_counter.CurrentQps(), p.avg, p.p50, p.p95, p.p99,
           g_error_counter.Total());
    printf("========================================\n");
}
static void PrintHelp() {
    printf("用法: bench_game_client --mode <MODE> [选项]\n\n");
    printf("模式:\n");
    printf("  single     单连接基线（记录各阶段耗时，可重复）\n");
    printf("  ramp       渐进加压（从 0 逐步增加到目标连接数）\n");
    printf("  steady     稳态压测（预热 + 固定连接数持续运行）\n");
    printf("  chaos      异常测试（断连风暴 + 恶意消息）\n");
    printf("  framesync  帧同步压测（2人组队 → StartGame → SendInput）\n");
    printf("  match      匹配系统压测（EnterMatch → CancelMatch 循环）\n\n");
    printf("通用选项:\n");
    printf("  --mode MODE           运行模式 (默认: single)\n");
    printf("  --server-ip IP        服务端地址 (默认: 127.0.0.1)\n");
    printf("  --port PORT           服务端端口 (默认: 8080)\n");
    printf("  --conn N              并发连接数 (默认: 1)\n");
    printf("  --duration SEC        每连接持续时间 (默认: 60)\n");
    printf("  --think-mean MS       泊松 think time 均值/ms (默认: 200)\n");
    printf("  --warmup SEC          预热时间 (默认: 10)\n");
    printf("  --timeout MS          单请求超时/ms (默认: 5000)\n");
    printf("  --repeat N            single 模式重复次数 (默认: 1)\n");
    printf("  --verbose             详细输出\n");
    printf("  --help, -h            显示帮助\n\n");
    printf("ramp 模式选项:\n");
    printf("  --ramp-rate N         每秒新建连接数 (默认: 10)\n\n");
    printf("chaos 模式选项:\n");
    printf("  --disconnect-pct N    断连百分比 (默认: 50)\n");
    printf("  --bad-msg-count N     恶意消息数量 (默认: 100)\n");
    printf("  --oversize-bytes N    超大包字节数 (默认: 65536)\n\n");
    printf("示例:\n");
    printf("  # 单连接基线，重复 10 次\n");
    printf("  ./bench_game_client --mode single --repeat 10 --duration 30\n\n");
    printf("  # 渐进加压到 100 连接\n");
    printf("  ./bench_game_client --mode ramp --conn 100 --ramp-rate 10 --duration 60\n\n");
    printf("  # 300 连接稳态 5 分钟\n");
    printf("  ./bench_game_client --mode steady --conn 300 --duration 300 --warmup 30\n\n");
    printf("  # 异常测试\n");
    printf("  ./bench_game_client --mode chaos --conn 100 --disconnect-pct 50\n\n");
    printf("  # 帧同步压测（100 客户端 = 50 个 2 人房间）\n");
    printf("  ./bench_game_client --mode framesync --conn 100 --duration 120 --ramp-rate 10\n\n");
    printf("  # 匹配系统压测\n");
    printf("  ./bench_game_client --mode match --conn 100 --duration 120 --ramp-rate 20\n");
}

// ============================================================
// 入口
// ============================================================
int main(int argc, char* argv[]) {
    // 解析命令行参数
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "--mode" && i + 1 < argc) {
            g_mode = argv[++i];
        } else if (arg == "--server-ip" && i + 1 < argc) {
            g_server_ip = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            g_server_port = static_cast<uint16_t>(atoi(argv[++i]));
        } else if (arg == "--conn" && i + 1 < argc) {
            g_connections = atoi(argv[++i]);
        } else if (arg == "--duration" && i + 1 < argc) {
            g_duration_sec = atoi(argv[++i]);
        } else if (arg == "--think-mean" && i + 1 < argc) {
            g_think_mean_ms = atof(argv[++i]);
        } else if (arg == "--warmup" && i + 1 < argc) {
            g_warmup_sec = atoi(argv[++i]);
        } else if (arg == "--timeout" && i + 1 < argc) {
            g_timeout_ms = atoi(argv[++i]);
        } else if (arg == "--repeat" && i + 1 < argc) {
            g_repeat = atoi(argv[++i]);
        } else if (arg == "--ramp-rate" && i + 1 < argc) {
            g_ramp_rate = atoi(argv[++i]);
        } else if (arg == "--disconnect-pct" && i + 1 < argc) {
            g_chaos_disconnect_pct = atoi(argv[++i]);
        } else if (arg == "--bad-msg-count" && i + 1 < argc) {
            g_chaos_bad_msg_count = atoi(argv[++i]);
        } else if (arg == "--oversize-bytes" && i + 1 < argc) {
            g_chaos_oversize_bytes = atoi(argv[++i]);
        } else if (arg == "--verbose") {
            g_verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            PrintHelp();
            return 0;
        }
    }

    printf("=== TinyRPC 游戏层压测客户端 ===\n");
    printf("模式: %s | 服务端: %s:%u\n", g_mode, g_server_ip, g_server_port);
    fflush(stdout);

    // 根据模式分发
    if (strcmp(g_mode, "single") == 0) {
        RunSingleMode();
    } else if (strcmp(g_mode, "ramp") == 0) {
        RunRampMode();
    } else if (strcmp(g_mode, "steady") == 0) {
        RunSteadyMode();
    } else if (strcmp(g_mode, "chaos") == 0) {
        RunChaosMode();
    } else if (strcmp(g_mode, "framesync") == 0) {
        RunFramesyncMode();
    } else if (strcmp(g_mode, "match") == 0) {
        RunMatchMode();
    } else {
        fprintf(stderr, "未知模式: %s\n", g_mode);
        PrintHelp();
        return 1;
    }

    printf("\n=== 完成 ===\n");
    return 0;
}
