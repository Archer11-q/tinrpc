# Connection::Send 缓冲区追加越界 Bug

**发现时间**：2026-07-14  
**发现场景**：编写游戏房间端到端流程测试时审查 `Connection::Send()` 实现  
**严重程度**：高（特定条件下数据损坏/崩溃）

## 问题描述

`Connection::Send()` 在处理连续两次 `Send()` 调用时，第二次调用会使用第一次调用的 `write_offset_` 作为新数据 `data` 的偏移量，导致：

1. **越界访问**：如果新 `data` 长度小于遗留的 `write_offset_`，`data.begin() + write_offset_` 会越界
2. **数据丢失**：如果新 `data` 长度大于 `write_offset_`，会跳过前面 `write_offset_` 字节的数据

## 触发条件

必须同时满足：
1. 第一次 `Send()` 返回部分发送（`n > 0 && n < data.size()`）
2. 第一次 `Send()` 的 `write_offset_` 还未被 `OnWrite()` 清零
3. 同一条连接上再次调用 `Send()` 追加新数据

在本项目中，由于 localhost TCP 缓冲区充足、帧体积小，`send()` 几乎总是全部成功或返回 EAGAIN（两者都不会设置非零 `write_offset_`），因此该 bug **在现有测试中未被实际触发**，但属于潜在的代码缺陷。

## 修复前代码

```cpp
void Connection::Send(const std::vector<uint8_t>& data) {
    if (write_buffer_.empty()) {
        ssize_t n = send(fd_, data.data(), data.size(), MSG_NOSIGNAL);
        if (n == static_cast<ssize_t>(data.size())) {
            return;
        }
        if (n > 0) {
            write_offset_ = static_cast<size_t>(n);  // ← 记录第一次的偏移
        }
    }

    // BUG: write_offset_ 来自第一次，对第二次的 data 错误复用
    write_buffer_.insert(write_buffer_.end(),
                         data.begin() + static_cast<long>(write_offset_),
                         data.end());

    loop_->UpdateEvents(fd_, EPOLLIN | EPOLLOUT | EPOLLET);
}
```

**场景**：第一次 `Send(data1, 100B)`，`send()` 只发了 60B → `write_offset_ = 60`，`write_buffer_` 存入 `data1[60:]`（40B）。第二次 `Send(data2, 50B)`，`write_buffer_` 非空，跳过直接发送，追加时使用 `data2.begin() + 60` → **越界！**（data2 只有 50B）。

## 修复后代码

```cpp
void Connection::Send(const std::vector<uint8_t>& data) {
    if (write_buffer_.empty()) {
        ssize_t n = send(fd_, data.data(), data.size(), MSG_NOSIGNAL);
        if (n == static_cast<ssize_t>(data.size())) {
            return;                        // 全部发送完毕
        }
        if (n > 0) {
            // 部分发送：用当前 n 作为偏移，追加剩余部分
            write_buffer_.insert(write_buffer_.end(),
                                 data.begin() + n, data.end());
        } else {
            // EAGAIN：追加全部数据
            write_buffer_.insert(write_buffer_.end(),
                                 data.begin(), data.end());
        }
    } else {
        // 缓冲区已有待发送数据：新数据追加到末尾保序
        write_buffer_.insert(write_buffer_.end(), data.begin(), data.end());
    }

    loop_->UpdateEvents(fd_, EPOLLIN | EPOLLOUT | EPOLLET);
}
```

**关键改动**：
- `Send()` 不再使用成员变量 `write_offset_`（该变量仅由 `OnWrite()` 使用，追踪发送进度）
- 首次发送时用本地变量 `n` 直接计算未发送部分
- 缓冲区非空时直接追加全部新数据

## 经验教训

1. **成员变量语义要清晰**：`write_offset_` 的语义是"发送缓冲区中已发送的偏移量"，只在 `OnWrite()` 中有意义。`Send()` 用它做新数据的偏移，语义错误。
2. **局部变量优于成员变量**：能用局部变量就不要用成员变量，避免状态污染。
3. **连续发送场景需覆盖**：单元测试应覆盖"第一次部分发送 + 立即追加第二次"的场景——但这在高吞吐场景才容易触发，本地 localhost 很难复现。
