# WSL2 DrvFs 下 perf record 失败 — Bad Address

**发现时间**：2026-07-27  
**发现场景**：在 WSL2 中运行 `perf record` 采集 CPU 热点数据  
**严重程度**：低（工具/环境问题，非代码 Bug）

## 问题描述

在 `/mnt/d/CLion/rpc/`（Windows 挂载到 WSL2 的目录）下运行：

```bash
perf record -F 99 -g --call-graph dwarf -o perf.data -- ./rpc
```

perf 报错：

```
failed to write perf data, error: Bad address
```

## 根因

WSL2 通过 DrvFs（9P 文件系统协议）挂载 Windows 驱动器。9P 协议**不支持 `mmap` 写操作**。perf 使用 `mmap` 作为其内部数据缓冲区的写机制——它在 `mmap` 映射的内存区域中直接构造 profiling 数据，然后 flush 到文件。当输出路径在 DrvFs 上时，`mmap` 返回 `-EFAULT (Bad address)`。

这是 **WSL2 的已知限制**，不是 perf 的 bug。

## 修复

将 perf 输出重定向到 WSL2 原生 ext4 文件系统（`/tmp` 或 `~`）：

```bash
# 错误 — DrvFs 路径
perf record -o /mnt/d/CLion/rpc/perf.data -- ./rpc

# 正确 — WSL2 原生路径
perf record -o /tmp/perf.data -- ./rpc

# 脚本也应使用原生路径
perf script -i /tmp/perf.data | ~/FlameGraph/stackcollapse-perf.pl > /tmp/perf.folded
```

同时，FlameGraph 脚本也需要安装在 WSL2 原生路径（`~/FlameGraph`），而非 `/mnt/d/...`。

## 经验教训

1. **WSL2 的 `/mnt/c/` 和 `/mnt/d/` 不等于 Linux 文件系统**：9P 协议有功能限制（无 mmap、无 inotify、性能差）。所有 Linux 原生工具（perf、strace、gdb 的某些功能）都应该在 WSL2 原生路径下运行
2. **编译产物放 `/mnt/` 没问题，工具链放原生路径**：编译器读写文件用的是 `read/write`（9P 支持），perf 用的是 `mmap`（9P 不支持）
3. **脚本中硬编码路径时要区分 DrvFs vs ext4**：`/mnt/d/` 前缀的路径只适用于 `git`、`cmake`、`make`，不适用于 `perf`、`strace -o`、`gdb -write-core`
