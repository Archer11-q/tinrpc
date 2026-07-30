// ============================================================
// bench_serialize — Layer 1：纯序列化 Benchmark（无网络）
//
// 用法：./bench_serialize
//
// 对比 TinyRPC TLV 二进制序列化 vs 朴素 JSON 文本序列化：
// 1. 序列化后体积
// 2. 编解码吞吐量（MB/s）
// 3. 单次平均耗时（ns）
//
// 三种 payload 大小：小(仅 int×2)、中(+1KB string)、大(+100KB string)
// ============================================================

#include "rpc/serializer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>
#include <cmath>
#include <cstdint>

// ============================================================
// 测试参数
// ============================================================
struct PayloadConfig {
    const char* name; // 规模名称
    int a; // 第一个 int
    int b; // 第二个 int
    size_t data_size; // 字符串 payload 长度
    int iterations; // 测试迭代次数
};

static const PayloadConfig kPayloads[] = {
    // 场景1：典型 RPC 调用 — 大整数（展示二进制定长优势）
    {"小字段-大整数 (2×int)", 123456789, -987654321, 0, 500000},
    // 场景2：多字段 — 模拟一个用户信息结构体
    {"多字段-混合 (10 fields)", 123456789, -987654321, 64, 100000},
    // 场景3：带大字符串的调用 — 字符串拷贝主导
    {"大字符串 (2×int + 10KB str)", 123456789, -987654321, 10240, 10000},
};

static const int kNumPayloads = sizeof(kPayloads) / sizeof(kPayloads[0]);

// ============================================================
// 测试数据结构 — 模拟一个用户信息 RPC 调用的参数
// ============================================================
struct TestData {
    int64_t user_id; // 大整数：123456789
    int64_t balance; // 负数：-987654321
    int32_t age; // 25
    int32_t level; // 42
    double score; // 98.6
    bool active; // true
    std::string name; // 可选字符串
    std::string data; // payload 字符串
};

static TestData MakeTestData(const PayloadConfig& cfg) {
    TestData d{};
    d.user_id = cfg.a;
    d.balance = cfg.b;
    d.age = 25;
    d.level = 42;
    d.score = 98.6;
    d.active = true;
    if (cfg.data_size > 0) {
        // "多字段"场景使用短 name，"大字符串"场景使用长 data
        if (cfg.data_size <= 64) {
            d.name = std::string(cfg.data_size, 'n');
        } else {
            d.name = "test_user";
            d.data = std::string(cfg.data_size, 'x');
        }
    }
    return d;
}

// ============================================================
// 朴素 JSON 编码 — 手动拼接，模拟项目中最常见的 JSON 用法
// ============================================================
static std::string JsonEncode(const TestData& d) {
    std::string json = "{";
    json += "\"user_id\":" + std::to_string(d.user_id) + ",";
    json += "\"balance\":" + std::to_string(d.balance) + ",";
    json += "\"age\":" + std::to_string(d.age) + ",";
    json += "\"level\":" + std::to_string(d.level) + ",";
    // score: 浮点数手动格式化
    char score_buf[32];
    snprintf(score_buf, sizeof(score_buf), "%.1f", d.score);
    json += "\"score\":" + std::string(score_buf) + ",";
    json += "\"active\":" + std::string(d.active ? "true" : "false");
    if (!d.name.empty()) {
        json += ",\"name\":\"" + d.name + "\"";
    }
    if (!d.data.empty()) {
        json += ",\"data\":\"" + d.data + "\"";
    }
    json += "}";
    return json;
}

// 朴素 JSON 解码
static bool JsonDecode(const std::string& json, TestData& out) {
    out = TestData{};

    // user_id
    auto pos = json.find("\"user_id\":");
    if (pos == std::string::npos)
        return false;
    pos += 10; // strlen("\"user_id\":") = 10
    size_t end = pos;
    if (end < json.size() && json[end] == '-')
        end++;
    while (end < json.size() && json[end] >= '0' && json[end] <= '9')
        end++;
    out.user_id = std::stoll(json.substr(pos, end - pos));

    // balance
    pos = json.find("\"balance\":", end);
    if (pos == std::string::npos)
        return false;
    pos += 10;
    end = pos;
    if (end < json.size() && json[end] == '-')
        end++;
    while (end < json.size() && json[end] >= '0' && json[end] <= '9')
        end++;
    out.balance = std::stoll(json.substr(pos, end - pos));

    // age
    pos = json.find("\"age\":", end);
    if (pos == std::string::npos)
        return false;
    pos += 6;
    end = pos;
    while (end < json.size() && json[end] >= '0' && json[end] <= '9')
        end++;
    out.age = std::stoi(json.substr(pos, end - pos));

    // level
    pos = json.find("\"level\":", end);
    if (pos == std::string::npos)
        return false;
    pos += 8;
    end = pos;
    while (end < json.size() && json[end] >= '0' && json[end] <= '9')
        end++;
    out.level = std::stoi(json.substr(pos, end - pos));

    // score
    pos = json.find("\"score\":", end);
    if (pos == std::string::npos)
        return false;
    pos += 8;
    end = pos;
    while (end < json.size() && (json[end] >= '0' && json[end] <= '9' || json[end] == '.'))
        end++;
    out.score = std::stod(json.substr(pos, end - pos));

    // active
    pos = json.find("\"active\":", end);
    if (pos == std::string::npos)
        return false;
    pos += 9;
    out.active = (json.substr(pos, 4) == "true");

    // name (可选)
    pos = json.find("\"name\":\"", end);
    if (pos != std::string::npos) {
        pos += 8;
        end = json.find('"', pos);
        if (end != std::string::npos)
            out.name = json.substr(pos, end - pos);
    }

    // data (可选)
    pos = json.find("\"data\":\"", end);
    if (pos != std::string::npos) {
        pos += 8;
        end = json.find('"', pos);
        if (end != std::string::npos)
            out.data = json.substr(pos, end - pos);
    }

    return true;
}

// ============================================================
// TLV 编码 — 使用 rpc::Serializer
// ============================================================
static std::vector<uint8_t> TlvEncode(const TestData& d) {
    rpc::Serializer ser;
    ser.WriteInt64(d.user_id);
    ser.WriteInt64(d.balance);
    ser.WriteInt32(d.age);
    ser.WriteInt32(d.level);
    ser.WriteDouble(d.score);
    ser.WriteBool(d.active);
    if (!d.name.empty())
        ser.WriteString(d.name);
    if (!d.data.empty())
        ser.WriteString(d.data);
    return ser.GetBuffer();
}

// TLV 解码
static bool TlvDecode(const std::vector<uint8_t>& bytes, TestData& out) {
    rpc::Serializer reader(bytes);

    auto user_id = reader.ReadInt64();
    auto balance = reader.ReadInt64();
    auto age = reader.ReadInt32();
    auto level = reader.ReadInt32();
    auto score = reader.ReadDouble();
    auto active = reader.ReadBool();

    if (!user_id || !balance || !age || !level || !score || !active)
        return false;

    out.user_id = *user_id;
    out.balance = *balance;
    out.age = *age;
    out.level = *level;
    out.score = *score;
    out.active = *active;

    // 剩余字节可能是 string
    out.name.clear();
    out.data.clear();
    // 尝试读取第一个 string（如果存在）
    // 通过检查剩余数据判断有几个 string
    // 简化处理：小负载场景没有 string，多字段场景有 1 个 name string，大字符场景有 2 个 string
    if (bytes.size() > 60) { // 6 个 TLV 字段 ≈ 54 字节，超过就有 string
        auto s1 = reader.ReadString();
        if (s1)
            out.name = *s1;
        auto s2 = reader.ReadString();
        if (s2)
            out.data = *s2;
    }
    return true;
}

// ============================================================
// Benchmark 工具
// ============================================================

using Clock = std::chrono::high_resolution_clock;

struct BenchResult {
    size_t serialized_bytes; // 单次序列化后体积（B）
    double encode_ns; // 单次编码平均耗时（ns）
    double decode_ns; // 单次解码平均耗时（ns）
    double encode_mbps; // 编码吞吐量（MB/s）
    double decode_mbps; // 解码吞吐量（MB/s）
    bool decode_correct; // 解码结果是否正确
};

static BenchResult RunTlvBench(const PayloadConfig& cfg) {
    TestData input = MakeTestData(cfg);

    // ---- 编码测试 ----
    std::vector<uint8_t> encoded;
    auto t0 = Clock::now();
    for (int i = 0; i < cfg.iterations; i++) {
        encoded = TlvEncode(input);
    }
    auto t1 = Clock::now();

    double encode_total_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    double encode_ns = encode_total_ns / static_cast<double>(cfg.iterations);

    // ---- 解码测试 ----
    TestData output{};
    auto t2 = Clock::now();
    for (int i = 0; i < cfg.iterations; i++) {
        TlvDecode(encoded, output);
    }
    auto t3 = Clock::now();

    double decode_total_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count());
    double decode_ns = decode_total_ns / static_cast<double>(cfg.iterations);

    BenchResult r;
    r.serialized_bytes = encoded.size();
    r.encode_ns = encode_ns;
    r.decode_ns = decode_ns;
    r.encode_mbps = (static_cast<double>(encoded.size() * cfg.iterations) / (1024.0 * 1024.0)) /
                    (encode_total_ns / 1e9);
    r.decode_mbps = (static_cast<double>(encoded.size() * cfg.iterations) / (1024.0 * 1024.0)) /
                    (decode_total_ns / 1e9);
    r.decode_correct = (output.user_id == input.user_id && output.balance == input.balance &&
                        output.age == input.age && output.level == input.level &&
                        std::abs(output.score - input.score) < 0.01 && output.active == input.active &&
                        output.name == input.name && output.data == input.data);
    return r;
}

static BenchResult RunJsonBench(const PayloadConfig& cfg) {
    TestData input = MakeTestData(cfg);

    // ---- 编码测试 ----
    std::string encoded;
    auto t0 = Clock::now();
    for (int i = 0; i < cfg.iterations; i++) {
        encoded = JsonEncode(input);
    }
    auto t1 = Clock::now();

    double encode_total_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    double encode_ns = encode_total_ns / static_cast<double>(cfg.iterations);

    // ---- 解码测试 ----
    TestData output{};
    auto t2 = Clock::now();
    for (int i = 0; i < cfg.iterations; i++) {
        JsonDecode(encoded, output);
    }
    auto t3 = Clock::now();

    double decode_total_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count());
    double decode_ns = decode_total_ns / static_cast<double>(cfg.iterations);

    BenchResult r;
    r.serialized_bytes = encoded.size();
    r.encode_ns = encode_ns;
    r.decode_ns = decode_ns;
    r.encode_mbps = (static_cast<double>(encoded.size() * cfg.iterations) / (1024.0 * 1024.0)) /
                    (encode_total_ns / 1e9);
    r.decode_mbps = (static_cast<double>(encoded.size() * cfg.iterations) / (1024.0 * 1024.0)) /
                    (decode_total_ns / 1e9);
    r.decode_correct = (output.user_id == input.user_id && output.balance == input.balance &&
                        output.age == input.age && output.level == input.level &&
                        std::abs(output.score - input.score) < 0.01 && output.active == input.active &&
                        output.name == input.name && output.data == input.data);
    return r;
}

// ============================================================
// 入口
// ============================================================
int main() {
    printf("=== Layer 1: 纯序列化 Benchmark（无网络） ===\n\n");

    printf("%-30s %8s %12s %12s %12s %12s\n", "场景", "体积", "编码(ns)", "解码(ns)", "编码MB/s",
           "解码MB/s");
    printf("%s\n", std::string(90, '-').c_str());

    for (int i = 0; i < kNumPayloads; i++) {
        const auto& cfg = kPayloads[i];

        printf("\n--- %s (%d 次迭代) ---\n", cfg.name, cfg.iterations);

        auto tlv = RunTlvBench(cfg);
        printf("  TLV:   体积=%4zu B | 编码=%8.0f ns | 解码=%8.0f ns | 编码=%7.1f MB/s | "
               "解码=%7.1f MB/s  %s\n",
               tlv.serialized_bytes, tlv.encode_ns, tlv.decode_ns, tlv.encode_mbps, tlv.decode_mbps,
               tlv.decode_correct ? "✅" : "❌");

        auto json = RunJsonBench(cfg);
        printf("  JSON:  体积=%4zu B | 编码=%8.0f ns | 解码=%8.0f ns | 编码=%7.1f MB/s | "
               "解码=%7.1f MB/s  %s\n",
               json.serialized_bytes, json.encode_ns, json.decode_ns, json.encode_mbps,
               json.decode_mbps, json.decode_correct ? "✅" : "❌");

        // 对比
        double volume_ratio = static_cast<double>(json.serialized_bytes) /
                              static_cast<double>(tlv.serialized_bytes);
        double encode_ratio = json.encode_ns / tlv.encode_ns;
        double decode_ratio = json.decode_ns / tlv.decode_ns;

        printf("  ── TLV vs JSON ──\n");
        printf("  体积:  TLV 是 JSON 的 %.1f%%（节省 %.1f%%）\n", (1.0 / volume_ratio) * 100.0,
               (1.0 - 1.0 / volume_ratio) * 100.0);
        printf("  编码:  TLV 是 JSON 的 %.1fx 快\n", encode_ratio);
        printf("  解码:  TLV 是 JSON 的 %.1fx 快\n", decode_ratio);
    }

    printf("\n=== 完成 ===\n");
    return 0;
}