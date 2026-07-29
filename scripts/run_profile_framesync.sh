#!/bin/bash
# ============================================================
# run_profile_framesync.sh — 帧同步 CPU 热点分析 + 火焰图
#
# 用法: ./scripts/run_profile_framesync.sh [SERVER_IP] [PORT]
#
# 执行:
#   1. 100 客户端(50房间) 帧同步稳态 + perf 采样 → 火焰图
#   2. 300 客户端(150房间) 帧同步稳态 + perf 采样 → 火焰图
#   3. 500 客户端(250房间) 帧同步渐进 + perf 采样 → 火焰图
#   4. 汇总对比报告 → docs/bench/perf/profile_framesync.md
#
# 负载特征:
#   - 2 人组队 → StartGame → SendInput 循环 (20 tick/s)
#   - 压力集中在: FrameSyncManager::Tick / InputBuffer / GameState / SnapshotManager
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
TMP_PERF_BASE="/tmp/perf_rpc_fs"

TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
MAGENTA='\033[0;35m'
NC='\033[0m'

echo -e "${GREEN}=== 帧同步 CPU 热点分析 & 火焰图 ===${NC}"
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
    local perf_out="$OUTPUT_DIR/perf_fs_${label}.out"
    local perf_folded="$OUTPUT_DIR/perf_fs_${label}.folded"
    local flame_svg="$OUTPUT_DIR/flamegraph_fs_${label}.svg"
    local hotspot_txt="$OUTPUT_DIR/hotspots_fs_${label}.txt"
    local bench_log="$OUTPUT_DIR/bench_fs_${label}.log"
    local extra_args="--warmup 5"

    echo ""
    echo -e "${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${YELLOW}[fs:$label] framesync | conn=$conn | duration=${duration}s${NC}"
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

    echo -e "${YELLOW}[3/5] 运行帧同步压测...${NC}"
    "$BENCH_BIN" \
        --mode "$mode" \
        --server-ip "$SERVER_IP" \
        --port "$SERVER_PORT" \
        --conn "$conn" \
        --duration "$duration" \
        --ramp-rate "$ramp_rate" \
        --think-mean 0 \
        $extra_args \
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
        --title "TinyRPC FrameSync — $label ($conn clients, ${duration}s, 20fps)" \
        --width 1600 --colors java \
        "$perf_folded" > "$flame_svg"

    rm -f "$perf_out"
    echo -e "${GREEN}[fs:$label] 完成!${NC}"
    echo -e "  ${CYAN}火焰图:   $flame_svg${NC}"
    echo -e "  ${CYAN}热点报告: $hotspot_txt${NC}"
}

# ============================================================
echo -e "${MAGENTA}  场景 1/3: 100 客户端 (50 房间) 帧同步${NC}"
run_scene "100-fs" "framesync" 100 180 10
echo -e "${YELLOW}等待 15 秒...${NC}"; sleep 15

echo -e "${MAGENTA}  场景 2/3: 300 客户端 (150 房间) 帧同步${NC}"
run_scene "300-fs" "framesync" 300 180 20
echo -e "${YELLOW}等待 20 秒...${NC}"; sleep 20

echo -e "${MAGENTA}  场景 3/3: 500 客户端 (250 房间) 帧同步${NC}"
run_scene "500-fs" "framesync" 500 120 30

# ============================================================
REPORT_FILE="$OUTPUT_DIR/profile_framesync.md"

echo ""
echo -e "${GREEN}生成汇总报告: $REPORT_FILE${NC}"

cat > "$REPORT_FILE" << EOF
# 帧同步 CPU 热点分析 & 火焰图报告

> 生成时间: $TIMESTAMP
> 构建类型: RelWithDebInfo (-fno-omit-frame-pointer -g)

## 1. 测试场景

| # | 场景 | 客户端 | 房间(2人) | 帧率 | 持续时间 |
|---|------|--------|----------|------|---------|
| 1 | 100-fs | 100 | 50 | 20fps | 180s |
| 2 | 300-fs | 300 | 150 | 20fps | 180s |
| 3 | 500-fs | 500 | 250 | 20fps | 120s |

## 2. 压测链路

\`\`\`
连接 → 登录 → 偶数号创建房间 → 奇数号加入房间
→ StartGame (启动 FrameSyncManager, 20fps Timer)
→ SendInput 循环 (每 50ms 发送 PlayerInputReq)
→ LeaveRoom → Disconnect
\`\`\`

## 3. 各场景火焰图

| 场景 | 火焰图 |
|------|--------|
| 100-fs | [flamegraph_fs_100-fs.svg](flamegraph_fs_100-fs.svg) |
| 300-fs | [flamegraph_fs_300-fs.svg](flamegraph_fs_300-fs.svg) |
| 500-fs | [flamegraph_fs_500-fs.svg](flamegraph_fs_500-fs.svg) |

## 4. 各场景 Top 热点

### 100-fs

\`\`\`
$(cat "$OUTPUT_DIR/hotspots_fs_100-fs.txt" 2>/dev/null || echo "(未生成)")
\`\`\`

### 300-fs

\`\`\`
$(cat "$OUTPUT_DIR/hotspots_fs_300-fs.txt" 2>/dev/null || echo "(未生成)")
\`\`\`

### 500-fs

\`\`\`
$(cat "$OUTPUT_DIR/hotspots_fs_500-fs.txt" 2>/dev/null || echo "(未生成)")
\`\`\`

## 5. 关键观察

- **FrameSyncManager::Tick** 是否成为热点？
- **InputBuffer** 乱序插入/弹出是否随客户端增长？
- **SnapshotManager** 快照创建开销？
- **GameState::tickLogic** 确定性更新成本？
- 帧数据广播 **BroadcastToRoom** 占比？
EOF

echo ""
echo -e "${GREEN}============================================${NC}"
echo -e "${GREEN}  帧同步 CPU 热点分析 — 全部完成${NC}"
echo -e "${GREEN}============================================${NC}"
echo "汇总报告: $REPORT_FILE"
