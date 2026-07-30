# Connection::OnClose() — Use-After-Free（自毁后访问成员）

**发现时间**：2026-07-25  
**发现场景**：压测 v0.11，50 并发时 `free(): chunks in smallbin corrupted`  
**严重程度**：致命（堆内存损坏，ASAN 精准定位 `connection.cpp:75`）

## 问题描述

`Connection::OnClose()` 先调用 `Unregister(fd_)`，其内部 `delete this` 销毁了 Connection 对象。紧接着访问 `fd_` 成员来调用 `close(fd_)` ——此时 `this` 已被释放，`fd_` 是访问已释放的内存。

## 错误代码

```cpp
void Connection::OnClose() {
    // ... 回调 disconnect 等 ...

    loop_->Unregister(fd_);   // 内部 delete this → Connection 已销毁！
    close(fd_);               // ← UAF：fd_ 所在内存已释放
}
```

## 正确代码

```cpp
void Connection::OnClose() {
    // ... 回调 disconnect 等 ...

    int fd = fd_;             // 1. 先保存 fd 到局部变量
    fd_ = -1;                 // 2. 标记无效，防止析构函数再次 close
    loop_->Unregister(fd);    // 3. 内部 delete this
    close(fd);                // 4. 用局部变量 close，安全
}
```

关键改动：**在 `delete this` 之前，把需要的成员值拷贝到栈上局部变量**。

## 经验教训

1. **`delete this` 之后的任何成员访问都是 UAF**：即使只是读一个 `int` 成员也不行
2. **栈上局部变量是自毁模式的安全网**：`int fd = fd_` 拷贝到寄存器/栈，对象销毁后仍有效
3. **设置 `fd_ = -1` 防止析构函数二次 close**：RAII 析构时 `if (fd_ >= 0) close(fd_)` 是常规实践
4. **事件驱动编程中，回调内自毁是常见模式**：Netty、libuv、boost.asio 都用类似方式，关键是掌握"先拷贝再自毁"的写法
