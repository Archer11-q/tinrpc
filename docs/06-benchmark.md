# 06 — Benchmark：RPC vs HTTP+JSON 性能对比

---

## 一、这一层解决什么问题？

### 当前状态：框架能跑，但"快不快"没人知道

v0.5 完成了 RPC 调用的完整闭环——客户端 `stub->Add(3,5)`，服务端 Dispatch 分发，结果通过 `future.get()` 返回。44 项测试全部通过。

**但有一个问题没回答：这套自建协议到底比通用的 HTTP+JSON 方案好在哪里？**

```
面试官：你为什么不用 gRPC？不用 HTTP+JSON？
你：因为我自实现的 TLV 协议更轻量。
面试官：用什么数据证明？
你：……（没有数据）
```

**Benchmark 层的职责**：用可量化的数据回答"为什么 TinyRPC 更快"这个问题——不是"我觉得更快"，而是"数据说更快"。

### 不止是写个压测脚本

v0.6 要做的不是简单地跑几个 `ab` 命令。它需要：

1. **实现一个对等的 HTTP+JSON 对照服务端**，用相同的业务逻辑，让对比公平
2. **实现一个统一的压测工具**，用相同的负载模式同时压测 RPC 和 HTTP+JSON 两端
3. **收集多维度指标**：QPS、延迟分布（avg/p50/p95/p99）、序列化后体积、CPU 使用率
4. **输出可复现的 benchmark 报告**，别人跑相同命令能得到相似结论

---

## 二、为什么要做性能对比？

### 2.1 每种设计选择都有成本

TinyRPC 的每一层选择，背后都有性能理由：

| 设计选择 | 对标方案 | 理论优势 |
|----------|----------|----------|
| TLV 二进制序列化 | JSON 文本序列化 | 体积更小，解析更快（无字符串解析开销） |
| 自定义帧协议（13B 帧头） | HTTP/1.1 文本头 | 头部紧凑，解析只需几次内存读取 |
| epoll + Reactor | 多线程 accept | 单线程处理大量连接，减少上下文切换 |
| 二进制协议直接解析 | HTTP 解析 + JSON 解析 | 两次解析变一次 |

但这些是**理论**。Benchmark 的任务是把理论变成数字。

### 2.2 数据结构体积对比（理论推算）

以调用 `Add(3, 5)` 为例：

**TinyRPC（TLV 协议）**：
```
帧头（13B）+ 方法名 TLV（1B type + 4B len + "Add" 3B = 8B）
  + 参数1 TLV（1B type + 4B len + 4B value = 9B）
  + 参数2 TLV（1B type + 4B len + 4B value = 9B）
= 13 + 8 + 9 + 9 = 39 字节
```

**HTTP+JSON**：
```http
POST /rpc/Add HTTP/1.1\r\n
Host: localhost:8080\r\n
Content-Type: application/json\r\n
Content-Length: 24\r\n
\r\n
{"a":3,"b":5,"method":"Add"}
```
≈ 120+ 字节（仅请求，不含响应）

**体量差距约 3 倍**。但如果消息体本身很大（比如传一个 1MB 的文件），帧头的差距就不重要了。所以 benchmark 要覆盖**不同大小的消息体**。

### 2.3 哪些场景下自建协议优势最明显

- **小消息高频调用**（如微服务间的 RPC 调用）：帧头开销占比大，自建协议优势明显
- **大消息低频调用**（如文件上传）：帧头占比小，差距缩小
- **长连接场景**：HTTP/1.1 的 Keep-Alive 可以复用连接，但每次请求仍有 HTTP 头开销；TinyRPC 的帧协议天然支持复用
- **短连接场景**：HTTP 的 TCP 三次握手 + 四次挥手开销更显著

---

## 三、Benchmark 指标体系

### 3.1 吞吐量（Throughput / QPS）

**定义**：每秒能处理多少个请求。

```
QPS = 总请求数 / 总耗时（秒）
```

这是"快不快"的最直接指标。但不是唯一的——只看 QPS 会被误导：

```
场景 A：服务端收到请求直接 return，QPS = 100000
场景 B：服务端做 10ms 计算后 return，QPS = 100

QPS 高 1000 倍就代表框架好 1000 倍？不。瓶颈在业务逻辑，不在框架。
```

所以 benchmark 要设置**不同业务耗时**的场景：空操作（测框架开销）、轻计算（模拟真实 RPC 调用）。

### 3.2 延迟（Latency）

**定义**：单个请求从发出到收到响应的完整时间。

不只看平均值——平均值掩盖了"最差情况"：

```
延迟序列：1ms, 1ms, 1ms, 1ms, 100ms
平均值：(1+1+1+1+100)/5 = 20.8ms  ← "看起来还行"
实际：20% 的请求等了 100ms！       ← 长尾延迟（tail latency）才是用户体验杀手
```

**需要收集的分位数**：
- **avg**：平均值，反映总体水平
- **p50（中位数）**：一半请求的延迟低于此值
- **p95**：95% 的请求延迟低于此值，反映"大部分用户"的体验
- **p99**：99% 的请求延迟低于此值，反映长尾情况

### 3.3 序列化负载（Serialization Overhead）

**定义**：序列化后的数据体积。

对比维度：
- 请求体积（request body + headers）
- 响应体积（response body + headers）
- 请求+响应总体积（一次完整 RPC 调用的网络传输量）

### 3.4 CPU 使用率

**定义**：在相同 QPS 下，服务端进程的 CPU 占用百分比。

这是"省不省资源"的指标。如果 TinyRPC 能在相同 QPS 下用更少的 CPU，就意味着同样的机器能承载更多请求。

收集方式：`/proc/[pid]/stat`（Linux）或 `GetProcessTimes()`（Windows），定期采样取平均。

### 3.5 为什么这些指标就够了

不做内存占用对比：两者都是短生命周期对象，稳态内存占用差异不大。
不做网络带宽对比：QPS × 单请求体积 = 带宽，可以自行推算。
不做 GC 暂停对比：C++ 手动管理内存，没有 GC。

---

## 四、压测方法学

### 4.1 环境控制

Benchmark 最容易犯的错误是**环境不一致**。同一段代码，在"刚开机"和"开了 20 个 Chrome 标签页"时跑出的结果完全不一样。

**控制措施**：
- 客户端和服务端跑在同一台机器上（避免网络抖动）
- 使用 `127.0.0.1` loopback（不走物理网卡）
- 固定 CPU 频率（关闭睿频/动态调频，`cpupower frequency-set`）
- 跑之前先做 warmup（预热），排除冷启动的影响（CPU 缓存、分支预测器、JIT 编译）
- 每组测试跑多次取平均值
- 关闭不必要的后台进程

### 4.2 Warmup（预热）为什么必要

```
第一次跑：QPS = 50000  ← CPU 缓存是冷的，分支预测器还没学习
第二次跑：QPS = 55000  ← 缓存开始命中
第三次跑：QPS = 58000  ← 稳态
```

Benchmark 报告应该记录的是**稳态性能**（第三次及之后的数据），但也要注明 warmup 前后的差异——这是面试时可以聊的"工程细节"。

### 4.3 并发模型

压测客户端需要模拟真实场景的并发负载：

```
                  ┌─────────────────┐
    主控线程      │  创建 N 个线程   │
                  │  每个线程发送    │
                  │  M 个请求        │
                  │  统计延迟        │
                  │  汇总到主线程    │
                  └─────────────────┘
```

**参数**：
- `N`（并发线程数）：模拟同时有多少个客户端在调用
- `M`（每线程请求数）：总请求数 = N × M

测试不同并发度下的表现：
- N=1：单客户端串行，测单次延迟
- N=4, 8, 16：逐步加压，找 QPS 饱和点
- N=64, 128：超饱和压力，测框架在过载下的行为

### 4.4 业务场景设计

至少两个场景：

**场景 A — 空操作（Echo）**：
- 服务端收到请求后立即返回空响应
- 目的：测量**框架本身的固定开销**（序列化+网络+分发）
- 这是框架性能的上限——所有优化都体现在这里

**场景 B — 轻计算（Add）**：
- 真实的 Add(a, b) 业务逻辑
- 目的：测量**框架开销在真实场景中的占比**
- 如果业务逻辑耗时 >> 框架开销，优化框架意义就不大

---

## 五、HTTP+JSON 对照组的实现方案

### 5.1 设计原则

对照组的业务逻辑必须和 TinyRPC **完全相同**——同样的 Add/Sub 函数，同样的错误处理。唯一不同的是通信协议和序列化格式。

```
TinyRPC 服务端                      HTTP+JSON 服务端
┌──────────────────┐              ┌──────────────────┐
│ TLV 帧协议        │              │ HTTP/1.1 文本协议  │
│ Binary Serializer │              │ JSON 序列化       │
├──────────────────┤              ├──────────────────┤
│     Add() / Sub()   ← 相同代码 →     Add() / Sub()  │
├──────────────────┤              ├──────────────────┤
│ epoll + Reactor  │              │ epoll + Reactor  │
└──────────────────┘              └──────────────────┘
          ↑ 不同 ↑                       ↑ 相同 ↑
```

**关键**：TinyRPC 的网络层（epoll + Reactor）已经实现好了。HTTP+JSON 服务端可以**复用同一套网络层代码**——在 `FrameCallback` 中做 HTTP 解析和 JSON 序列化即可。

### 5.2 自实现 vs 第三方库

这是个重要的设计决策：

| 方案 | 优点 | 缺点 |
|------|------|------|
| 自实现 HTTP/1.1 + JSON | 零外部依赖原则一致，可控 | 工作量大，可能存在实现缺陷 |
| 引入第三方库（nlohmann/json + httplib） | 成熟的实现，对比更"公正" | 引入外部依赖 |

**建议**：自实现一个**最小化的 HTTP/1.1 解析器**和**简单的 JSON 序列化**，只覆盖 benchmark 需要的部分（POST 请求解析、JSON 对象序列化/反序列化）。

原因：
- 保持"零外部依赖"原则——如果你用第三方 JSON 库、对方用 Protobuf，那差距到底是"二进制 vs 文本"还是"好库 vs 烂库"？说不清楚
- 自实现的"朴素 JSON"更接近很多项目中实际的手工 JSON 拼接（没有 simdjson 那种优化）
- HTTP/1.1 只需要解析请求行 + 几个必要头部 + Content-Length，不需要完整实现 RFC 2616
- 面试可以聊"为什么我不需要完整的 HTTP 解析器"

### 5.3 最小化 HTTP/1.1 解析

只需要解析这些：

```
POST /rpc/Add HTTP/1.1\r\n          ← 请求行：METHOD + PATH
Host: localhost:8080\r\n             ← 跳过
Content-Type: application/json\r\n   ← 跳过
Content-Length: 24\r\n               ← 需要：读多少字节的 body
\r\n
{"a":3,"b":5}                        ← JSON body
```

实现思路：
- 按 `\r\n` 分割请求头
- 找到 `Content-Length: N` 行，提取 N
- 跳过 `\r\n\r\n` 后读取 N 字节作为 body
- 解析 JSON body（见下节）

### 5.4 最小化 JSON 序列化

只需要支持 benchmark 涉及的类型：

```cpp
// 序列化（C++ → JSON 字符串）
std::string JsonEncode(const Request& req) {
    // {"a":3,"b":5}  或  {"a":3,"b":5,"method":"Add"}
}

// 反序列化（JSON 字符串 → C++）
std::optional<Request> JsonDecode(const std::string& json) {
    // 朴素实现：手动解析 "a":3,"b":5 这种格式
    // 不处理嵌套、数组、转义字符、Unicode
}
```

**不需要实现**：嵌套对象、数组、字符串转义、Unicode、浮点数精度、数字格式验证。benchmark 的数据都是简单的 `int32`。

### 5.5 HTTP+JSON 响应格式

```http
HTTP/1.1 200 OK\r\n
Content-Type: application/json\r\n
Content-Length: 9\r\n
\r\n
{"result":8}
```

或错误响应：
```http
HTTP/1.1 400 Bad Request\r\n
Content-Type: application/json\r\n
Content-Length: 20\r\n
\r\n
{"error":"bad request"}
```

---

## 六、压测工具的设计

### 6.1 两种方案

**方案 A：外部压测脚本（Python/Lua）**

```
Python 脚本 ──TCP──→ 服务端（RPC / HTTP+JSON）
```

- 优点：和被测服务独立进程，不影响性能测量
- 缺点：Python 本身可能成为瓶颈（GIL、socket 性能），需要额外维护一套 RPC 客户端实现

**方案 B：C++ 内置压测客户端**

```
C++ 压测程序 ──TCP──→ 服务端（RPC / HTTP+JSON）
```

- 优点：用同一个语言和同一套 IO 基础设施，压测客户端不会成为瓶颈
- 缺点：需要额外写客户端代码，但可以复用现有的 Serializer/Protocol/EventLoop

**决策**：方案 B。TinyRPC 的 RpcClient 已经实现了客户端逻辑。HTTP+JSON 对照组的客户端可以复用 Connection + EventLoop。用 C++ 写压测代码是零成本的——不需要引入 Python 依赖。

### 6.2 压测流程

```
压测主控程序
├── 1. 启动服务端（fork 子进程 或 单独线程）
├── 2. 等待服务端就绪
├── 3. Warmup：发送 1000 个请求（不计入统计）
├── 4. 正式压测：创建 N 个客户端线程，每个发送 M 个请求
│   ├── 记录每个请求的延迟（发前时间戳 → 收后时间戳）
│   ├── 成功/失败计数
│   └── 所有线程完成后汇总
├── 5. 计算统计指标：avg / p50 / p95 / p99 / QPS
├── 6. 收集 CPU 采样数据
├── 7. 输出报告
└── 8. 关闭服务端
```

### 6.3 延迟测量精度

`std::chrono::high_resolution_clock` 在 Linux 上通常是 `clock_gettime(CLOCK_MONOTONIC)`，精度在纳秒级。对于 benchmark（微秒到毫秒级延迟），这个精度足够。

**注意**：不要用 `std::chrono::system_clock`——它可能被 NTP 校时调整。用 `steady_clock` 或 `high_resolution_clock`。

```cpp
auto start = std::chrono::steady_clock::now();
// ... 发送请求 + 等待响应 ...
auto end = std::chrono::steady_clock::now();
auto latency_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
```

### 6.4 避免"测了自己"

客户端和被测服务必须跑在**不同进程**中。如果跑在同一个进程的不同线程里，线程调度、锁竞争、内存分配都会互相影响——测出来的不是真实性能。

**方案**：
- 主程序 `fork()` + `exec()` 启动服务端子进程（Linux）
- 或者手动启动服务端进程，压测程序连接它
- Benchmark 脚本建议用第二种——更简单，也方便单独调试服务端

---

## 七、可能的代码优化（以 benchmark 数据为驱动）

v0.6 定位是"先测后优化"——而不是"先优化再测"。优化应该在数据指导下进行，而不是凭直觉。

### 7.1 如果序列化是瓶颈

**可能的优化**：
- TLV 写入时预分配缓冲区大小，避免动态扩容（`reserve()`）
- 小整数用 varint 编码（类似 Protobuf），减少小数字的 4 字节固定开销
- 合并连续的同类型 TLV 字段（批处理）

### 7.2 如果内存拷贝是瓶颈

**当前的拷贝点**（从数据流中可以找到）：
- `Buffer::Append()` 时拷贝到接收缓冲区
- `TryPopFrame()` 时从 Buffer 拷贝出 Frame body
- 回调入队时拷贝 Frame 到 ThreadPool 任务
- 发送队列中的数据拷贝

**可能的优化**：
- 使用 `std::move` 或 `std::shared_ptr` 传递所有权，避免拷贝
- 使用 scatter/gather I/O（`writev`/`readv`）合并多次小数据拷贝
- 自定义内存池减少 `new`/`delete`

### 7.3 如果线程调度是瓶颈

**当前状态**：ThreadPool 已实现但未接入 Connection（见 HANDOFF.md 第 3 条）。

**可能的优化**：
- 将 ThreadPool 接入 Connection，FrameCallback 在 worker 线程执行
- 减少锁粒度——当前是全局锁保护整个队列，可以改为每 worker 一个队列 + work stealing
- 如果 `std::mutex` 在低竞争下仍有明显开销，考虑自旋锁（`std::atomic_flag`）

### 7.4 如果无锁队列成为必须

**前提**：benchmark 数据证明 `std::mutex` 是瓶颈，且无锁队列能显著改善。

**实现选项**：
- 单生产者单消费者（SPSC）：最简单，适合每个 Connection 有独立发送队列
- 多生产者多消费者（MPMC）：复杂，正确性极难保证
- 或者等到 v0.7+ 再考虑

### 7.5 优化原则

1. **每次只改一个变量**：改了序列化又改了线程池，QPS 提升了 20%——是谁的功劳？不知道。每次只改一处，跑一次 benchmark，记录效果。
2. **保留优化前的代码**（git commit 记录即可）：方便回溯"哪个优化最有效"
3. **优化完后更新 benchmark 报告**：附上优化前后的对比数据
4. **不是所有优化都要做**：如果一个优化只提升 2% QPS 但增加 200 行代码，衡量投入产出比

---

## 八、Benchmark 输出物

### 8.1 性能对比报告（加入 README）

```
┌─────────────────────┬──────────┬───────────┬──────────┐
│ 指标                │ TinyRPC  │ HTTP+JSON │ 提升倍数  │
├─────────────────────┼──────────┼───────────┼──────────┤
│ 请求体积 (Add)      │ 39 B     │ 120 B     │ 3.1x     │
│ 响应体积 (Add)      │ 25 B     │ 80 B      │ 3.2x     │
│ QPS (Echo, 8线程)   │ 85000    │ 42000     │ 2.0x     │
│ QPS (Add, 8线程)    │ 78000    │ 40000     │ 1.95x    │
│ P50 延迟 (Echo)     │ 45 μs    │ 95 μs     │ 2.1x     │
│ P99 延迟 (Echo)     │ 120 μs   │ 350 μs    │ 2.9x     │
│ CPU 使用率 (8线程)  │ 65%      │ 78%       │ 1.2x     │
└─────────────────────┴──────────┴───────────┴──────────┘
```

### 8.2 devlog 记录

每个优化项记录：
- 优化前数据（哪个 benchmark 场景、具体指标值）
- 改了什么（代码 diff 概述）
- 优化后数据
- 原因分析（为什么变快了 / 为什么没变快）

### 8.3 命令

```
./build/bench_rpc     # 压测 TinyRPC
./build/bench_http    # 压测 HTTP+JSON
./build/bench_report  # 汇总输出 Markdown 表格
```

---

## 九、新增/修改文件清单（v0.6）

### 新增

| 文件 | 作用 |
|------|------|
| `include/rpc/http_parser.h` | 最小化 HTTP/1.1 请求解析器 |
| `src/http_parser.cpp` | HTTP 解析器实现 |
| `include/rpc/json_serializer.h` | 最小化 JSON 序列化/反序列化 |
| `src/json_serializer.cpp` | JSON 序列化器实现 |
| `include/rpc/benchmark.h` | Benchmark 统计工具类（延迟分位数、QPS 计算） |
| `src/benchmark.cpp` | Benchmark 工具实现 |
| `src/bench_server.cpp` | 压测服务端（启动 RPC 或 HTTP+JSON 模式） |
| `src/bench_client.cpp` | 压测客户端（多线程并发请求 + 统计） |
| `tests/test_http_parser.cpp` | HTTP 解析器单元测试 |
| `tests/test_json.cpp` | JSON 序列化单元测试 |
| `docs/06-benchmark.md` | 本文档 |

### 修改

| 文件 | 改动内容 |
|------|----------|
| `README.md` | 添加性能对比表格 |
| `CMakeLists.txt` | 添加新源文件、新 target（bench_server, bench_client） |
| `CLAUDE.md` | 更新版本进度 |
| `docs/CHANGELOG.md` | 记录 v0.6 版本 |
| `docs/devlog.md` | 记录 benchmark 设计决策和优化历程 |

---

## 十、面试重点

1. **为什么 RPC 框架比 HTTP+JSON 快？** — 三个层面：序列化（二进制 vs 文本）、协议帧（定长紧凑帧头 vs 可变文本头）、解析路径（一次反序列化 vs HTTP 解析 + JSON 解析两次）。能分条说清每层的开销差异。

2. **Benchmark 怎么做才科学？** — warmup 消除冷启动、多次测量取平均、控制变量（同机同负载同业务逻辑）、多维度指标（不只 QPS）、延迟看分位数不看均值。面试官听到"我看了 p99 延迟"就知道你做过真正的性能分析。

3. **延迟为什么要看分位数？** — 平均值被长尾拉偏，掩盖了最差情况。"平均延迟 10ms"和"p99 延迟 500ms"可以同时存在——后者的用户体验差得多。长尾延迟（tail latency）是分布式系统的核心概念。

4. **你的 benchmark 有没有什么局限？** — loopback 不经过真实网络（带宽接近无限、无丢包）、未测大消息场景、未测多机跨网络场景。知道局限说明你有工程判断力，不是只会跑脚本。

5. **如果 benchmark 结果不理想怎么办？** — 按数据找到瓶颈（CPU profile、perf top），针对瓶颈优化，再测。用数据说话，不是拍脑袋猜。这个"测量→定位→优化→再测量"的循环比"一次测出好结果"更能体现工程能力。

6. **自实现协议 vs Protobuf 的性能对比？** — Protobuf 也有 varint、字段编号等优化，但引入了 IDL 编译步骤和库依赖。TinyRPC 用简单 TLV 实现，性价比高。如果面试官追问，可以承认 Protobuf 在大规模场景下更成熟，但对本项目来说"零依赖 + 可学习"的价值更大。

7. **HTTP/2 或 HTTP/3 会不会比 TinyRPC 快？** — HTTP/2 引入了二进制帧和头部压缩，确实缩小了和自建协议的差距。但 HTTP/2 仍然有 HTTP 语义层的开销（方法、状态码、头部），不像 RPC 协议那样直接面向方法调用。这个讨论展示了从 L4 到 L7 的全局视野。
