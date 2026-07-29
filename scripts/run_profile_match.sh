#!/bin/bash
# ============================================================
# run_profile_match.sh — 匹配系统 CPU 热点分析 + 火焰图
#
# 用法: ./scripts/run_profile_match.sh [SERVER_IP] [PORT]
#
# 执行:
#   1. 100 客户端匹配循环 + perf 采样 → 火焰图
#   2. 300 客户端匹配循环 + perf 采样 → 火焰图
#   3. 500 客户端匹配循环 + perf 采样 → 火焰图
#   4. 汇总对比报告 → docs/bench/perf/profile_match.md
#
# 负载特征:
#   - EnterMatch(随机ELO) → TryMatch(批量配对) → CancelMatch 循环
#   - 压力集中在: MatchQueue::EnterQueue / FindMatch / TryMatch / EloCalculator
# ============================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

SERVER_IP="${1:-127.0.0.1}"
SERVER_PORT="${2:-8080}"
RPC_BIN="$PROJECT_ROOT/build/rpc"
BENCH_BIN="$PROJECT_ROOT/build/bench_game_client"
FLAMEGRAPH_DIR="${FLAMEGRAPH_DIR:-$HOME/FlameGraph}"
OUTPUT_DIR="$PROJECT_ROOT/docs/bench/perf"
TMP_PERF_BASE="/tmp/perf_rpc_match"

TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
MAGENTA='\033[0;35m'
NC='\033[0m'

echo -e "${GREEN}=== 匹配系统 CPU 热点分析 & 火焰图 ===${NC}"
echo "服务端: $SERVER_IP:$SERVER_PORT"
echo "时间:   $TIMESTAMP"
echo ""

FAIL=0

if [ ! -f "$RPC_BIN" ]; then
    echo -e "${RED}错误: $RPC_BIN 不存在${NC}"
    FAIL=1
fi
if [ ! -f "$BENCH_BIN" ]; then
    echo -e "${RED}错误: $BENCH_BIN 不存在${NC}"
    FAIL=1
fi

# 检查 ASAN
if nm "$RPC_BIN" 2>/dev/null | grep -q '__asan\|__sanitizer\|asan_init'; then
    echo -e "${RED}错误: $RPC_BIN 编译时开启了 ASAN，热点会失真${NC}"
    echo "  cd build && cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_ASAN=OFF && make -j\$(nproc) rpc bench_game_client"
    FAIL=1
fi

# FlameGraph 自动搜索
if [ -n "${SUDO_USER:-}" ]; then
    REAL_HOME="$(eval echo ~$SUDO_USER)"
else
    REAL_HOME="$HOME"
fi
for candidate in \
    "${FLAMEGRAPH_DIR:-}" \
    "$REAL_HOME/FlameGraph" \
    "/mnt/d/FlameGraph" \
    "/opt/FlameGraph"; do
    if [ -n "$candidate" ] && [ -f "$candidate/stackcollapse-perf.pl" ] && [ -f "$candidate/flamegraph.pl" ]; then
        FLAMEGRAPH_DIR="$candidate"
        break
    fi
done
STACK_COLLAPSE="$FLAMEGRAPH_DIR/stackcollapse-perf.pl"
FLAMEGRAPH_PL="$FLAMEGRAPH_DIR/flamegraph.pl"
if [ -z "$FLAMEGRAPH_DIR" ] || [ ! -f "$STACK_COLLAPSE" ] || [ ! -f "$FLAMEGRAPH_PL" ]; then
    echo -e "${RED}错误: FlameGraph 未找到${NC}"
    FAIL=1
fi

if [ "$FAIL" -eq 1 ]; then exit 1; fi

mkdir -p "$OUTPUT_DIR"

# ============================================================
PERF_PID=""
cleanup() {
    if [ -n "$PERF_PID" ] && kill -0 "$PERF_PID" 2>/dev/null; then
        kill -INT "$PERF_PID" 2>/dev/null || true
        for i in $(seq 1 10); do
            if ! kill -0 "$PERF_PID" 2>/dev/null; then break; fi
            sleep 1
        done
        if kill -0 "$PERF_PID" 2>/dev/null; then kill -9 "$PERF_PID" 2>/dev/null || true; fi
    fi
    local remaining; remaining=$(lsof -ti ":$SERVER_PORT" 2>/dev/null || true)
    if [ -n "$remaining" ]; then kill -9 "$remaining" 2>/dev/null || true; fi
    PERF_PID=""
}
trap cleanup EXIT

wait_for_server() {
    local max_wait=15; local waited=0
    while [ $waited -lt $max_wait ]; do
        if timeout 1 bash -c "echo >/dev/tcp/$SERVER_IP/$SERVER_PORT" 2>/dev/null; then return 0; fi
        sleep 1; waited=$((waited + 1))
    done
    return 1
}

# ============================================================
run_scene() {
    local label="$1" mode="$2" conn="$3" duration="$4" ramp_rate="$5"
    local perf_file="${TMP_PERF_BASE}_${label}.data"
    local perf_out="$OUTPUT_DIR/perf_match_${label}.out"
    local perf_folded="$OUTPUT_DIR/perf_match_${label}.folded"
    local flame_svg="$OUTPUT_DIR/flamegraph_match_${label}.svg"
    local hotspot_txt="$OUTPUT_DIR/hotspots_match_${label}.txt"
    local bench_log="$OUTPUT_DIR/bench_match_${label}.log"

    echo ""
    echo -e "${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${YELLOW}[match:$label] match | conn=$conn | duration=${duration}s${NC}"
    echo -e "${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"

    rm -f "$perf_file"

    echo -e "${YELLOW}[1/5] 启动服务端 (perf record)...${NC}"
    perf record -F 99 -g --call-graph dwarf -m 8M -o "$perf_file" -- "$RPC_BIN" &
    PERF_PID=$!

    echo -e "${YELLOW}[2/5] 等待服务端就绪...${NC}"
    if ! wait_for_server; then
        echo -e "${RED}错误: 服务端未就绪${NC}"; cleanup; return 1
    fi
    echo -e "${GREEN}  就绪 (PID=$PERF_PID)${NC}"

    echo -e "${YELLOW}[3/5] 运行匹配系统压测...${NC}"
    "$BENCH_BIN" \
        --mode "$mode" \
        --server-ip "$SERVER_IP" \
        --port "$SERVER_PORT" \
        --conn "$conn" \
        --duration "$duration" \
        --ramp-rate "$ramp_rate" \
        --think-mean 0 \
        2>&1 | tee "$bench_log"
    echo ""

    echo -e "${YELLOW}[4/5] 停止服务端...${NC}"
    kill -INT "$PERF_PID" 2>/dev/null || true
    local wait_count=0
    while kill -0 "$PERF_PID" 2>/dev/null && [ $wait_count -lt 30 ]; do
        sleep 1; wait_count=$((wait_count + 1))
    done
    if kill -0 "$PERF_PID" 2>/dev/null; then kill -9 "$PERF_PID" 2>/dev/null || true; fi
    PERF_PID=""

    if [ ! -f "$perf_file" ]; then
        echo -e "${RED}错误: perf.data 未生成${NC}"; return 1
    fi
    echo -e "${GREEN}  perf.data: $perf_file ($(du -h "$perf_file" | cut -f1))${NC}"

    echo -e "${YELLOW}[5/5] 生成火焰图 + 热点报告...${NC}"
    perf report -i "$perf_file" --sort=overhead,symbol --stdio --no-children 2>/dev/null | head -50 > "$hotspot_txt"
    perf report -i "$perf_file" --sort=overhead,symbol --stdio 2>/dev/null | head -50 >> "$hotspot_txt"
    perf script -i "$perf_file" 2>/dev/null > "$perf_out"
    "$STACK_COLLAPSE" "$perf_out" > "$perf_folded"
    "$FLAMEGRAPH_PL" \
        --title "TinyRPC MatchService — $label ($conn clients, ${duration}s)" \
        --width 1600 --colors java \
        "$perf_folded" > "$flame_svg"

    rm -f "$perf_out"
    echo -e "${GREEN}[match:$label] 完成!${NC}"
    echo -e "  ${CYAN}火焰图:   $flame_svg${NC}"
    echo -e "  ${CYAN}热点报告: $hotspot_txt${NC}"
}

# ============================================================
echo -e "${MAGENTA}  场景 1/3: 100 客户端匹配循环${NC}"
run_scene "100-match" "match" 100 180 20
echo -e "${YELLOW}等待 15 秒...${NC}"; sleep 15

echo -e "${MAGENTA}  场景 2/3: 300 客户端匹配循环${NC}"
run_scene "300-match" "match" 300 180 30
echo -e "${YELLOW}等待 20 秒...${NC}"; sleep 20

echo -e "${MAGENTA}  场景 3/3: 500 客户端匹配循环${NC}"
run_scene "500-match" "match" 500 120 50

# ============================================================
REPORT_FILE="$OUTPUT_DIR/profile_match.md"

echo ""
echo -e "${GREEN}生成汇总报告: $REPORT_FILE${NC}"

cat > "$REPORT_FILE" << EOF
# 匹配系统 CPU 热点分析 & 火焰图报告

> 生成时间: $TIMESTAMP
> 构建类型: RelWithDebInfo (-fno-omit-frame-pointer -g)

## 1. 测试场景

| # | 场景 | 客户端 | 模式 | 持续时间 |
|---|------|--------|------|---------|
| 1 | 100-match | 100 | EnterMatch→CancelMatch 循环 | 180s |
| 2 | 300-match | 300 | EnterMatch→CancelMatch 循环 | 180s |
| 3 | 500-match | 500 | EnterMatch→CancelMatch 循环 | 120s |

## 2. 压测链路

\`\`\`
服务端:
  连接 → 登录
  → EnterMatch(随机ELO) → EnterQueue(二分插入排序)
  → TryMatch(批量配对: 遍历队列 + 分差比较 + 双方出队)
  → OnMatchFound(创建房间 + 通知双方 + 超时)
  → CancelMatch(清理队列) → 循环
\`\`\`

## 3. 压力模块

| 模块 | 压力点 |
|------|--------|
| MatchQueue::EnterQueue | 二分插入 (std::lower_bound + insert) — 每次入队 |
| MatchQueue::TryMatch | 全队列遍历配对 (O(n)) — 每次 EnterMatch 触发 |
| MatchQueue::CancelMatch | 线性查找 + erase (O(n)) — 每次取消 |
| EloCalculator | ELO 分计算 — 每次配对 |
| MatchQueue::FindMatch | 单个匹配搜索 — 超时扫描 |
| TryMatch 中的 sort | 配对后重新排序 |

## 4. 各场景火焰图

| 场景 | 火焰图 |
|------|--------|
| 100-match | [flamegraph_match_100-match.svg](flamegraph_match_100-match.svg) |
| 300-match | [flamegraph_match_300-match.svg](flamegraph_match_300-match.svg) |
| 500-match | [flamegraph_match_500-match.svg](flamegraph_match_500-match.svg) |

## 5. 各场景 Top 热点

### 100-match

\`\`\`
$(cat "$OUTPUT_DIR/hotspots_match_100-match.txt" 2>/dev/null || echo "(未生成)")
\`\`\`

### 300-match

\`\`\`
$(cat "$OUTPUT_DIR/hotspots_match_300-match.txt" 2>/dev/null || echo "(未生成)")
\`\`\`

### 500-match

\`\`\`
$(cat "$OUTPUT_DIR/hotspots_match_500-match.txt" 2>/dev/null || echo "(未生成)")
\`\`\`

## 6. 关键观察

- **EnterQueue** 二分插入排序是否随队列长度成为瓶颈？
- **TryMatch** O(n) 遍历是否随并发恶化？
- **CancelMatch** 线性查找 erase 的占比？
- **FindMatch** 单独匹配搜索的成本？
- TryMatch 内部的 **sort/erase** 批量操作？
EOF

echo ""
echo -e "${GREEN}============================================${NC}"
echo -e "${GREEN}  匹配系统 CPU 热点分析 — 全部完成${NC}"
echo -e "${GREEN}============================================${NC}"
echo "汇总报告: $REPORT_FILE"
