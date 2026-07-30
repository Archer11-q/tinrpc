// ============================================================
// packet_frag_test — 协议帧分片分析客户端
//
// 用法：
//   ./packet_frag_test --port 8080 --scenario all
//
// 刻意制造粘包/拆包场景，验证 Buffer::TryPopFrame 的
// 帧边界检测逻辑是否正确。
//
// 帧格式（参照 common.h）：
//   ┌────────┬──────────┬──────────┬──────────┬──────────┬──────────┬──────────┐
//   │ 魔数    │ 总长度    │ 请求 ID   │ 消息类型   │ 方法名长度 │ 方法名     │ body     │
//   │ 2 bytes │ 4 bytes   │ 4 bytes   │ 1 byte    │ 2 bytes   │ N bytes   │ M bytes  │
//   └────────┴──────────┴──────────┴──────────┴──────────┴──────────┴──────────┘
//
// Buffer 分片处理逻辑（参照 buffer.cpp）：
//   1. 检查缓冲区 ≥ 6 字节（魔数 + 总长度）
//   2. 校验魔数 = 0xBABE
//   3. 读取总长度 total_len
//   4. 校验 total_len 在 [kFrameHeaderSize, kMaxFrameSize] 范围内
//   5. 缓冲区 < total_len → 等待更多数据（拆包处理）
//   6. 缓冲区 ≥ total_len → 切出一帧，消费对应字节（粘包处理）
// ============================================================

#include "rpc/protocol.h"
#include "rpc/buffer.h"
#include "rpc/serializer.h"
#include "rpc/common.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <thread>
#include <chrono>

// ============================================================
// 命令行参数
// ============================================================
static uint16_t g_port = 8080;
static const char* g_scenario = "all";

// ============================================================
// 辅助：构造一个 Add 请求帧
// body = Serializer(a, b)，帧类型 = Request
// ============================================================
static std::vector<uint8_t> MakeAddFrame(uint32_t request_id, int32_t a, int32_t b) {
    rpc::Serializer ser;
    ser.WriteInt32(a);
    ser.WriteInt32(b);
    return rpc::ProtocolFrame::Encode(request_id, rpc::MessageType::Request, "Add", ser.GetBuffer());
}

// ============================================================
// 辅助：从响应帧的 body 中解码出 int32 结果
// ============================================================
static int DecodeResponseResult(const rpc::Frame& frame) {
    rpc::Serializer ser(frame.body);
    auto val = ser.ReadInt32();
    return val.value_or(-99999);
}

// ============================================================
// 辅助：连接到服务端
// ============================================================
static int ConnectToServer() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "[错误] socket() 失败\n");
        return -1;
    }

    // 禁用 Nagle，减少内核合并小包的概率（对拆包场景关键）
    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(g_port);
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) <= 0) {
        fprintf(stderr, "[错误] inet_pton() 失败\n");
        close(fd);
        return -1;
    }

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        fprintf(stderr, "[错误] connect() 失败（服务端是否启动？）\n");
        close(fd);
        return -1;
    }

    return fd;
}

// ============================================================
// 辅助：接收响应帧，打印详细的分片过程
//
// 模拟服务端的接收路径：
//   recv() → Buffer::Append() → Buffer::TryPopFrame() → ProtocolFrame::Decode()
//
// 返回收到的所有 Response 帧
// ============================================================
static std::vector<rpc::Frame> ReceiveResponses(int fd, int expected_count, int timeout_ms) {
    rpc::Buffer buf;
    std::vector<rpc::Frame> responses;
    char recv_buf[4096];

    // 设置接收超时
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    printf("  接收过程:\n");
    int recv_seq = 0;

    while (static_cast<int>(responses.size()) < expected_count) {
        ssize_t n = recv(fd, recv_buf, sizeof(recv_buf), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                printf("    recv #%d: 超时（已等 %dms）\n", ++recv_seq, timeout_ms);
            } else {
                printf("    recv #%d: 错误 (errno=%d: %s)\n", ++recv_seq, errno, strerror(errno));
            }
            break;
        }
        if (n == 0) {
            printf("    recv #%d: 连接关闭（对端断开）\n", ++recv_seq);
            break;
        }

        recv_seq++;
        printf("    recv #%d: %zd 字节 → Buffer.Append()", recv_seq, n);

        // 追加到 Buffer（模拟服务端 Connection::OnRead 的逻辑）
        buf.Append(reinterpret_cast<const uint8_t*>(recv_buf), static_cast<size_t>(n));
        printf(" | Buffer 累积: %zu 字节\n", buf.Size());

        // 循环弹出完整帧（模拟 TryPopFrame 的粘包/拆包处理）
        while (true) {
            auto raw = buf.TryPopFrame();
            if (!raw)
                break; // 不够一帧 → 等待更多数据

            auto frame = rpc::ProtocolFrame::Decode(*raw);
            if (!frame) {
                printf("      TryPopFrame → %zu 字节 [解码失败，丢弃]\n", raw->size());
                continue;
            }

            if (frame->msg_type == rpc::MessageType::Response) {
                int result = DecodeResponseResult(*frame);
                printf("      TryPopFrame → %zu 字节 (完整帧: request_id=%u, result=%d) | Buffer "
                       "剩余: %zu 字节\n",
                       raw->size(), frame->request_id, result, buf.Size());
                responses.push_back(std::move(*frame));
            } else {
                printf("      TryPopFrame → %zu 字节 (非 Response 帧, msg_type=%d, 跳过)\n",
                       raw->size(), static_cast<int>(frame->msg_type));
            }
        }
    }

    printf("  共收到 %zu 个响应帧（期望 %d 个）\n", responses.size(), expected_count);
    return responses;
}

// ============================================================
// 场景 1：正常发送（对照组）
// 3 帧逐帧 send()，无粘包无拆包
// ============================================================
static bool RunNormal() {
    printf("\n=== 场景 1: normal（对照组：3 帧逐帧发送）===\n");

    int fd = ConnectToServer();
    if (fd < 0)
        return false;

    auto f1 = MakeAddFrame(1, 3, 5); // Add(3,5) = 8
    auto f2 = MakeAddFrame(2, 10, 20); // Add(10,20) = 30
    auto f3 = MakeAddFrame(3, 7, 2); // Add(7,2) = 9

    printf("  发送: 3 帧，逐帧 send()\n");
    printf("    帧 #1: %zu 字节 (request_id=1, Add(3,5))\n", f1.size());
    printf("    帧 #2: %zu 字节 (request_id=2, Add(10,20))\n", f2.size());
    printf("    帧 #3: %zu 字节 (request_id=3, Add(7,2))\n", f3.size());

    send(fd, f1.data(), f1.size(), MSG_NOSIGNAL);
    send(fd, f2.data(), f2.size(), MSG_NOSIGNAL);
    send(fd, f3.data(), f3.size(), MSG_NOSIGNAL);

    auto responses = ReceiveResponses(fd, 3, 2000);
    close(fd);

    // 验证
    bool ok = (responses.size() == 3) && (DecodeResponseResult(responses[0]) == 8) &&
              (DecodeResponseResult(responses[1]) == 30) &&
              (DecodeResponseResult(responses[2]) == 9);
    printf("  验证: %s\n\n", ok ? "✓ 通过" : "✗ 失败");
    return ok;
}

// ============================================================
// 场景 2：粘包 — 2 帧合并一次 send()
// ============================================================
static bool RunSticky2() {
    printf("\n=== 场景 2: sticky_2（粘包：2 帧合并发送）===\n");

    int fd = ConnectToServer();
    if (fd < 0)
        return false;

    auto f1 = MakeAddFrame(1, 3, 5);
    auto f2 = MakeAddFrame(2, 10, 20);

    // 拼接为一个 buffer
    std::vector<uint8_t> combined;
    combined.insert(combined.end(), f1.begin(), f1.end());
    combined.insert(combined.end(), f2.begin(), f2.end());

    printf("  发送: 帧 #1(%zu 字节) + 帧 #2(%zu 字节) 拼接 = %zu 字节，一次 send()\n", f1.size(),
           f2.size(), combined.size());
    printf("  预期: 服务端一次 recv 收到粘包的 2 帧，Buffer.TryPopFrame 逐帧弹出\n");

    send(fd, combined.data(), combined.size(), MSG_NOSIGNAL);

    auto responses = ReceiveResponses(fd, 2, 2000);
    close(fd);

    bool ok = (responses.size() == 2) && (DecodeResponseResult(responses[0]) == 8) &&
              (DecodeResponseResult(responses[1]) == 30);
    printf("  验证: %s\n\n", ok ? "✓ 通过" : "✗ 失败");
    return ok;
}

// ============================================================
// 场景 3：粘包 — 5 帧合并一次 send()
// ============================================================
static bool RunSticky5() {
    printf("\n=== 场景 3: sticky_5（粘包：5 帧合并发送）===\n");

    int fd = ConnectToServer();
    if (fd < 0)
        return false;

    std::vector<uint8_t> combined;
    int expected_results[5];

    for (int i = 0; i < 5; i++) {
        int a = i + 1;
        int b = i * 2;
        auto f = MakeAddFrame(static_cast<uint32_t>(i + 1), a, b);
        expected_results[i] = a + b;
        combined.insert(combined.end(), f.begin(), f.end());
        printf("  帧 #%d: %zu 字节 (request_id=%d, Add(%d,%d)=%d)\n", i + 1, f.size(), i + 1, a, b,
               expected_results[i]);
    }

    printf("  发送: 5 帧拼接 = %zu 字节，一次 send()\n", combined.size());

    send(fd, combined.data(), combined.size(), MSG_NOSIGNAL);

    auto responses = ReceiveResponses(fd, 5, 2000);
    close(fd);

    bool ok = (responses.size() == 5);
    for (int i = 0; i < 5 && ok; i++) {
        if (DecodeResponseResult(responses[static_cast<size_t>(i)]) != expected_results[i])
            ok = false;
    }
    printf("  验证: %s\n\n", ok ? "✓ 通过" : "✗ 失败");
    return ok;
}

// ============================================================
// 场景 4：拆包 — 1 帧分两次发送（前一半 + 后一半）
// ============================================================
static bool RunSplitHalf() {
    printf("\n=== 场景 4: split_half（拆包：1 帧分两次发送）===\n");

    int fd = ConnectToServer();
    if (fd < 0)
        return false;

    auto frame = MakeAddFrame(1, 3, 5);
    size_t half = frame.size() / 2;

    printf("  发送: 帧总长 %zu 字节，分 2 次发送\n", frame.size());
    printf("    第 1 次: 前 %zu 字节（帧头不完整，Buffer 应等待）\n", half);
    printf("    第 2 次: 后 %zu 字节（补齐后 TryPopFrame 弹出完整帧）\n", frame.size() - half);

    // 第一次：只发送前半部分
    send(fd, frame.data(), half, MSG_NOSIGNAL);
    printf("    已发送前 %zu 字节，等待 10ms...\n", half);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // 第二次：发送后半部分
    send(fd, frame.data() + half, frame.size() - half, MSG_NOSIGNAL);
    printf("    已发送后 %zu 字节\n", frame.size() - half);

    auto responses = ReceiveResponses(fd, 1, 2000);
    close(fd);

    bool ok = (responses.size() == 1) && (DecodeResponseResult(responses[0]) == 8);
    printf("  验证: %s\n\n", ok ? "✓ 通过" : "✗ 失败");
    return ok;
}

// ============================================================
// 场景 5：极端拆包 — 1 帧逐字节发送
// ============================================================
static bool RunSplitByte() {
    printf("\n=== 场景 5: split_byte（极端拆包：逐字节发送）===\n");

    int fd = ConnectToServer();
    if (fd < 0)
        return false;

    auto frame = MakeAddFrame(1, 3, 5);

    printf("  发送: 帧总长 %zu 字节，逐字节 send()\n", frame.size());
    printf("  预期: 每次 recv 收到少量字节，Buffer 逐步累积，够一帧后才弹出\n");

    for (size_t i = 0; i < frame.size(); i++) {
        send(fd, frame.data() + i, 1, MSG_NOSIGNAL);
        // 每字节间隔 1ms，给 TCP 栈时间将每个字节作为独立包发送
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    printf("  已发送全部 %zu 字节\n", frame.size());

    auto responses = ReceiveResponses(fd, 1, 3000);
    close(fd);

    bool ok = (responses.size() == 1) && (DecodeResponseResult(responses[0]) == 8);
    printf("  验证: %s\n\n", ok ? "✓ 通过" : "✗ 失败");
    return ok;
}

// ============================================================
// 场景 6：粘包 + 拆包组合
//   帧 A 分两半：先发前一半，再把后一半和完整帧 B 粘在一起发
// ============================================================
static bool RunCombo() {
    printf("\n=== 场景 6: combo（粘包+拆包组合）===\n");

    int fd = ConnectToServer();
    if (fd < 0)
        return false;

    auto fA = MakeAddFrame(1, 1, 2); // Add(1,2) = 3
    auto fB = MakeAddFrame(2, 10, 5); // Add(10,5) = 15

    size_t halfA = fA.size() / 2;

    printf("  帧 A: %zu 字节 (request_id=1, Add(1,2)=3)\n", fA.size());
    printf("  帧 B: %zu 字节 (request_id=2, Add(10,5)=15)\n", fB.size());
    printf("  发送策略:\n");
    printf("    第 1 次: 帧 A 前 %zu 字节（不完整，Buffer 等待）\n", halfA);
    printf("    第 2 次: 帧 A 后 %zu 字节 + 帧 B 全部 %zu 字节 = %zu 字节（粘包）\n",
           fA.size() - halfA, fB.size(), fA.size() - halfA + fB.size());

    // 第一次：帧 A 的前半
    send(fd, fA.data(), halfA, MSG_NOSIGNAL);
    printf("    已发送帧 A 前 %zu 字节，等待 10ms...\n", halfA);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // 第二次：帧 A 的后半 + 完整的帧 B（粘在一起）
    std::vector<uint8_t> combined;
    combined.insert(combined.end(), fA.begin() + static_cast<ptrdiff_t>(halfA), fA.end());
    combined.insert(combined.end(), fB.begin(), fB.end());
    send(fd, combined.data(), combined.size(), MSG_NOSIGNAL);
    printf("    已发送 (帧A后半 + 帧B) 共 %zu 字节\n", combined.size());

    auto responses = ReceiveResponses(fd, 2, 2000);
    close(fd);

    bool ok = (responses.size() == 2) && (DecodeResponseResult(responses[0]) == 3) &&
              (DecodeResponseResult(responses[1]) == 15);
    printf("  验证: %s\n\n", ok ? "✓ 通过" : "✗ 失败");
    return ok;
}

// ============================================================
// 场景 7：随机分片 — 3 帧打散为 1~15 字节随机块发送
// ============================================================
static bool RunRandomChunks() {
    printf("\n=== 场景 7: random_chunks（随机分片：3 帧切割为随机大小块发送）===\n");

    int fd = ConnectToServer();
    if (fd < 0)
        return false;

    auto f1 = MakeAddFrame(1, 3, 5);
    auto f2 = MakeAddFrame(2, 8, 2);
    auto f3 = MakeAddFrame(3, 1, 9);

    // 3 帧拼接为一个连续 buffer
    std::vector<uint8_t> combined;
    combined.insert(combined.end(), f1.begin(), f1.end());
    combined.insert(combined.end(), f2.begin(), f2.end());
    combined.insert(combined.end(), f3.begin(), f3.end());

    printf("  帧 #1: %zu 字节 (request_id=1, Add(3,5)=8)\n", f1.size());
    printf("  帧 #2: %zu 字节 (request_id=2, Add(8,2)=10)\n", f2.size());
    printf("  帧 #3: %zu 字节 (request_id=3, Add(1,9)=10)\n", f3.size());
    printf("  拼接后: %zu 字节\n", combined.size());

    // 使用确定性伪随机将拼接 buffer 切割为 1~15 字节的块
    std::vector<std::vector<uint8_t>> chunks;
    size_t offset = 0;
    int seed = 42;
    while (offset < combined.size()) {
        seed = (seed * 1103515245 + 12345) & 0x7fffffff;
        size_t chunk_size = 1 + static_cast<size_t>(seed % 15); // 1~15 字节
        if (offset + chunk_size > combined.size())
            chunk_size = combined.size() - offset;
        chunks.emplace_back(combined.begin() + static_cast<ptrdiff_t>(offset),
                            combined.begin() + static_cast<ptrdiff_t>(offset + chunk_size));
        offset += chunk_size;
    }

    printf("  切割为 %zu 个随机块（1~15 字节/块），逐块 send()\n", chunks.size());

    for (size_t i = 0; i < chunks.size(); i++) {
        send(fd, chunks[i].data(), chunks[i].size(), MSG_NOSIGNAL);
        // 每块间隔 2ms
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    printf("  已发送全部 %zu 块\n", chunks.size());

    auto responses = ReceiveResponses(fd, 3, 3000);
    close(fd);

    bool ok = (responses.size() == 3) && (DecodeResponseResult(responses[0]) == 8) &&
              (DecodeResponseResult(responses[1]) == 10) &&
              (DecodeResponseResult(responses[2]) == 10);
    printf("  验证: %s\n\n", ok ? "✓ 通过" : "✗ 失败");
    return ok;
}

// ============================================================
// 入口
// ============================================================
int main(int argc, char* argv[]) {
    // 命令行解析
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            g_port = static_cast<uint16_t>(atoi(argv[++i]));
        } else if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
            g_scenario = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("用法: packet_frag_test [选项]\n");
            printf("  --port PORT       服务端端口 (默认 8080)\n");
            printf("  --scenario NAME   测试场景 (默认 all)\n");
            printf("\n场景列表:\n");
            printf("  all           运行全部 7 个场景\n");
            printf("  normal        对照组：3 帧逐帧发送\n");
            printf("  sticky_2      粘包：2 帧合并一次 send()\n");
            printf("  sticky_5      粘包：5 帧合并一次 send()\n");
            printf("  split_half    拆包：1 帧分两次发送\n");
            printf("  split_byte    极端拆包：逐字节发送\n");
            printf("  combo         粘包+拆包组合\n");
            printf("  random_chunks 随机分片\n");
            return 0;
        }
    }

    printf("╔══════════════════════════════════════════╗\n");
    printf("║   协议帧分片分析 — Buffer 粘包/拆包验证  ║\n");
    printf("╠══════════════════════════════════════════╣\n");
    printf("║  帧头: 13 字节 (魔数 0xBABE + 总长度 +   ║\n");
    printf("║        request_id + msg_type + 方法名长度)║\n");
    printf("║  最大帧: 10 MB                           ║\n");
    printf("║  端口: %-5u                              ║\n", g_port);
    printf("╚══════════════════════════════════════════╝\n");

    // 先检查服务端是否可达
    {
        int test_fd = ConnectToServer();
        if (test_fd < 0) {
            fprintf(stderr, "\n[错误] 无法连接到 127.0.0.1:%u，请先启动 bench_server --mode rpc\n",
                    g_port);
            return 1;
        }
        close(test_fd);
        printf("[预检] 服务端连接成功\n");
    }

    struct ScenarioEntry {
        const char* name;
        bool (*fn)();
    };

    ScenarioEntry all_scenarios[] = {
        {"normal", RunNormal},
        {"sticky_2", RunSticky2},
        {"sticky_5", RunSticky5},
        {"split_half", RunSplitHalf},
        {"split_byte", RunSplitByte},
        {"combo", RunCombo},
        {"random_chunks", RunRandomChunks},
    };

    int passed = 0;
    int failed = 0;
    int total = 0;

    for (auto& entry : all_scenarios) {
        if (strcmp(g_scenario, "all") != 0 && strcmp(g_scenario, entry.name) != 0)
            continue;

        total++;
        bool result = entry.fn();
        if (result)
            passed++;
        else
            failed++;
    }

    // 汇总报告
    printf("╔══════════════════════════════════════════╗\n");
    printf("║              测试汇总报告                ║\n");
    printf("╠══════════════════════════════════════════╣\n");
    printf("║  总场景数:  %2d                          ║\n", total);
    printf("║  通过:      %2d                          ║\n", passed);
    printf("║  失败:      %2d                          ║\n", failed);
    printf("╚══════════════════════════════════════════╝\n");

    return failed > 0 ? 1 : 0;
}
