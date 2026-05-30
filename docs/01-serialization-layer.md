# 01 — RPC 序列化层：原理与 TLV 编码

---

## 一、为什么需要序列化层？

RPC 调用 `stub->Add(3, 5)` 运行在客户端进程内存里，但 `Add` 函数的实际执行体在另一台机器上。需要把两样东西通过网络发过去：

1. **调用哪个函数** — `"Add"`
2. **传什么参数** — 整数 `3` 和整数 `5`

内存里的 `int` 是一个 4 字节的二进制值，需要放到 TCP 字节流里：

```cpp
int a = 3;
// 内存里看起来是：0x03 0x00 0x00 0x00（小端）
// 还是：         0x00 0x00 0x00 0x03（大端）？
// 接收方怎么知道你发的是 int 还是 float？
// 如果你发的是 string "hello"，接收方怎么知道这个字符串有多长？
```

**序列化层存在的唯一目的：把内存中任意类型的 C++ 对象，转换成一段「接收方能无歧义还原」的字节序列，以及反过来。**

---

## 二、为什么不用 JSON？

| 开销项 | JSON | 二进制 |
|--------|------|--------|
| 序列化 `int 3` | 1 字节的数字 + 可能的逗号 | 4 字节 |
| 序列化 `int 123456789` | `"123456789"` = 9 字节 | 4 字节 |
| 序列化 `double 3.14` | `"3.14"` → 至少 4 字节，还要解析浮点数字符串 | 8 字节，内存直接 memcpy |
| 方法名字符串 `"Add"` | 每次请求都要发 3 字节 + 引号 | 可以用 1 字节的方法 ID 映射 |
| 反序列化开销 | 字符串解析、数字转换 | 直接 memcpy + 字节序转换 |

**核心差距**：JSON 把「解析数据格式」的工作留给了接收方，二进制序列化在发送方就把数据组织成接收方能直接消费的布局。

序列化/反序列化可能占总耗时的 30%-50%，在一个有链式调用的 RPC 系统中会被放大。

---

## 三、TLV 编码详解

TLV = **Type-Length-Value**。

### 单个字段的编码布局

```
┌──────────┬──────────────┬─────────────────┐
│  Type    │   Length     │     Value       │
│ (1 byte) │  (4 bytes)   │  (N bytes)      │
└──────────┴──────────────┴─────────────────┘
```

**Type（1 字节）**：告诉接收方"接下来这串字节是什么类型"

| 值 | 类型 |
|----|------|
| 0x01 | int32_t |
| 0x02 | int64_t |
| 0x03 | float |
| 0x04 | double |
| 0x05 | string |
| 0x06 | bool |

接收方看到 `0x01`，就知道后面 4 字节应该按 `int32_t` 来解释。没有这个标记，接收方差一个字节就会导致后续所有数据错位——这是二进制协议最常见的灾难性 bug。

**Length（4 字节）**：告诉接收方"Value 部分有多长"。对于定长类型（如 int32 永远是 4 字节），Length 看起来冗余，但保留它的好处是：**编解码逻辑完全统一**，不需要针对不同类型写特殊的读取逻辑。

**Value（N 字节）**：实际数据。对于整数/浮点数，是内存中的二进制表示（但要做字节序统一）。

### 完整请求示例：`Add(3, 5)`

```
参数1:  int32 值为 3          参数2:  int32 值为 5
┌──────┬──────────┬──────────┬──────┬──────────┬──────────┐
│ 0x01 │ 0x00000004│0x00000003│ 0x01 │ 0x00000004│0x00000005│
│ TYPE │  LENGTH   │  VALUE   │ TYPE │  LENGTH   │  VALUE   │
└──────┴──────────┴──────────┴──────┴──────────┴──────────┘
       ← 7 bytes per arg →        ← 7 bytes per arg →
                         总长度 = 14 bytes
```

对比 JSON `{"method":"Add","params":[3,5]}` = 30+ bytes。

---

## 四、字节序问题（大端 vs 小端）

### 问题

同一个 `int 3`，在不同 CPU 上内存布局不同：

```
大端（网络字节序）：高位字节存低地址
  地址:  [0]    [1]    [2]    [3]
  内容:  0x00   0x00   0x00   0x03

小端（x86 CPU）：低位字节存低地址
  地址:  [0]    [1]    [2]    [3]
  内容:  0x03   0x00   0x00   0x00
```

如果客户端是 x86（小端），服务端是 ARM（大端），客户端发 `0x03 0x00 0x00 0x00`，服务端按大端读出来变成 `0x03000000 = 50331648`——**3 变成了五千万**。

### 解决方案：统一用网络字节序（大端）

```cpp
uint32_t htonl(uint32_t hostlong);   // host to network long (4 bytes)
uint16_t htons(uint16_t hostshort);  // host to network short (2 bytes)
uint32_t ntohl(uint32_t netlong);    // network to host long
uint16_t ntohs(uint16_t netshort);   // network to host short
```

即使客户端和服务端都是 x86，也需要做字节序转换——**协议规范定义了网络传输用大端，不能假设对端 CPU 架构**。这是协议设计的稳健性原则。

---

## 五、为什么不直接用 Protobuf？

| | 自实现 TLV | Protobuf |
|------|------------|----------|
| 依赖 | 零外部依赖 | 需要装 protoc 编译器、链接 libprotobuf |
| 理解深度 | 理解每一字节的布局 | 理解 `.proto` 文件怎么写 |
| 面试话题 | 能讲 TLV → varint → protobuf 原理，成体系 | 只能说"我用了 protobuf"，追问细节就悬 |
| 代码量 | ~200 行 | 1 行 `syntax = "proto3";` |
| 错误处理 | 自己处理，面试能讲 | protobuf 替你处理，讲不出细节 |

**策略**：自实现 TLV，在 README 中写一节"与 Protobuf 的关系"，展示理解 protobuf 的 varint 编码是 TLV 的优化变体（Type 和 Length 合并到一个变长整数里）。

---

## 六、接口设计

```cpp
class Serializer {
public:
    // ========== 写入（序列化）==========
    void WriteInt32(int32_t value);
    void WriteInt64(int64_t value);
    void WriteFloat(float value);
    void WriteDouble(double value);
    void WriteString(const std::string& value);
    void WriteBool(bool value);

    // 获取序列化后的字节数据
    std::vector<uint8_t> GetBuffer() const;

    // ========== 读取（反序列化）==========
    explicit Serializer(const std::vector<uint8_t>& data);

    int32_t      ReadInt32();
    int64_t      ReadInt64();
    float        ReadFloat();
    double       ReadDouble();
    std::string  ReadString();
    bool         ReadBool();

private:
    std::vector<uint8_t> buffer_;  // 数据缓冲区
    size_t read_pos_;              // 当前读位置（反序列化时用）

    void     WriteUint32BigEndian(uint32_t value);
    uint32_t ReadUint32BigEndian();
};
```

### 设计决策

**为什么用成对 Write/Read 而不是模板？**
每种类型编码方式不同——整型/浮点是定长+字节序转换，string 是变长（先写长度再写内容）。成对方法让每种类型的编解码逻辑独立清晰，面试能讲。

**为什么内部用 `std::vector<uint8_t>` 而不是裸指针？**
- 自动管理内存增长，无需预估 buffer 大小
- 消除缓冲区溢出风险
- 上层直接取 `buffer.data(), buffer.size()` 传给 socket send

**为什么用内部读指针而非手动传索引？**
读指针由 Serializer 自己维护，消除调用者手动管理偏移的出错可能——这是封装的根本意义。

---

## 七、错误处理

反序列化时可能发生的错误：

1. 读 int32 时，剩余字节不足 4
2. Type 标记不认识（不是 0x01~0x06）
3. Length 声明 100 字节，但实际剩余只有 10 字节

使用 `std::optional` 处理错误情况，避免异常对 RPC 性能的影响：

```cpp
std::optional<int32_t> ReadInt32();
std::optional<std::string> ReadString();
```

---

## 八、和协议帧层的关系

序列化层只负责**单个参数/返回值的编解码**。协议帧层管"元信息"（方法名、请求ID、消息类型）。

```
客户端调用 Add(3, 5)
  │
  ├─ 协议帧层：构造帧头
  │    魔数 0xBABE | 总长度 | 请求ID=1 | 消息类型=0x01 | 方法名"Add"
  │
  └─ 序列化层：构造参数 body
       Serializer ser;
       ser.WriteInt32(3);   // 参数1
       ser.WriteInt32(5);   // 参数2
       → body = ser.GetBuffer();  // 14 字节
  │
  └─ 帧头 + body → 发给服务端
```

```
服务端收到数据
  │
  ├─ 协议帧层：解析帧头
  │    验证魔数 → 读总长度 → 读请求ID → 读消息类型 → 读方法名 "Add"
  │
  └─ 序列化层：解析参数 body
       Serializer ser(body);
       int a = ser.ReadInt32();  // 3
       int b = ser.ReadInt32();  // 5
       → 调用真实的 Add(3,5) → 得到 8
```
