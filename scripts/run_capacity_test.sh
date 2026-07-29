#!/bin/bash
# ============================================================
# run_capacity_test.sh — 渐进加压 + 稳态容量测试
#
# 用法: ./scripts/run_capacity_test.sh [SERVER_IP] [PORT]
#
# 执行:
#   1. 10 连接渐进加压（ramp-up）
#   2. 50 连接渐进加压
#   3. 100 连接稳态 5 分钟
#   4. 300 连接稳态 5 分钟
#   5. 500 连接 ramp-up（找拐点）
#   6. 输出汇总报告到 docs/bench/02-capacity.md
# ============================================================

set -euo pipefail

# 自动定位项目根目录
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

SERVER_IP="${1:-127.0.0.1}"
SERVER_PORT="${2:-8080}"
BENCH_BIN="$PROJECT_ROOT/build/bench_game_client"
OUTPUT_DIR="$PROJECT_ROOT/docs/bench"
OUTPUT_FILE="$OUTPUT_DIR/02-capacity.md"
TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

echo -e "${GREEN}=== 容量测试套件 ===${NC}"
echo "服务端: $SERVER_IP:$SERVER_PORT"
echo "时间: $TIMESTAMP"
echo ""

# 检查
if [ ! -f "$BENCH_BIN" ]; then
    echo -e "${RED}错误: $BENCH_BIN 不存在${NC}"
    exit 1
fi

if ! timeout 2 bash -c "echo >/dev/tcp/$SERVER_IP/$SERVER_PORT" 2>/dev/null; then
    echo -e "${RED}错误: 无法连接到 $SERVER_IP:$SERVER_PORT${NC}"
    exit 1
fi

mkdir -p "$OUTPUT_DIR"

# 初始化报告
cat > "$OUTPUT_FILE" << EOF
# 02 — 容量测试报告

> 生成时间: $TIMESTAMP
> 服务端: $SERVER_IP:$SERVER_PORT

---

## 1. 测试场景

| 测试 | 连接数 | 类型 | 持续时间 | ramp速率 |
|------|--------|------|----------|----------|
| 10 连接渐进 | 10 | ramp | 60s/conn | 5 conn/s |
| 50 连接渐进 | 50 | ramp | 60s/conn | 10 conn/s |
| 100 连接稳态 | 100 | steady | 300s (5min) | 20 conn/s |
| 300 连接稳态 | 300 | steady | 300s (5min) | 30 conn/s |
| 500 连接加压 | 500 | ramp | 60s/conn | 50 conn/s |

## 2. 容量测试数据汇总

| 场景 | 连接数 | 成功数 | QPS | avg(us) | p50(us) | p95(us) | p99(us) | 错误率 |
|------|--------|--------|-----|---------|---------|---------|----------|--------|
| 10-ramp | 10 | - | - | - | - | - | - | - |
| 50-ramp | 50 | - | - | - | - | - | - | - |
| 100-steady | 100 | - | - | - | - | - | - | - |
| 300-steady | 300 | - | - | - | - | - | - | - |
| 500-ramp | 500 | - | - | - | - | - | - | - |

## 3. 各场景详细数据

EOF

# ============================================================
# 函数: 运行单次测试并记录
# ============================================================
run_test() {
    local label="$1"
    local mode="$2"
    local conn="$3"
    local duration="$4"
    local ramp_rate="$5"
    local logfile="$OUTPUT_DIR/capacity_${label}.log"

    echo ""
    echo -e "${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${YELLOW}[$label] $mode | conn=$conn | duration=${duration}s | ramp=${ramp_rate}/s${NC}"
    echo -e "${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"

    $BENCH_BIN \
        --mode "$mode" \
        --server-ip "$SERVER_IP" \
        --port "$SERVER_PORT" \
        --conn "$conn" \
        --duration "$duration" \
        --ramp-rate "$ramp_rate" \
        --think-mean 200 \
        --warmup 10 \
        2>&1 | tee "$logfile"

    echo -e "${GREEN}[$label] 完成，日志: $logfile${NC}"
}

# ============================================================
# 执行测试
# ============================================================

echo -e "${YELLOW}注意: 每个测试之间建议等待 10 秒让服务端恢复${NC}"
echo ""

# 1. 10 连接渐进
run_test "10-ramp" "ramp" 10 60 5
echo -e "${CYAN}等待 10 秒...${NC}"; sleep 10

# 2. 50 连接渐进
run_test "50-ramp" "ramp" 50 60 10
echo -e "${CYAN}等待 15 秒...${NC}"; sleep 15

# 3. 100 连接稳态 5 分钟
run_test "100-steady" "steady" 100 300 20
echo -e "${CYAN}等待 20 秒...${NC}"; sleep 20

# 4. 300 连接稳态 5 分钟
run_test "300-steady" "steady" 300 300 30
echo -e "${CYAN}等待 30 秒...${NC}"; sleep 30

# 5. 500 连接 ramp（找拐点）
run_test "500-ramp" "ramp" 500 60 50

# ============================================================
# 汇总
# ============================================================
echo ""
echo -e "${GREEN}=== 全部容量测试完成 ===${NC}"
echo ""
echo "提取 Markdown 表格行..."

echo "" >> "$OUTPUT_FILE"
echo "## 4. Markdown 汇总行" >> "$OUTPUT_FILE"
echo "" >> "$OUTPUT_FILE"
echo "| 场景 | 连接数 | 时长 | QPS | avg(us) | p50(us) | p95(us) | p99(us) | 错误率 |" >> "$OUTPUT_FILE"
echo "|------|--------|------|-----|---------|---------|---------|----------|--------|" >> "$OUTPUT_FILE"

for label in 10-ramp 50-ramp 100-steady 300-steady 500-ramp; do
    logfile="$OUTPUT_DIR/capacity_${label}.log"
    if [ -f "$logfile" ]; then
        # 提取 Markdown 表格行（bench 客户端输出的那行）
        md_line=$(grep -E '^\|.*\|.*\|.*\|.*\|.*\|.*\|.*\|.*\|$' "$logfile" | tail -1)
        if [ -n "$md_line" ]; then
            echo "$md_line" >> "$OUTPUT_FILE"
        else
            echo "| $label | - | - | - | - | - | - | - | - |" >> "$OUTPUT_FILE"
        fi
    fi
done

echo ""
echo -e "${GREEN}容量测试报告: $OUTPUT_FILE${NC}"
echo ""
echo "下一步: 分析数据 → 火焰图分析 → 瓶颈定位"
echo "  ./scripts/run_serialize_bench.sh"
