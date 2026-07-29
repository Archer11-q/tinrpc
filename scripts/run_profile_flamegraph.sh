#!/bin/bash
# ============================================================
# run_profile_flamegraph.sh — CPU 热点分析 + 火焰图
#
# 用法: ./scripts/run_profile_flamegraph.sh [SERVER_IP] [PORT]
#
# 执行:
#   1. 100 连接稳态压测 + perf 采样 → 火焰图 + 热点报告
#   2. 300 连接稳态压测 + perf 采样 → 火焰图 + 热点报告
#   3. 500 连接渐进加压 + perf 采样 → 火焰图 + 热点报告
#   4. 汇总对比报告 → docs/bench/perf/profile_report.md
#
# 已知问题规避:
#   - perf.data 写到 /tmp/ (WSL2 原生 ext4)，不用 /mnt/d/ (DrvFs)
#   - perf buffer 用 -m 8M，避免 mlock 限制
#   - DWARF 调用栈展开，比 frame-pointer 更准确
# ============================================================

set -euo pipefail

# 自动定位项目根目录
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

SERVER_IP="${1:-127.0.0.1}"
SERVER_PORT="${2:-8080}"
RPC_BIN="$PROJECT_ROOT/build/rpc"
BENCH_BIN="$PROJECT_ROOT/build/bench_game_client"
OUTPUT_DIR="$PROJECT_ROOT/docs/bench/perf"

# 如果通过 sudo 执行，用真实用户的 $HOME 而非 /root
# $SUDO_USER 是 sudo 时保留的原始用户名
if [ -n "${SUDO_USER:-}" ]; then
    REAL_HOME="$(eval echo ~$SUDO_USER)"
else
    REAL_HOME="$HOME"
fi
TMP_PERF_BASE="/tmp/perf_rpc"

TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
MAGENTA='\033[0;35m'
NC='\033[0m'

# ============================================================
# 前置检查
# ============================================================
echo -e "${GREEN}=== TinyRPC CPU 热点分析 & 火焰图 ===${NC}"
echo "服务端: $SERVER_IP:$SERVER_PORT"
echo "时间:   $TIMESTAMP"
echo ""

FAIL=0

# 检查可执行文件
if [ ! -f "$RPC_BIN" ]; then
    echo -e "${RED}错误: $RPC_BIN 不存在，请先编译${NC}"
    echo "  cd build && cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo && make -j\$(nproc)"
    FAIL=1
fi

if [ ! -f "$BENCH_BIN" ]; then
    echo -e "${RED}错误: $BENCH_BIN 不存在，请先编译${NC}"
    echo "  cd build && cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo && make -j\$(nproc) bench_game_client"
    FAIL=1
fi

# 检查 ASAN — 压测/火焰图绝对不能开，否则热点失真（sanitizer 函数会挤占真实业务热点）
if nm "$RPC_BIN" 2>/dev/null | grep -q '__asan\|__sanitizer\|asan_init'; then
    echo -e "${RED}=============================================${NC}"
    echo -e "${RED}错误: $RPC_BIN 编译时开启了 AddressSanitizer!${NC}"
    echo -e "${RED}=============================================${NC}"
    echo ""
    echo "  ASAN 会使 CPU 热点严重失真:"
    echo "    - __sanitizer::StackDepotBase::Put 会占据 Top-1"
    echo "    - asan_new / QuarantineChunk 渗透到每个 alloc/free 路径"
    echo "    - 真实业务函数被压扁，火焰图失去分析价值"
    echo ""
    echo "  修复:"
    echo "    cd build && cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_ASAN=OFF"
    echo "    make -j\$(nproc) rpc bench_game_client"
    echo ""
    FAIL=1
fi

# 检查 perf
if ! command -v perf &>/dev/null; then
    echo -e "${RED}错误: perf 未安装${NC}"
    echo "  sudo apt-get update"
    echo "  sudo apt-get install linux-tools-common linux-tools-generic linux-tools-\$(uname -r)"
    FAIL=1
fi

# 检查 FlameGraph — 自动搜索多个常见路径
FLAMEGRAPH_DIR=""
STACK_COLLAPSE=""
FLAMEGRAPH_PL=""

# 搜索优先级:
#   1. 环境变量 FLAMEGRAPH_DIR（用户显式指定）
#   2. $REAL_HOME/FlameGraph（默认 clone 位置）
#   3. /opt/FlameGraph
#   4. stackcollapse-perf.pl 在 $PATH 里（推断目录）
if [ -n "${FLAMEGRAPH_DIR:-}" ]; then
    # 用户已通过环境变量指定
    STACK_COLLAPSE="$FLAMEGRAPH_DIR/stackcollapse-perf.pl"
    FLAMEGRAPH_PL="$FLAMEGRAPH_DIR/flamegraph.pl"
fi

if [ -z "$FLAMEGRAPH_DIR" ] || [ ! -f "$STACK_COLLAPSE" ] || [ ! -f "$FLAMEGRAPH_PL" ]; then
    for candidate in \
        "$REAL_HOME/FlameGraph" \
        "/mnt/d/FlameGraph" \
        "/mnt/c/FlameGraph" \
        "/opt/FlameGraph" \
        "/usr/local/FlameGraph"; do
        if [ -f "$candidate/stackcollapse-perf.pl" ] && [ -f "$candidate/flamegraph.pl" ]; then
            FLAMEGRAPH_DIR="$candidate"
            STACK_COLLAPSE="$candidate/stackcollapse-perf.pl"
            FLAMEGRAPH_PL="$candidate/flamegraph.pl"
            break
        fi
    done
fi

# 最后尝试通过 $PATH 查找
if [ -z "$FLAMEGRAPH_DIR" ]; then
    SCP_PATH="$(command -v stackcollapse-perf.pl 2>/dev/null || true)"
    if [ -n "$SCP_PATH" ]; then
        FLAMEGRAPH_DIR="$(dirname "$SCP_PATH")"
        STACK_COLLAPSE="$SCP_PATH"
        FLAMEGRAPH_PL="$FLAMEGRAPH_DIR/flamegraph.pl"
    fi
fi

if [ -z "$FLAMEGRAPH_DIR" ] || [ ! -f "$STACK_COLLAPSE" ] || [ ! -f "$FLAMEGRAPH_PL" ]; then
    echo -e "${RED}错误: FlameGraph 脚本未找到${NC}"
    echo "  已搜索: $REAL_HOME/FlameGraph, /opt/FlameGraph, /usr/local/FlameGraph"
    if [ -n "${SUDO_USER:-}" ]; then
        echo "  注意: 当前通过 sudo 执行，已自动查找真实用户 ($SUDO_USER) 的 HOME"
    fi
    echo "  安装: git clone https://github.com/brendangregg/FlameGraph.git ~/FlameGraph"
    FAIL=1
fi

# 检查 perf_event_paranoid (需要 ≤1 才能非 root 采样)
PARANOID=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo 99)
if [ "$PARANOID" -gt 1 ]; then
    if [ "$(id -u)" -eq 0 ]; then
        # root 用户不 受 paranoid 限制，静默通过
        :
    else
        echo -e "${YELLOW}警告: kernel.perf_event_paranoid=$PARANOID (>1), 非 root 无法采样${NC}"
        echo "  方案A: sudo sysctl -w kernel.perf_event_paranoid=1"
        echo "  方案B: sudo $0 $SERVER_IP $SERVER_PORT"
        echo ""
    fi
fi

# 检查 mlock 限制
MLOCK_KB=$(ulimit -l 2>/dev/null || echo 0)
if [ "$MLOCK_KB" != "unlimited" ] && [ "$MLOCK_KB" -lt 8192 ] 2>/dev/null; then
    echo -e "${YELLOW}警告: max locked memory = ${MLOCK_KB}KB (< 8192KB), 已用 -m 8M 规避${NC}"
    echo ""
fi

if [ "$FAIL" -eq 1 ]; then
    exit 1
fi

echo -e "${GREEN}  ✓ rpc 服务端可执行文件${NC}"
echo -e "${GREEN}  ✓ bench_game_client 可执行文件${NC}"
echo -e "${GREEN}  ✓ perf 已安装${NC}"
echo -e "${GREEN}  ✓ FlameGraph 已安装 ($FLAMEGRAPH_DIR)${NC}"
echo ""

# 创建输出目录
mkdir -p "$OUTPUT_DIR"

# ============================================================
# 清理函数：确保服务端进程被终止
# ============================================================
PERF_PID=""
cleanup() {
    if [ -n "$PERF_PID" ] && kill -0 "$PERF_PID" 2>/dev/null; then
        echo -e "${YELLOW}[清理] 正在终止 perf (PID=$PERF_PID)...${NC}"
        # 先 SIGINT，让 perf 优雅结束并写入数据
        kill -INT "$PERF_PID" 2>/dev/null || true
        # 最多等 10 秒
        for i in $(seq 1 10); do
            if ! kill -0 "$PERF_PID" 2>/dev/null; then
                break
            fi
            sleep 1
        done
        # 还没退出就强杀
        if kill -0 "$PERF_PID" 2>/dev/null; then
            echo -e "${RED}[清理] 强制终止 perf${NC}"
            kill -9 "$PERF_PID" 2>/dev/null || true
        fi
    fi
    # 确保端口释放
    local remaining
    remaining=$(lsof -ti ":$SERVER_PORT" 2>/dev/null || true)
    if [ -n "$remaining" ]; then
        kill -9 "$remaining" 2>/dev/null || true
    fi
    PERF_PID=""
}
trap cleanup EXIT

# ============================================================
# 等待服务端就绪
# ============================================================
wait_for_server() {
    local max_wait=15
    local waited=0
    while [ $waited -lt $max_wait ]; do
        if timeout 1 bash -c "echo >/dev/tcp/$SERVER_IP/$SERVER_PORT" 2>/dev/null; then
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
    done
    return 1
}

# ============================================================
# 核心函数: 启动服务端(perf下) → 压测 → 停止 → 生成火焰图
# ============================================================
run_profile_scene() {
    local label="$1"       # 场景标签: 100-steady, 300-steady, 500-ramp
    local mode="$2"        # bench 模式: steady, ramp
    local conn="$3"        # 连接数
    local duration="$4"    # 持续时间(秒)
    local ramp_rate="$5"   # ramp 速率
    local extra_args="${6:-}"  # 额外参数 (warmup 等)

    local perf_file="${TMP_PERF_BASE}_${label}.data"
    local perf_out="$OUTPUT_DIR/perf_${label}.out"
    local perf_folded="$OUTPUT_DIR/perf_${label}.folded"
    local flame_svg="$OUTPUT_DIR/flamegraph_${label}.svg"
    local hotspot_txt="$OUTPUT_DIR/hotspots_${label}.txt"
    local bench_log="$OUTPUT_DIR/bench_${label}.log"

    echo ""
    echo -e "${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${YELLOW}[$label] $mode | conn=$conn | duration=${duration}s | ramp=${ramp_rate}/s${NC}"
    echo -e "${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo "  perf data: $perf_file"
    echo "  flamegraph: $flame_svg"
    echo ""

    # ---- 步骤 1: 清理旧 perf 数据 ----
    rm -f "$perf_file"

    # ---- 步骤 2: 在 perf 下启动服务端 ----
    echo -e "${YELLOW}[1/5] 启动服务端 (perf record -F 99 -g -m 8M)...${NC}"

    # perf 参数说明:
    #   -F 99         采样频率 99Hz (避免与定时器同频)
    #   -g            记录调用栈
    #   --call-graph dwarf  用 DWARF 展开栈 (比 fp 准确)
    #   -m 8M         缓冲区 8MB (规避 WSL2 mlock 限制)
    #   -o /tmp/...   写到 WSL2 原生 ext4 (规避 DrvFs Bad address)
    perf record \
        -F 99 \
        -g --call-graph dwarf \
        -m 8M \
        -o "$perf_file" \
        -- "$RPC_BIN" &
    PERF_PID=$!

    # 等待服务端就绪
    echo -e "${YELLOW}[2/5] 等待服务端就绪...${NC}"
    if ! wait_for_server; then
        echo -e "${RED}错误: 服务端未在 ${max_wait} 秒内就绪${NC}"
        cleanup
        return 1
    fi
    echo -e "${GREEN}  服务端就绪 (PID=$PERF_PID)${NC}"

    # ---- 步骤 3: 运行压测客户端 ----
    echo -e "${YELLOW}[3/5] 运行压测客户端...${NC}"

    local bench_cmd=(
        "$BENCH_BIN"
        --mode "$mode"
        --server-ip "$SERVER_IP"
        --port "$SERVER_PORT"
        --conn "$conn"
        --duration "$duration"
        --ramp-rate "$ramp_rate"
        --think-mean 200
        $extra_args
    )

    # 稳态模式加预热
    if [ "$mode" = "steady" ]; then
        bench_cmd+=(--warmup 10)
    fi

    "${bench_cmd[@]}" 2>&1 | tee "$bench_log"

    echo ""

    # ---- 步骤 4: 停止服务端 (SIGINT → perf 写入数据) ----
    echo -e "${YELLOW}[4/5] 停止服务端 & 等待 perf 写入数据...${NC}"

    # 发送 SIGINT 给 perf，perf 会转而 SIGINT 子进程并开始写数据
    kill -INT "$PERF_PID" 2>/dev/null || true

    # 等待 perf 进程退出（写入 perf.data 需要几秒）
    local wait_count=0
    while kill -0 "$PERF_PID" 2>/dev/null && [ $wait_count -lt 30 ]; do
        sleep 1
        wait_count=$((wait_count + 1))
        if [ $((wait_count % 5)) -eq 0 ]; then
            echo "  等待 perf 退出... (${wait_count}s)"
        fi
    done

    if kill -0 "$PERF_PID" 2>/dev/null; then
        echo -e "${RED}  perf 未在 30s 内退出，强制终止${NC}"
        kill -9 "$PERF_PID" 2>/dev/null || true
    fi
    PERF_PID=""

    # 确认 perf.data 已生成
    if [ ! -f "$perf_file" ]; then
        echo -e "${RED}错误: $perf_file 未生成${NC}"
        return 1
    fi
    echo -e "${GREEN}  perf.data: $perf_file ($(du -h "$perf_file" | cut -f1))${NC}"

    # ---- 步骤 5: 生成火焰图 + 热点报告 ----
    echo -e "${YELLOW}[5/5] 生成火焰图和热点报告...${NC}"

    # 5a. 热点函数 Top 30
    echo "  生成热点报告: $hotspot_txt"
    perf report -i "$perf_file" --sort=overhead,symbol --stdio --no-children 2>/dev/null | \
        head -50 > "$hotspot_txt"
    # 同时生成带 children 的（看完整调用链开销）
    perf report -i "$perf_file" --sort=overhead,symbol --stdio 2>/dev/null | \
        head -50 >> "$hotspot_txt"

    # 5b. 展开调用栈 → 折叠 → 火焰图
    echo "  展开调用栈: $perf_out"
    perf script -i "$perf_file" 2>/dev/null > "$perf_out"

    echo "  折叠调用栈: $perf_folded"
    "$STACK_COLLAPSE" "$perf_out" > "$perf_folded"

    echo "  生成火焰图: $flame_svg"
    "$FLAMEGRAPH_PL" \
        --title "TinyRPC Game Server — $label ($conn conn, ${duration}s)" \
        --width 1600 \
        --colors java \
        "$perf_folded" > "$flame_svg"

    echo ""
    echo -e "${GREEN}[$label] 完成!${NC}"
    echo -e "  ${CYAN}火焰图:   $flame_svg${NC}"
    echo -e "  ${CYAN}热点报告: $hotspot_txt${NC}"
    echo -e "  ${CYAN}压测日志: $bench_log${NC}"
    echo ""

    # 清理中间文件（保留 folded 供后续对比）
    if [ -f "$perf_out" ] && [ -f "$perf_folded" ]; then
        rm -f "$perf_out"
    fi
}

# ============================================================
# 执行三场景采样
# ============================================================

echo -e "${MAGENTA}============================================${NC}"
echo -e "${MAGENTA}  场景 1/3: 100 连接稳态 (中负载)${NC}"
echo -e "${MAGENTA}============================================${NC}"

run_profile_scene "100-steady" "steady" 100 180 20

echo -e "${YELLOW}等待 15 秒让系统恢复...${NC}"
sleep 15

# ----------------------------------------------------------
echo -e "${MAGENTA}============================================${NC}"
echo -e "${MAGENTA}  场景 2/3: 300 连接稳态 (高负载)${NC}"
echo -e "${MAGENTA}============================================${NC}"

run_profile_scene "300-steady" "steady" 300 180 30

echo -e "${YELLOW}等待 20 秒让系统恢复...${NC}"
sleep 20

# ----------------------------------------------------------
echo -e "${MAGENTA}============================================${NC}"
echo -e "${MAGENTA}  场景 3/3: 500 连接渐进 (找拐点)${NC}"
echo -e "${MAGENTA}============================================${NC}"

run_profile_scene "500-ramp" "ramp" 500 120 50

# ============================================================
# 生成汇总报告
# ============================================================
REPORT_FILE="$OUTPUT_DIR/profile_report.md"

echo ""
echo -e "${GREEN}生成汇总报告: $REPORT_FILE${NC}"

cat > "$REPORT_FILE" << EOF
# CPU 热点分析 & 火焰图报告

> 生成时间: $TIMESTAMP
> 服务端: \`$RPC_BIN\`
> 构建类型: RelWithDebInfo (-fno-omit-frame-pointer -g)
> 工具: perf record -F 99 -g --call-graph dwarf -m 8M

---

## 1. 测试场景

| # | 场景 | 连接数 | 模式 | 持续时间 | ramp速率 | 火焰图 |
|---|------|--------|------|----------|----------|--------|
| 1 | 100-steady | 100 | steady | 180s | 20/s | [flamegraph_100-steady.svg](flamegraph_100-steady.svg) |
| 2 | 300-steady | 300 | steady | 180s | 30/s | [flamegraph_300-steady.svg](flamegraph_300-steady.svg) |
| 3 | 500-ramp | 500 | ramp | 120s | 50/s | [flamegraph_500-ramp.svg](flamegraph_500-ramp.svg) |

## 2. 各场景 Top 热点

### 2.1 100 连接稳态

\`\`\`
$(cat "$OUTPUT_DIR/hotspots_100-steady.txt" 2>/dev/null || echo "(数据未生成)")
\`\`\`

### 2.2 300 连接稳态

\`\`\`
$(cat "$OUTPUT_DIR/hotspots_300-steady.txt" 2>/dev/null || echo "(数据未生成)")
\`\`\`

### 2.3 500 连接渐进

\`\`\`
$(cat "$OUTPUT_DIR/hotspots_500-ramp.txt" 2>/dev/null || echo "(数据未生成)")
\`\`\`

## 3. 火焰图解读指南

### 怎么看火焰图
- **X 轴宽度** = CPU 占比，越宽越热（按函数名字母排序，不是时间轴）
- **Y 轴高度** = 调用栈深度，从下到上是 caller → callee
- **颜色** = 随机，仅用于区分不同栈帧，无特殊含义
- **点击** = 在浏览器中可放大到某个函数子树
- **搜索框** = 右上角可高亮特定函数

### 常见瓶颈模式
| 火焰图形状 | 含义 |
|-----------|------|
| 平顶山 (plateau) | 某个函数自身耗时很宽 → 优化该函数本体 |
| 塔楼 (tower) | 深层调用链 → 考虑减少调用深度或内联 |
| 多塔并立 | 多个独立热点路径 → 逐个优化 |
| 细碎锯齿 | 大量小函数分散 → 可能是虚函数/间接调用开销 |

## 4. 对比分析

### 随负载变化的趋势

| 指标 | 100-steady | 300-steady | 500-ramp | 趋势 |
|------|-----------|-----------|---------|------|
| QPS (峰值) | - | - | - | |
| p99 延迟 (us) | - | - | - | |
| 热点函数 #1 | - | - | - | |
| 热点函数 #2 | - | - | - | |
| 热点函数 #3 | - | - | - | |

> 从对应的 bench_*.log 中提取 QPS 和延迟数据填入上表。

## 5. 优化建议

（根据火焰图分析结果填入）

---

## 附录: 生成火焰图的命令

\`\`\`bash
# 单场景快速运行
perf record -F 99 -g --call-graph dwarf -m 8M -o /tmp/perf_rpc.data -- ./build/rpc &
# ... 运行压测客户端 ...
kill -INT \$!  # SIGINT 让 perf 优雅结束

# 生成火焰图
perf script -i /tmp/perf_rpc.data | \\
  ~/FlameGraph/stackcollapse-perf.pl | \\
  ~/FlameGraph/flamegraph.pl --width 1600 > flamegraph.svg
\`\`\`
EOF

echo ""
echo -e "${GREEN}============================================${NC}"
echo -e "${GREEN}  CPU 热点分析 & 火焰图 — 全部完成${NC}"
echo -e "${GREEN}============================================${NC}"
echo ""
echo "输出文件:"
echo -e "  ${CYAN}汇总报告:${NC} $REPORT_FILE"
echo ""
echo "火焰图 (浏览器打开):"
for svg in "$OUTPUT_DIR"/flamegraph_*.svg; do
    if [ -f "$svg" ]; then
        echo -e "  ${CYAN}$(basename "$svg")${NC} → $svg"
    fi
done
echo ""
echo "热点报告:"
for txt in "$OUTPUT_DIR"/hotspots_*.txt; do
    if [ -f "$txt" ]; then
        echo -e "  ${CYAN}$(basename "$txt")${NC} → $txt"
    fi
done
echo ""
echo "压测日志:"
for log in "$OUTPUT_DIR"/bench_*.log; do
    if [ -f "$log" ]; then
        echo -e "  ${CYAN}$(basename "$log")${NC} → $log"
    fi
done
echo ""
echo -e "${YELLOW}提示: 用浏览器打开火焰图 SVG，可以点击放大、搜索函数名${NC}"
