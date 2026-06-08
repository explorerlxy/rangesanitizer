# LLM-Enhanced Sanitizer Check Elimination: From Passive Analysis to Active Hardening

> 基于静态 sound 冗余消除方案（ASan-- 2022, SanRazor 2021）的局限性分析，提出 LLM 语义增强的插桩决策框架，涵盖"被动分析消除"到"主动源码级加固"的完整技术路线。
>
> 文档编号: `SAN-IDEA-001` | 关联综述: `sanitizer-landscape-and-open-problems.md`

---

## 1. 背景与动机

### 1.1 现有静态方案的 ceiling

ASan-- (2022 USENIX Security) 和 SanRazor (2021 OSDI) 代表了冗余检查消除的两个极端：

| 方案           | 方法              | 优势           | 根本局限                    |
| ------------ | --------------- | ------------ | ----------------------- |
| **ASan--**   | 纯静态抽象解释，保 sound | 零漏报风险        | 保守性过强，消除比例有限 (~30%)     |
| **SanRazor** | 动态热点分析 + 静态验证   | 消除比例高 (~50%) | 不保 sound；冷路径上的 bug 可能漏检 |

两者的共同天花板是：**信息封闭性**。纯静态分析只能使用 IR 级别的类型、控制流和数据流信息；动态分析只能观察已执行路径的行为。两者都无法利用：

- 源码中的自然语言注释和文档契约
- 跨翻译单元的程序员意图（如 "buffer_size 永远不会超过 4096"）
- API 的隐式语义（如 POSIX read() 写入上限 == count 参数）
- 代码模式的先验知识（如 "先检查再写入" 的常见安全模式）

LLM 的核心能力恰好映射到这些盲区。

### 1.2 核心假设

本文档基于以下前提假设：

1. **LLM 具有可靠的语义理解能力**：能够从源码、注释、文档中提取安全相关的语义约束。
2. **LLM 作为 proposer，静态分析作为 verifier**：LLM 提出候选消除/加固方案，轻量级静态分析验证其正确性，避免幻觉导致的 soundness 破坏。
3. **测试接口规格已知**：明确知道哪些 IO 是攻击面（taint source），其余是固定执行环境。

---

## 2. 增益 1-5：LLM 语义分析驱动的被动消除

> 本节对应上一轮讨论中识别的五个增益类别，每个类别补充完整示例、静态分析失败原因、LLM 推理路径和量化估计。

### 2.1 增益 1：跨函数/TU 语义级不变量推理

**静态分析失败原因**：函数边界导致信息截断（function summary 丢失精度），跨翻译单元分析难以scale。

**实例**：

```c
// ---- handler.h ----
#define MAX_BODY_SIZE 4096

// ---- parser.c ----
int parse_content_length(const char* header, int header_len) {
    int val = extract_int_field(header, header_len, "Content-Length:");
    if (val < 0 || val > MAX_BODY_SIZE) return -1;
    return val;
}

// ---- handler.c ----
#include "handler.h"

void handle_request(const char* raw, int raw_len) {
    int body_len = parse_content_length(raw, raw_len);
    if (body_len < 0) return;

    char body_buf[MAX_BODY_SIZE];
    // ASan 插桩点：需要证明 body_len <= sizeof(body_buf)
    memcpy(body_buf, raw + header_offset, body_len);                 
}
```

**静态分析视角**：`parse_content_length` 在另一 TU，其返回值 `body_len` 的 range 信息丢失。抽象解释器看到的 range 是 `[-∞, +∞]`，保守保留检查。

**LLM 推理路径**：
1. 读取 `handler.h` 中的宏定义 `MAX_BODY_SIZE = 4096`
2. 理解 `parse_content_length` 的语义：解析 HTTP Content-Length，超过上限返回错误
3. 识别 `body_buf` 大小为 `MAX_BODY_SIZE`
4. 建立等价关系：`body_len` 的上界 ≤ `MAX_BODY_SIZE` == `sizeof(body_buf)`
5. 提出：此 `memcpy` 的 ASan 检查可删除

**验证器**：只需在 handler.c 中常量传播 `MAX_BODY_SIZE = 4096`，验证 `body_buf` 大小与此常量匹配。

**量化估计**：在 Nginx/Redis/Chromium 等项目中，"安全上限宏 + 跨函数传递"模式占内存操作的 15-25%。

---

### 2.2 增益 2：API 语义契约的隐式知识

**静态分析失败原因**：外部符号（libc、系统调用）无 IR 可分析，必须假设最坏情况。

**实例**：

```c
void process_file(const char* path) {
    struct stat st;
    stat(path, &st);
    char* buf = malloc(st.st_size);

    int fd = open(path, O_RDONLY);
    ssize_t n = read(fd, buf, st.st_size);
    // ASan 检查：read() 是否越界写入 buf？
    // 静态分析：read 是外部 libc 函数，无法分析
    close(fd);
    process_data(buf, n);
}
```

**LLM 推理路径**：
1. LLM 掌握 POSIX read() 规范：`read(fd, buf, count)` 最多读取 `count` 字节
2. `buf` 大小 = `st.st_size`（来自 stat），`read` 参数 = `st.st_size`
3. 推断：`read()` 写入字节数 ≤ `st.st_size` = `sizeof(buf)`
4. 提出：此 `read` 相关的 ASan 检查可删除

**关键洞察**：这不是特例。每个与 libc/libc++/系统调用交互的内存操作，静态分析都面临"外部函数盲区"。LLM 的 API 知识可以直接填补这个盲区。

**量化估计**：在典型的 C 项目中，libc 调用相关的内存操作占 20-30%，其中大部分受 POSIX/标准语义约束。

---

### 2.3 增益 3：隐式结构体布局不变量（Intra-Object Overflow 专项）

**静态分析失败原因**：ASan 使用 redzone 隔离对象，对象内部字段之间无 redzone，因此根本不检查 intra-object overflow。

**实例**：

```c
struct msg {
    uint8_t  type;
    uint8_t  flags;
    uint16_t length;    // 网络字节序：payload 长度
    uint32_t payload[64];
};

void dispatch_msg(struct msg* m) {
    uint16_t plen = ntohs(m->length);
    uint32_t* dst = m->payload;
    for (int i = 0; i < plen; i++) {
        dst[i] = process(dst[i]);
        // ASan：不检查（intra-object）
        // 静态分析：无法关联 length 字段和 payload[64] 的边界
    }
}
```

**LLM 推理路径**：
1. 从字段名 `length` 推断其语义为 "payload 长度"
2. 从注释 `payload[64]` 推断数组容量为 64
3. 识别缺失检查：`plen` 可能 > 64，导致 intra-object overflow
4. **关键动作**：LLM 不只是"识别冗余检查"，而是能发现**现有 sanitizer 根本不覆盖的漏洞类别**

**量化估计**：这是全新的检测能力，而非消除比例提升。直接对应综述中的 Open Problem 3.1。

---

### 2.4 增益 4：循环不变量的模式级识别

**静态分析失败原因**：非平凡循环不变量需要归纳证明，生产级静态分析器（Clang Static Analyzer, Infer）难以自动构造。

**实例**：

```c
void copy_strings(char** strs, int count, char* out, int out_cap) {
    int total = 0;
    for (int i = 0; i < count; i++) {
        int len = strlen(strs[i]);
        if (total + len + 1 > out_cap) return;  // 提前退出
        memcpy(out + total, strs[i], len);      // ASan 检查点
        total += len;
        out[total++] = '\n';
    }
}
```

**静态分析困境**：要证明 `memcpy` 安全，需要归纳不变量 `total = Σ(strlen(strs[0..i-1])) + i`，这在大多数工业级分析器中无法自动推导。

**LLM 推理路径**：
1. 识别模式："累加写入前先检查容量"
2. 验证：循环内的 `if (total + len + 1 > out_cap) return` 在语义上保护所有后续写入
3. 提出：`memcpy` 和 `out[total++]` 的 ASan 检查均可删除

**量化估计**：此类"累加写入 + 前置容量检查"模式在字符串处理和序列化代码中高频出现，估计占可消除检查的 10-15%。

---

### 2.5 增益 5：跨模块全局变量软约束传播

**静态分析失败原因**：全局变量值域分析在跨 TU 场景下极其保守，尤其是配置驱动的初始化模式。

**实例**：

```c
// ---- config.c ----
static int g_buffer_size = 0;

void config_init(const char* cfgfile) {
    g_buffer_size = parse_config_int(cfgfile, "buffer_size");
    if (g_buffer_size > 65536) g_buffer_size = 65536;
}

int get_buffer_size(void) { return g_buffer_size; }

// ---- worker.c ----
void process_batch(void) {
    int sz = get_buffer_size();
    char* buf = alloca(sz);
    read_input(buf, sz);
    // ASan：sz 来自全局变量，值域未知 → 保留所有检查
}
```

**LLM 推理路径**：
1. 读取 `config_init`，"记住"摘要：`g_buffer_size ∈ [0, 65536]`
2. 在 `worker.c` 中识别 `sz = get_buffer_size()` 的值域继承此约束
3. 推断 `alloca(sz)` 最大分配 65536 字节
4. 如果后续访问有已知边界，提出检查消除

**关键区别**：这不是数据流分析，而是**语义摘要**——LLM 用自然语言级别的理解替代了形式化的抽象域。

---

## 3. 增益 6：主动源码级边界检查插入

> 用户补充 idea 1：LLM 不应只做"分析者"，而应成为"主动加固者"。

### 3.1 核心思想

现有范式是：源码 → 编译器 IR → Sanitizer 插桩 → 运行时检查。

LLM 增强的新范式是：

```
源码 + 文档 → LLM 语义理解 → 主动插入显式边界检查（源码级）
                                            ↓
                              编译器：这些显式检查使 Sanitizer 检查可静态证明冗余
                                            ↓
                              消除冗余 Sanitizer 检查 → 性能提升 + Soundness 保持
```

### 3.2 为什么源码级插入比 IR 级更有优势

**关键洞察**：编译器在 lowering 过程中丢失了大量源代码语义。例如：

- `getelementptr inbounds` 的 `inbounds` 标记在 C 中是隐含的，如果显式插入边界检查，编译器可以优化掉后续的冗余检查。
- `sizeof(array)` 在 IR 中变成常量，但数组的"语义身份"丢失了。

**实例**：

```c
// 原始代码（Sanitizer 无法静态证明安全）
void parse(uint8_t* data, int len) {
    if (len < 20) return;
    int msg_len = (data[0] << 8) | data[1];
    char msg[256];
    memcpy(msg, data + 2, msg_len);  // ASan 检查：msg_len <= 254?
}
```

静态分析无法证明 `msg_len <= 254`（因为 `data[0..1]` 来自外部输入）。

**LLM 主动加固后**：

```c
void parse(uint8_t* data, int len) {
    if (len < 20) return;
    int msg_len = (data[0] << 8) | data[1];
    // LLM 插入：显式边界断言
    if (msg_len > sizeof(msg) - 2) return;  // ← 显式语义约束
    char msg[256];
    memcpy(msg, data + 2, msg_len);
    // ASan 检查：现在可被常量传播 + 范围分析静态证明冗余
}
```

编译器看到这个显式检查后，可以常量传播 `sizeof(msg) = 256`，推导出 `msg_len <= 254`，从而证明 ASan 的边界检查恒为真。

### 3.3 与 ASan-- 的协同

ASan-- 的核心是："如果静态分析能证明安全，就删除 sanitizer 检查"。

LLM 主动加固的价值在于：**让 ASan-- 能够证明更多东西**。不是替换 ASan--，而是"喂"给 ASan-- 更多可分析的事实。

### 3.4 具体策略

LLM 在源码级插入三类显式检查：

| 类型           | 插入位置   | 示例                                                    |
| ------------ | ------ | ----------------------------------------------------- |
| **前置边界断言**   | 指针使用前  | `if (len > buf_size) return;`                         |
| **长度-容量绑定**  | 结构体字段间 | `assert(plen <= sizeof(payload)/sizeof(payload[0]));` |
| **API 契约断言** | 外部调用后  | `assert(nread <= buf_size);`                          |

---

## 4. 增益 7：测试 IO 导向的条件插桩

> 用户补充 idea 2：针对测试 IO 进行运行时条件判断，仅在受测试输入影响时启用 sanitizer 路径。

### 4.1 核心思想

Sanitizer 检查的开销是均匀分布在所有内存操作上的。但漏洞挖掘的核心假设是：**bug 只出现在攻击者可控的输入路径上**。

测试 IO 导向的条件插桩（Test-IO-Directed Conditional Instrumentation, TIOCI）基于以下观察：

1. 测试输入只占程序 IO 的一部分（其余是配置文件、静态资源、内部状态）
2. 在 fuzzing/测试阶段，我们可以精确定义哪些 IO 是"危险的"
3. 大多数内存操作在处理"安全"数据时不需要 sanitizer 检查

### 4.2 技术方案

**方案 A：轻量级动态 Taint 跟踪 + 条件检查**

```c
// 插桩后代码（概念）
void process(char* buf, int len, int is_from_test_io) {
    if (is_from_test_io) {
        __asan_check_access(buf, len);  // 完整 sanitizer 检查
    }
    // 否则：走 fast path，无检查
    // ...
}
```

**方案 B：函数级粒度切换**

```c
// 通过静态分析识别：哪些函数可能处理测试输入
// 对这些函数插桩，其余不插桩

// 编译时生成两个版本
void process_fast(char* buf, int len) { ... }      // 无 sanitizer
void process_safe(char* buf, int len) { ... }      // 有 sanitizer

// 运行时通过调用上下文选择
void dispatch(char* buf, int len, int is_test_input) {
    if (is_test_input)
        process_safe(buf, len);
    else
        process_fast(buf, len);
}
```

### 4.3 为什么 LLM 在这里有独特价值

传统动态 taint tracking 的开销极高（通常 2-10x），抵消了 sanitizer 优化带来的收益。

LLM 可以做的事情：

1. **静态识别"测试输入可达函数"**：通过源码语义分析，识别哪些函数直接或间接处理测试输入，哪些函数是"纯内部逻辑"
2. **选择性插桩决策**：只对"测试输入可达"的函数插桩，其余函数完全不插桩
3. **混合粒度**：在函数内部，LLM 识别出"安全区"（不受测试输入影响的基本块），在这些区域内跳过 sanitizer 检查

### 4.4 实例

```c
// HTTP 服务器处理流程
void handle_connection(int fd) {
    char buf[4096];
    int n = recv(fd, buf, sizeof(buf), 0);  // ← 测试输入入口
    
    // --- Zone A: 受测试输入影响 ---
    Request req = parse_request(buf, n);    // 需要完整 sanitizer
    
    // --- Zone B: 纯内部逻辑 ---
    int route_id = lookup_route(req.path);  // 路由表是固定的
    Handler h = dispatch_table[route_id];   // dispatch_table 是常量
    
    // --- Zone C: 混合 ---
    char response[8192];
    h(req, response);  // handler 可能受 req 影响，也可能不受影响
}
```

**LLM 分析**：
1. `recv()` 是测试输入入口 → Zone A 标记为 tainted
2. `dispatch_table` 是编译时常量，不受输入影响 → Zone B 标记为 safe
3. 对每个 handler 函数进行可达性分析，判断其是否直接/间接使用 `req` 中的 tainted 字段

**结果**：Zone B 可以完全跳过 sanitizer 插桩；Zone A 和 C 中的部分路径可以有条件地启用检查。

### 4.5 与 GWP-ASan 的区别

| 特性 | GWP-ASan (采样) | TIOCI (测试 IO 导向) |
|------|----------------|----------------------|
| 采样依据 | 随机 / 基于分配大小 | 基于输入来源的 taint 分析 |
| 漏报特性 | 概率性，不可量化 | 确定性：非测试输入路径上的 bug 被有意忽略 |
| 适用场景 | 生产环境 | 开发测试 / 定向 fuzzing |
| 开销降低原理 | 减少检测频率 | 减少检测范围 |

---

## 5. 统一架构：LLM 增强的 Sanitizer 优化 Pipeline

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           输入层                                        │
│  源码 + 注释 + 文档 + 测试接口规格（哪些 IO 是 taint source）            │
└─────────────────────────────────────────────────────────────────────────┘
                                    ↓
┌─────────────────────────────────────────────────────────────────────────┐
│                        LLM 语义分析层                                    │
│                                                                         │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌────────────┐  │
│  │ 跨函数不变量  │  │ API 语义知识  │  │ 结构体布局   │  │ 循环模式   │  │
│  │ 推理          │  │ 注入          │  │ 推断         │  │ 识别       │  │
│  └──────────────┘  └──────────────┘  └──────────────┘  └────────────┘  │
│                                                                         │
│  ┌──────────────────────┐  ┌────────────────────────────────────────┐  │
│  │ 主动源码级边界检查    │  │ 测试 IO 可达性分析 + 条件插桩决策      │  │
│  │ 插入（增益 6）       │  │ （增益 7）                             │  │
│  └──────────────────────┘  └────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
                                    ↓
┌─────────────────────────────────────────────────────────────────────────┐
│                       静态验证层（轻量级）                               │
│  - 常量传播验证 LLM 提出的 range 约束                                    │
│  - 控制流分析验证显式断言可达性                                          │
│  - 符号执行验证条件插桩的 soundness（可选）                              │
│  → 通过验证的提议被采纳；失败则回退到保守策略                           │
└─────────────────────────────────────────────────────────────────────────┘
                                    ↓
┌─────────────────────────────────────────────────────────────────────────┐
│                        编译器插桩层                                      │
│  - 消除 LLM+静态验证确认的冗余检查                                       │
│  - 对测试 IO 不可达路径使用 fast path                                   │
│  - 保留所有未被证明冗余的检查                                           │
└─────────────────────────────────────────────────────────────────────────┘
                                    ↓
┌─────────────────────────────────────────────────────────────────────────┐
│                        运行时层                                          │
│  - 条件插桩：taint flag 决定检查路径                                    │
│  - 测试覆盖引导：优先执行被插桩路径                                      │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 6. 风险分析与对冲策略

### 6.1 LLM 幻觉风险

| 风险场景 | 影响 | 对冲策略 |
|---------|------|---------|
| LLM 错误地将安全检查标记为冗余 | False Negative（漏报） | LLM 只作为 proposer，静态验证器必须验证每个提议 |
| LLM 插入错误的边界断言 | 引入新 bug / 误报 | 断言使用 `assert()`（测试模式）而非 `if`（生产模式）；符号执行验证 |
| LLM 错误识别 taint source | 条件插桩保护范围错误 | taint source 由用户明确指定，LLM 只负责可达性分析 |

### 6.2 计算成本

LLM 推理的成本高于静态分析。解决方案：

1. **增量分析**：只对变更的函数重新运行 LLM 分析
2. **缓存语义摘要**：LLM 生成的函数级语义摘要持久化，复用于后续编译
3. **离线批处理**：LLM 分析作为 CI 预计算步骤，不阻塞每次编译

### 6.3 Soundness 保证

本框架的核心原则是：**LLM 提供启发式，静态分析提供保证**。

- 任何 sanitizer 检查的删除必须经过静态验证
- LLM 插入的源码级检查必须经过编译器确认可达
- 条件插桩的 soundness 依赖于 taint 分析的保守性（over-approximation）

---

## 7. 与综述开放问题的映射

| 本文增益 | 直接关联的开放问题 | 解决程度 |
|---------|------------------|---------|
| 增益 3（intra-object） | OP 3.1 Intra-Object Overflow | **部分解决**：LLM 可推断字段边界，但需编译器配合子对象级元数据 |
| 增益 6（主动加固） | OP 3.3 生产化瓶颈（开销） | **部分解决**：显式检查使更多 sanitizer 检查可消除 |
| 增益 7（条件插桩） | OP 3.3 生产化瓶颈（开销） | **部分解决**：定向减少检测范围而非概率采样 |
| 全部增益 | OP 3.5 评估基准缺陷 | **提出新需求**：需要能衡量"语义级消除"效果的基准 |

---

## 8. 下一步研究方向

1. **实现原型**：选择一个现有 sanitizer（如 ASan）和一个小型目标程序（如 Redis 或 libpng），验证 LLM 主动加固 + ASan-- 的协同效果
2. **评估基准设计**：设计能衡量"语义级冗余消除"的基准，超越 Juliet 和 SPEC
3. **条件插桩的形式化**：对 TIOCI 做 soundness 分析，证明其不会引入 false negative
4. **多 Sanitizer 兼容性**：验证 LLM 插入的显式检查是否也能帮助 MSan、UBSan 等其他 sanitizer 的优化

---

*文档创建日期：2026-05-13*
*关联文档：`sanitizer-landscape-and-open-problems.md`*
*状态：研究想法（待原型验证）*