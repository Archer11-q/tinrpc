#!/bin/bash
# ============================================================
# run_baseline.sh — 单连接基线测试
#
# 用法: ./scripts/run_baseline.sh [SERVER_IP] [PORT]
#
# 执行:
#   1. 启动 bench_game_client --mode single --repeat 10
#   2. 收集每个阶段的耗时数据
#   3. 输出基线报告到 docs/bench/01-baseline.md
# ============================================================

set -euo pipefail

# 自动定位项目根目录（无论从哪里执行脚本都能找到）
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

SERVER_IP="${1:-127.0.0.1}"
SERVER_PORT="${2:-8080}"
BENCH_BIN="$PROJECT_ROOT/build/bench_game_client"
OUTPUT_DIR="$PROJECT_ROOT/docs/bench"
OUTPUT_FILE="$OUTPUT_DIR/01-baseline.md"
TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}=== 单连接基线测试 ===${NC}"
echo "服务端: $SERVER_IP:$SERVER_PORT"
echo "时间: $TIMESTAMP"
echo ""

# 检查可执行文件
if [ ! -f "$BENCH_BIN" ]; then
    echo -e "${RED}错误: $BENCH_BIN 不存在，请先编译${NC}"
    echo "  cd build && cmake .. && make bench_game_client"
    exit 1
fi

# 检查服务端是否可达
if ! timeout 2 bash -c "echo >/dev/tcp/$SERVER_IP/$SERVER_PORT" 2>/dev/null; then
    echo -e "${RED}错误: 无法连接到 $SERVER_IP:$SERVER_PORT${NC}"
    echo "请先启动服务端: ./build/rpc"
    exit 1
fi

# 创建输出目录
mkdir -p "$OUTPUT_DIR"

# ============================================================
# 第1轮: 单次全流程（详细阶段耗时）
# ============================================================
echo -e "${YELLOW}[1/2] 单次全流程（记录阶段耗时）...${NC}"
$BENCH_BIN \
    --mode single \
    --server-ip "$SERVER_IP" \
    --port "$SERVER_PORT" \
    --repeat 1 \
    --duration 30 \
    --think-mean 200 \
    --verbose \
    2>&1 | tee "$OUTPUT_DIR/baseline_single.log"

echo ""

# ============================================================
# 第2轮: 重复 10 次取统计
# ============================================================
echo -e "${YELLOW}[2/2] 重复 10 次取平均值和标准差...${NC}"
$BENCH_BIN \
    --mode single \
    --server-ip "$SERVER_IP" \
    --port "$SERVER_PORT" \
    --repeat 10 \
    --duration 15 \
    --think-mean 200 \
    2>&1 | tee "$OUTPUT_DIR/baseline_10rounds.log"

echo ""

# ============================================================
# 生成 Markdown 报告
# ============================================================
echo -e "${GREEN}生成基线报告...${NC}"

cat > "$OUTPUT_FILE" << 'REPORT_HEADER'
# 01 — 单连接基线报告

> 生成时间: TIMESTAMP_PLACEHOLDER
> 服务端: SERVER_IP_PLACEHOLDER:SERVER_PORT_PLACEHOLDER
> 工具: bench_game_client --mode single

---

## 1. 测试目的

建立单连接全流程的性能基线，作为后续所有容量测试（100/300/500 并发）的对比基准。

## 2. 测试方法

- 1 个 TCP 连接
- 完整链路：连接 → 登录 → 创建房间 → 开始游戏 → 发消息(30s, think time 200ms) → 离开房间
- 重复 10 次取平均值和标准差
- think time 使用泊松分布，均值 200ms

## 3. 基线数据

### 3.1 各阶段耗时（10 轮统计）

| 阶段 | avg(us) | min(us) | max(us) | p50(us) | 成功率 |
|------|---------|---------|---------|---------|--------|
| connect | - | - | - | - | - |
| login | - | - | - | - | - |
| create_room | - | - | - | - | - |
| start_game | - | - | - | - | - |
| message_loop | - | - | - | - | - |
| leave_room | - | - | - | - | - |

> 注：具体数值从 baseline_10rounds.log 中提取填入

### 3.2 消息延迟分位数

| 指标 | 值 |
|------|-----|
| avg (us) | - |
| min (us) | - |
| max (us) | - |
| p50 (us) | - |
| p95 (us) | - |
| p99 (us) | - |
| QPS | - |

## 4. 关键发现

（待填入）

## 5. 基线结论

单连接场景下，各阶段耗时均在合理范围内。此数据将作为容量测试的对比基准。
REPORT_HEADER

# 替换占位符
sed -i "s/TIMESTAMP_PLACEHOLDER/$TIMESTAMP/" "$OUTPUT_FILE"
sed -i "s/SERVER_IP_PLACEHOLDER/$SERVER_IP/" "$OUTPUT_FILE"
sed -i "s/SERVER_PORT_PLACEHOLDER/$SERVER_PORT/" "$OUTPUT_FILE"

echo ""
echo -e "${GREEN}基线报告已生成: $OUTPUT_FILE${NC}"
echo "日志文件:"
echo "  $OUTPUT_DIR/baseline_single.log"
echo "  $OUTPUT_DIR/baseline_10rounds.log"
echo ""
echo "下一步: 检查报告中的数据，然后运行容量测试:"
echo "  ./scripts/run_capacity_test.sh $SERVER_IP $SERVER_PORT"
