#!/bin/bash
# ============================================================
# run_serialize_bench.sh — Protobuf 序列化基准测试
#
# 用法: ./scripts/run_serialize_bench.sh
#
# 运行已有的 bench_serialize 和 test_proto_vs_tlv，
# 收集序列化性能基线数据。
# ============================================================

set -euo pipefail

# 自动定位项目根目录
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

OUTPUT_DIR="$PROJECT_ROOT/docs/bench"
OUTPUT_FILE="$OUTPUT_DIR/03-serialize-baseline.md"
TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}=== Protobuf 序列化基准测试 ===${NC}"
echo "时间: $TIMESTAMP"
echo ""

mkdir -p "$OUTPUT_DIR"

# ============================================================
# 1. bench_serialize: TLV vs JSON 序列化对比
# ============================================================
BENCH_SERIALIZE="./build/bench_serialize"
if [ -f "$BENCH_SERIALIZE" ]; then
    echo -e "${YELLOW}[1/3] TLV vs JSON 序列化对比...${NC}"
    $BENCH_SERIALIZE 2>&1 | tee "$OUTPUT_DIR/serialize_tlv_vs_json.log"
    echo ""
else
    echo -e "${RED}[1/3] bench_serialize 不存在，跳过${NC}"
fi

# ============================================================
# 2. test_proto_vs_tlv: Protobuf vs TLV 对比
# ============================================================
TEST_PROTO_VS_TLV="./build/test_proto_vs_tlv"
if [ -f "$TEST_PROTO_VS_TLV" ]; then
    echo -e "${YELLOW}[2/3] Protobuf vs TLV 对比测试...${NC}"
    $TEST_PROTO_VS_TLV 2>&1 | tee "$OUTPUT_DIR/serialize_proto_vs_tlv.log"
    echo ""
else
    echo -e "${RED}[2/3] test_proto_vs_tlv 不存在，跳过${NC}"
fi

# ============================================================
# 3. 不同消息大小的 Protobuf 单独测试
# ============================================================
echo -e "${YELLOW}[3/3] Protobuf 不同消息大小序列化测试...${NC}"

# 用 bench_serialize 的数据（如果存在）提取关键数据
echo "从日志中提取序列化基准数据..."

# ============================================================
# 生成报告
# ============================================================
cat > "$OUTPUT_FILE" << EOF
# 03 — 序列化性能基准报告

> 生成时间: $TIMESTAMP

---

## 1. 测试目的

评估 Protobuf 序列化/反序列化的性能基线，作为网络压测数据的参考——区分"瓶颈在网络IO"还是"瓶颈在序列化"。

## 2. 测试方法

- bench_serialize: 测试 TLV 和 JSON 在不同消息大小下的编解码性能
- test_proto_vs_tlv: 对比 Protobuf 和 TLV 的序列化性能
- 测试维度: 编码耗时(ns)、解码耗时(ns)、编码吞吐量(MB/s)、序列化后体积(B)

## 3. 基准数据

### 3.1 编码/解码耗时（ns）

| 消息类型 | 消息大小 | TLV编码(ns) | TLV解码(ns) | JSON编码(ns) | JSON解码(ns) | TLV vs JSON |
|----------|----------|-------------|-------------|--------------|--------------|-------------|
| 小 (2×int) | ~40B | - | - | - | - | - |
| 中 (10 fields) | ~100B | - | - | - | - | - |
| 大 (10KB str) | ~10KB | - | - | - | - | - |

### 3.2 序列化后体积对比

| 消息类型 | TLV 体积 | JSON 体积 | 压缩比 |
|----------|----------|-----------|--------|
| 小 (2×int) | - | - | - |
| 中 (10 fields) | - | - | - |
| 大 (10KB str) | - | - | - |

## 4. 在压测场景中的应用

- 如果序列化耗时 < 网络RTT的5%，则瓶颈在网络IO，优先优化网络参数
- 如果序列化耗时 > 网络RTT的20%，则序列化是显著瓶颈，需要优化

## 5. 结论

（待填入，基于实际数据）
EOF

echo ""
echo -e "${GREEN}序列化基准报告: $OUTPUT_FILE${NC}"
echo ""
echo "下一步: 火焰图分析 / 瓶颈定位"
