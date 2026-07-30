# TTY 同步日志 — 300 连接时"假死"的性能陷阱

**发现时间**：2026-07-28  
**发现场景**：perf + 火焰图分析匹配系统 300 并发时，服务端 QPS 骤降，表现如同"卡死"  
**严重程度**：高（性能悬崖，但非功能 Bug）

## 问题描述

在 300 并发匹配压测时，服务端性能出现断崖式下降。火焰图左侧出现一个巨大的"尖塔"：

```
tty_write → pty_write → queue_work → __raw_spin_lock → __pv_queued_spin_lock_slowpath
```

这是一条**完全在内核态**的调用链：用户态 `printf`/`std::cout` → 内核 TTY 驱动 → 自旋锁慢路径。

**根因**：Linux 的 TTY 输出被一个内核自旋锁保护，同一时刻只有一个进程/线程能向同一个终端写入。300 个连接的业务线程都在 `printf` 调试日志，全部排队等 TTY 锁 → CPU 空转在内核自旋锁上 → 服务端"假死"。

## 火焰图识别特征

```
正常服务：                           TTY 瓶颈：
[====网络栈====]                     [尖塔         ][====网络栈====]
[== 业务逻辑 ==]                     [tty_write     ][== 业务逻辑 ==]
[== epoll  ==]                       [pty_write     ][== epoll  ==]
                                     [__spin_lock   ]
                                     [_slowpath     ]
```

左侧孤立的窄而高的尖塔是 TTY 锁竞争的经典特征。

## 修复

### 压测时

```bash
# 重定向 stdout/stderr 到 /dev/null
./rpc > /dev/null 2>&1

# 或运行时关闭输出
./rpc --quiet
```

### 生产代码

```cpp
// 之前：每次请求都打印
printf("[DEBUG] Request from fd=%d, method=%s\n", fd, method.c_str());

// 之后：条件编译或日志级别控制
#ifdef DEBUG_LOG
    printf("[DEBUG] Request from fd=%d, method=%s\n", fd, method.c_str());
#endif
```

或使用异步日志库（spdlog async mode），将日志写入操作从 IO 线程剥离。

## 经验教训

1. **`printf` 不是免费的**：在 500 QPS 时无影响，在 50,000 QPS 时每个 `printf` 都在抢内核锁
2. **火焰图是诊断此类问题的唯一有效工具**：QPS 骤降 + 延迟飙升，靠看代码无法定位到 TTY 锁——只有火焰图能把内核态调用链完整展示出来
3. **压测时必须关闭调试输出**：这是性能测试的基本原则，但因太基础反而容易被忽略
4. **日志级别是可维护性的基础设施**：DEBUG/INFO/WARN/ERROR 分级 + 运行时切换，应该在项目早期就建立
