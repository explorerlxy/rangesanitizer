# RangeSanitizer (RSan) 研究工作总结

> 基于论文: *RangeSanitizer: Detecting Memory Errors with Efficient Range Checks*
> Floris Gorter, Cristiano Giuffrida — Vrije Universiteit Amsterdam
> USENIX Security 2025
> 源码: https://github.com/vusec/rangesanitizer

---

## 1. 核心动机

RSan 解决了一个根本矛盾：**Redzone-based sanitizer（如 ASan）兼容性好但无法做 range check；Pointer-based bounds checker 能做 range check 但兼容性差。** RSan 首次将 range check 能力带入 redzone 世界，实现了"两者的最优结合"。

### 关键洞察

传统 redzone sanitizer（ASan、FloatZone）检查一个内存范围（如 `memset(buf, 0, x)`）时必须**逐字节扫描** redzone，而 pointer-based bounds checker 只需**两次比较**（`buf >= base && buf+x <= bound`）。RSan 通过新颖的元数据格式，使 redzone sanitizer 也能用**一次元数据查找 + 一次比较**完成范围检查。

---

## 2. 核心设计

### 2.1 元数据布局：将 bound 信息存储在 underflow redzone 中

```
内存布局（两个相邻对象）:
┌──────────┬──────────────────────┬──────────┬──────────────────────┐
│ Redzone  │       Object N       │ Redzone  │       Object N+1     │
│(prev obj)│    (80 bytes)        │[meta: B] │                      │
└──────────┴──────────────────────┴──────────┴──────────────────────┘
            ▲ base of N                      ▲ base of N+1
                                             │
                                    bound(N) 存储在 base(N+1)-8
```

- 每个对象的 **upper bound**（`base + 原始请求大小`）存储在其后一个对象的 underflow redzone 中
- 元数据位置：**当前对象 base 的前 8 字节**（即 `base - 8`）
- 所有 redzone 插入在 size class slot 的**末尾**（而非开头），这保证了 object 起始地址与 slot 对齐，使得 underflow 检测自然生效

### 2.2 内存分配器：Power-of-Two Size Classes

```
char *buf = malloc(80);
// 1. 插入 redzone (+32)         → 112 bytes
// 2. 填充到 2 的幂 (128)         → 128 bytes, size class = 2^7
// 3. 存储 bound = base+80        → *(base-8) = base+80
// 4. 打标签 log2(128)=7 到指针高位 → buf = 0x7.....000
```

- 使用 **2 的幂** 作为 size class（而非 ASan 的 8 字节对齐），使得对齐计算变为简单的位运算
- 通过指针标签直接编码 size class（`log2(slot_size)`），不需要额外的查找表

### 2.3 指针打标签 (Pointer Tagging)

**显式打标签（Explicit Tagging）**——需要硬件支持：
- **Arm TBI** (Top-Byte Ignore)：8 位标签，从 bit 56 开始
- **Intel LAM** (Linear Address Masking)：6 位或 15 位标签（U57 或 U48 模式）
- 标签直接通过 `OR` 运算写入指针高位，硬件自动忽略这些位

**隐式打标签（Implicit Tagging）**——无需硬件支持（x86 legacy）：
- 重新划分 48 位用户地址空间（bits [46:41] 作为隐式标签）
- 每个 size class 占据专用的 2TB 虚拟地址区域
- 指针所处的地址范围**隐式编码**了其 size class
- 代价：最大对象大小限制为 2TB，内存开销更高（78% vs 42%）

### 2.4 Base Address 计算

```c
// Listing 1: Variable shift (Arm, AMD)
tag = ptr >> 41;       // 或 >> 56 on Arm
base = (ptr >> tag) << tag;

// Listing 2: bzhi + XOR (Intel x86)
tag = ptr >> 41;
base = ptr ^ bzhi(ptr, tag);   // bzhi 归零高位，XOR 归零低位
```

关键优化：Intel CPU 上变量移位性能差，用 `bzhi`（BMI 指令集）+ `XOR` 替代，将近减半了 sanitizer 总开销。

### 2.5 检查范式（Check Algorithm）

```c
// Listing 3: 完整检查
tag = ptr >> 41;                    // 1. 提取 size class
base = ptr ^ bzhi(ptr, tag);        // 2. 计算 base address
bound = *(base - 8);                // 3. 读取 upper bound 元数据
if (ptr + offset > bound) {         // 4. 一次比较完成检查
    if (tag != 0) {                 // 5. slow-path: 排除 untagged 内存
        error!                      // 6. 检测到 bug
    }
}
```

**一个检查覆盖三种漏洞**：
- **Buffer overflow**：`ptr+offset > bound` 时触发
- **Buffer underflow**：underflow 后 `align_down` 会落到前一个对象上，读取其 bound，任何正地址都会 > 该 bound
- **Use-after-free**：释放时将 bound 设为 0，后续任何访问都会因 `addr > 0` 而触发错误

### 2.6 Fake Object 与 Guard Page

- 每个 size class 必须以一个 **fake object** 作为前缀，承载第一个真实对象的元数据
- 为避免 fake object 消耗过多内存（大 size class 情况），用 **guard page** 替代

### 2.7 Heap Quarantine：设计、开销与根本问题

> ⚠️ 这是 RSan 性能开销中**除 sanitizer check 之外最大的单一来源**，也是 MixSan 试图通过 MemTag 机制绕过的核心瓶颈。

#### 2.7.1 为什么需要 Quarantine

RSan 的时序检测机制极其简单：**释放对象时将 bound 元数据置 0**。此后任何对该对象的访问都会因 `addr > 0` 而触发错误。

但问题在于：如果不延迟内存复用，被释放的内存可能立刻被重新分配。新的对象会有新的 non-zero bound，此时旧指针（dangling pointer）的访问会错误地通过检查——**漏报（false negative）**。

因此 RSan 必须引入 **Heap Quarantine**：释放后的内存不立即归还给分配器，而是在一个队列中"隔离"一段时间，确保 dangling pointer 在此期间被使用时触发检测。

#### 2.7.2 实现：线程安全的 Ring Buffer

论文明确描述 RSan 的 quarantine 为 **"a simple thread-safe ring buffer"**（§7.2）。这意味着：

- **进程级全局锁**：每次 `free()` 都要争抢 quarantine 队列的线程安全锁（`ENABLE_QUARANTINE` 宏控制）
- **内存持有**：quarantine 中的内存页不被释放，导致 RSS 膨胀
- **固定容量**：队列满后，最早进入的对象被真正释放并归还给 TCMalloc

#### 2.7.3 运行时开销定量分析

| 开销维度 | 数值 | 数据来源 |
|---------|------|---------|
| Geomean runtime overhead | **11pp**（总 51pp 中的第二大项） | SPEC CPU2006, §7.2 |
| 分配密集型 benchmark 最坏情况 | **126pp** on `403.gcc` | §7.2, Figure 7 |
| 低分配 benchmark | ~0pp (e.g., `400.perlbench`, `401.bzip2`) | Figure 7 |
| Arm TBI 平台 | **5.1pp**（x86 的 46%） | §7.4 |

论文明确指出：

> *"We confirmed that the performance penalty from the quarantine is caused by **memory fragmentation**, and not by any bottleneck in our quarantine data structure implementation (a simple thread-safe ring buffer)."*

这句话有两个关键含义：
1. **瓶颈不是锁争抢本身**（ring buffer 足够简单），而是 quarantine 导致的**内存碎片化**
2. 碎片化破坏了 TCMalloc 的分配效率——频繁的 quarantine 入队/出队使内存布局碎片化，power-of-two size class 的对齐优势被稀释

#### 2.7.4 内存开销

| 配置 | 内存开销 |
|------|---------|
| Quarantine 启用（隐式 tagging） | **239%** |
| Quarantine 禁用（隐式 tagging） | **78%** |
| Quarantine 禁用（显式 tagging, Arm/Intel） | **42%** |

Quarantine 贡献了约 **160pp 的内存开销**——是 RSan 内存膨胀的最大来源。

#### 2.7.5 Quarantine 对时序检测的根本限制

论文 §7.1 坦承：

> *"For temporal errors, RSan's detection guarantees are identical to ASan and **fundamentally limited by the size of the heap quarantine** to delay reuse of the memory."*

核心矛盾：
- **Quarantine 越大** → 检测窗口越长 → 漏报率越低 → **但内存开销和碎片化越严重**
- **Quarantine 越小** → 性能越好 → **但 dangling pointer 在对象被重新分配后变成漏报**

这是所有依赖 quarantine 的 sanitizer（ASan、FloatZone、RSan）的**本质性 tradeoff**——无法通过工程优化彻底解决。

#### 2.7.6 对 MixSan 的启示

MixSan 引入 **MemTag 版本号机制** 正是为了**打破这个 tradeoff**：

| | RSan | MixSan |
|------|------|--------|
| 时序检测机制 | bound ← 0，依赖 quarantine 延迟复用 | MemTag ← MemTag+1，版本号永久不匹配 |
| UAF 在对象重新分配后能否检测 | ❌（若复用则 bound 恢复 → 漏报） | ✅（MemTag 增量确保任何复用都不匹配旧指针） |
| 是否依赖 quarantine | 是（不 quarantine 则立即复用 → 大量漏报） | **否**（可禁用 quarantine，降低碎片化开销） |
| Double-free 检测 | ❌（无 DF 专用检测） | ✅（free 时检测 MemTag 是否已递增） |

**MixSan 理论上可以在 `ENABLE_QUARANTINE=OFF` 下运行，省略 RSan 开销中这 11pp（geomean）/ 126pp（worst-case）的代价，同时保持（甚至增强）时序检测能力。**

---

## 3. 编译器优化

RSan 的 range check 能力解锁了大量传统 redzone 无法使用的优化：

### 3.1 已有优化（继承自 ASan--）

| 优化 | 说明 |
|------|------|
| Unsatisfiable checks | 静态可证明安全的访问，直接消除检查 |
| Recurring checks | 去重：如果已有的检查必然先于另一个，消除后者 |
| Neighboring checks | 三个同对象访问，中间的检查可跳过 |

### 3.2 循环优化（Loop Optimizations）

**无条件执行 + 循环不变量指针**（Listing 4）：
- Hoist 检查到循环外（单个检查替代 N 次检查）

**无条件执行 + 循环变量指针**（Listing 5）：
- 利用 LLVM SCEV 分析计算指针的起始和终止地址
- 在循环外插入一个 **range check** 覆盖整个循环的范围
- **这是 RSan 循环优化的最大收益来源**：贡献 16.5pp 的开销降低

**条件执行 + 循环变量指针**（Listing 6）：
- 缓存 bound 元数据到局部变量（首次访问时计算，后续复用）
- `if (ptr+size > cached_bound) { cached_bound = full_check(ptr); }`

**保守性保证**：使用别名分析确保循环内没有对目标指针的潜在释放调用，否则不优化。

### 3.3 Range Check 合并（Check Merging）

**常量偏移合并**（Listing 7）：
- 多个访问同一 GEP base、不同常量偏移的内存操作 → 合并为覆盖 [min_offset, max_offset] 的单一 range check

**负-正偏移对合并**（Listing 8）：
- 一个 GEP offset 可证明为负、另一个可证明为正 → 合并为 range check

**Lower-Higher 对合并**（Listing 9）：
- 两个变量偏移、同一 GEP base 的访问 → 运行时比较 ptr+x 和 ptr+y，将检查合并为一个 range check
- 用一次 `min/max` 比较和一次 range check 替代两次完整检查

### 3.4 优化效果（SPEC CPU2006）

| 阶段 | 累计 geomean 开销 |
|------|-------------------|
| 未优化 RSan | 90.1% |
| + 已有优化 (ASan--) | 78.8% |
| + 循环优化（全部） | 58.8% |
| + Check 合并（全部） | **51.0%** |

---

## 4. 实现要点

- **LLVM 16.0.6 LTO** + 修改的 **TCMalloc 2.15**
- **栈**：使用 SafeStack + size class 分离（借鉴 FloatZone/StickyTags）
- **全局变量**：通过 linker script 迁移到对应 size class 区域
- **隐式打标签**：自定义 dynamic linker 在程序启动时重组地址空间
- **Guard page**：在 size class 前插入 `MAP_NORESERVE` 映射避免段错误

---

## 5. 性能评估

### 5.1 SPEC CPU 开销对比

| Sanitizer | SPEC CPU2006 | SPEC CPU2017 |
|-----------|-------------|-------------|
| **RSan** | **51.0%** | **44.5%** |
| FloatZone | ~52% | ~62.6% |
| GiantSan | ~60% | ~55% |
| ASan-- | ~82% | ~80% |
| ASan | ~95% | ~87% |

- RSan 在所有对比中最快
- FloatZone 在 SPEC 2017 的混合核心（P-core + E-core）上开销大幅波动（E-core 上 600.perlbench 有 27x 开销），RSan 在不同核心上表现稳定

### 5.2 显式打标签（Arm TBI / Intel LAM）

| Sanitizer | Arm TBI 开销 | Intel LAM 开销 |
|-----------|-------------|---------------|
| **RSan** | **54.0%** | **54.0%** |
| ASan-- | 75.4% | 143.1% |
| ASan | 86.7% | 159.1% |
| HWASan | 131.6% | 268.3% |
| FloatZone | N/A | 60.0% |

- RSan 显式打标签时内存开销降至 42%（无 quarantine），接近 ASan 水平
- HWASan（硬件辅助 ASan）在最新 Intel CPU 上性能反而极差

### 5.3 开销构成（SPEC CPU2006 分解）

**Geomean 分解：**

| 组件 | 开销贡献 | 说明 |
|------|---------|------|
| Power-of-two heap allocator | 1pp | 含隐式 pointer tagging |
| Redzone padding + metadata 写入 | 3pp | 分配时扩展对象到 2 的幂 + 写入 bound |
| Stack size class 分离 | 1pp | SafeStack 多栈分离 |
| Globals 迁移 | ~0pp | linker script 重排，无运行时代价 |
| **Sanitizer checks** | **35pp** | 最大开销，编译器优化后从原始 ~55pp 降至 35pp |
| **Heap quarantine** | **11pp** | ⚠️ 第二大开销源，根因是内存碎片化 |

**Quarantine 开销的 per-benchmark 分化**（论文 Figure 7 数据）：

| Benchmark | Quarantine 开销 | 特征 |
|-----------|----------------|------|
| `403.gcc` | **126pp** | 分配密集型，碎片化最严重 |
| `483.xalancbmk` | **~50pp** | 大量 C++ 对象分配/释放 |
| `400.perlbench` | ~0pp | 低分配率，quarantine 几乎无影响 |
| `401.bzip2` | ~0pp | 流式处理，分配少 |
| `445.gobmk` | ~5pp | 中等分配率 |
| `456.hmmer` | ~3pp | 计算密集，分配适中 |
| **Geomean** | **11pp** | 高方差，平均值掩盖了分配密集型程序的严重退化 |

**跨平台 Quarantine 开销差异：**

| 平台 | Quarantine 运行时开销 | Quarantine 内存开销（总开销-无quarantine） |
|------|---------------------|----------------------------------------|
| x86 implicit tagging | **11pp** | 239% - 78% = **161pp** |
| Arm TBI | **5.1pp** | 228% - 42% = **186pp** |
| Intel LAM | ~11pp | 207% - 42% = **165pp** |

> 论文观察到 Arm 平台 quarantine 运行时开销仅为 x86 的 46%（5.1pp vs 11pp），可能源于 Arm 的内存子系统对碎片化模式的不同敏感度。

**根因分析（论文原话）：**

> *"We confirmed that the performance penalty from the quarantine is caused by **memory fragmentation**, and not by any bottleneck in our quarantine data structure implementation."*

这意味着即使 quarantine 数据结构本身再优化（lock-free ring buffer、无锁队列等），只要 quarantine 机制存在，**内存碎片化对 TCMalloc 的破坏就是不可避免的**。碎片化导致：
1. TCMalloc 的 freelist 命中率下降
2. Power-of-two size class 的对齐优势被碎片稀释
3. 页表压力增大（quarantine 持有的页面不释放但也不合并）

### 5.4 Fuzzing 吞吐量

- RSan vs ASan--：geomean 吞吐量提升 **33.7%**（最高 70.1%）
- RSan vs FloatZone：geomean 吞吐量提升 **37.5%**（最高 67.6%）
- libxml2 上：1 小时后 coverage 峰值提升 **5.8%**

---

## 6. 安全检测能力

### 6.1 vs ASan：RSan 多检测的 Bug 类型

| 场景 | ASan | RSan |
|------|------|------|
| 跳过 redzone 的非线性溢出 | ✗（访问落到合法内存则漏检） | ✓（range check 覆盖无限 redzone） |
| 非对齐的部分溢出 | ✗（需要 8 字节对齐） | ✓（range check 不依赖对齐） |
| Wide string 操作 | ✗（未插桩 wcscpy 等） | ✓（插桩覆盖） |

### 6.2 Juliet Test Suite 检测率

| CWE 类别 | Total | ASan | RSan |
|----------|-------|------|------|
| Stack buffer overflow (121) | 2,885 | 2,791 | **2,885** |
| Heap buffer overflow (122) | 3,365 | 3,318 | **3,365** |
| Buffer underwrite (124) | 1,001 | 907 | **1,001** |
| Buffer overread (126) | 657 | 563 | **657** |
| Buffer underread (127) | 1,001 | 907 | **1,001** |
| Double free (415) | 799 | 799 | 799 |
| Use-after-free (416) | 374 | 374 | 374 |

- RSan 在对所有类别达到 **100% 检测率**
- ASan 漏检的主要原因：跳过 redzone 的溢出，以及未初始化的元数据读取返回 0 导致 RSan 额外检测

### 6.3 CVE 检测

16 个真实 CVE：RSan 和 ASan 均能全部检测。

---

## 7. 局限性

### 继承自 Redzone 方案的通用局限
- 不检测 **intra-object overflow**（结构体字段间溢出）
- 自定义内存分配器不兼容
- 非线性溢出若恰好落在有效内存区域仍可能漏检（但 RSan 的 range check 优化减小了这种可能）

### RSan 特有的技术局限

| 局限 | 说明 | 严重性 |
|------|------|--------|
| **Quarantine 驱动的内存碎片化** | 每次 `free()` 进入线程安全 ring buffer 队列，延迟归还内存，导致 TCMalloc 分配效率下降。分配密集型程序（如 `403.gcc`）quarantine 单独贡献 126pp | **★★★ 性能瓶颈** |
| **时序检测依赖 quarantine 大小** | UAF 检测窗口完全由 quarantine 容量决定；对象复用后 bound 恢复非零 → 漏报。这是"概率性 temporal safety"，非确定性 | **★★★ 检测能力上限** |
| **无 Double-Free 专用检测** | free 时仅检查 bound 是否为 0（简陋的 DF 检测），无法区分"首次 free"和"double free" | **★★ 漏检风险** |
| 动态库不支持 | SafeStack 不支持 shared library | 工程问题，非根本性 |
| base-8 段错误 | untagged 内存若 base 在页面前 7 字节，`*(base-8)` 会段错误 | 通过 `MAP_NORESERVE` 缓解 |
| 与应用指针打标签冲突 | 程序自身使用指针高位时会冲突 | 罕见 |
| 隐式打标签的地址空间限制 | 最大对象 2TB，需重组地址空间 | 仅 legacy 平台 |
| Intel LAM tag 空间未充分利用 | 15 位标签可支持更多优化 | 未来工作 |

---

## 8. 与 MixSan 的关系

### 8.1 架构同源性

MixSan **以 RSan 为基础**，两者的核心机制高度重叠：

| 机制 | RSan | MixSan |
|------|------|--------|
| 元数据位置 | base-8（underflow redzone） | base-8（同） |
| Size class 打标签 | 隐式（bit 41-47）或显式（TBI/LAM） | 隐式（bit 41-47，ISR） |
| 基础检查 | 单次范围检查（spatial） | 三阶段：SizeTag→Temporal→Spatial |
| 时序检测 | bound 置 0 | MemTag 递增 + 匹配 |
| 内存分配器 | TCMalloc（power-of-two） | TCMalloc（power-of-two） |
| 栈/全局 | SafeStack 分离 + linker script | SafeStack 分离 |
| 编译器基础设施 | LLVM 16 LTO + SafeStack pass | LLVM 16 LTO + SafeStack pass |

### 8.2 MixSan 在 RSan 基础上的增强

MixSan 的关键改进（按重要性排序）：

1. **从"Quarantine 依赖"到"MemTag 确定性检测"（★ 核心贡献）**：RSan 释放时简单置 0，必须依赖 quarantine 延迟复用才能保持检测窗口。quarantine 带来 11pp（geomean）/ 126pp（403.gcc 最坏情况）的运行时开销和 161pp 的内存开销，且一旦对象被重新分配，旧指针的访问变为漏报。MixSan 用 16-bit 递增 MemTag 版本号替代——每个分配代际获得唯一 temporal identity，即使对象被重新分配 N 次，旧指针的 MemTag 仍不匹配新对象的元数据，实现**确定性 temporal detection**。理论上可完全禁用 quarantine，省去其全部开销。

2. **从单阶段到三阶段检测**：增加了 `SizeTag gate`（快速跳过 untagged/uninstrumented 指针）和独立的 `Temporal check`（MemTag 匹配），将 RSan 的单次 bound 比较拆分为渐进式的三层过滤。

3. **指针 MemTag 传播**：通过 `tagPointerArg`/`tagPointerFromReturn` 在函数边界传播 MemTag，使跨函数、跨 TU 的指针仍携带 temporal identity。

4. **Double-Free 专用检测**：`do_free_with_callback` 中检查 MemTag 是否匹配——释放不匹配的指针直接触发报警。

5. **外部调用 MemTag 剥离**：`InstrumentCalls` 确保兼容性，可选 `TmpCheckBeforeCall` 增强检测。

### 8.3 RSan 的优化如何惠及 MixSan

RSan 的编译器优化对 MixSan **全部适用**（因为 MixSan 保留了 RSan 的 Spatial Check 作为 Stage 3）：
- 循环 hoisting / range check 可以减少 MixSan 的 `BoundsBB` 插入频率
- Check 合并可以减少 `InsertCheck` / `InsertCheckRange` 的调用次数
- 但需要注意：循环优化可能与 MixSan 的 temporal check 冲突（见 RSan §5.2 关于 UAF detection 的讨论）

### 8.4 Quarantine 问题的 MixSan 解决方案

这是 MixSan 相对于 RSan 最根本的架构优势：

```
RSan 路径:  free() → bound=0 → quarantine 队列 → 延迟复用 → 等待过期
            ↓
          性能代价: 11pp runtime + 161pp memory（碎片化）
          检测上限: 概率性（quarantine 满后复用 → 漏报）

MixSan 路径: free() → MemTag++ → 元数据版本号永久递增
             ↓
           性能: 无需 quarantine（仅一个原子递增操作）
           检测: 确定性（任何复用都无法匹配旧 MemTag）
```

**关键结论**：MixSan 可以在 `ENABLE_QUARANTINE=OFF` 下运行，省略 RSan 开销中 quarantine 贡献的 **11pp（geomean）/ 126pp（最坏情况）**，同时获得比 RSan 更强的确定性时序检测。

---

## 9. 关键数字速览

| 指标 | 数值 |
|------|------|
| SPEC CPU2017 geomean 开销 | **44%** |
| SPEC CPU2006 geomean 开销 | **51%** |
| Quarantine 运行时开销 (geomean) | **11pp**（占总额 22%） |
| Quarantine 运行时开销 (最坏: 403.gcc) | **126pp** |
| Quarantine 内存开销 | **161pp**（239%-78%），禁用后降至 42%（显式 tagging） |
| Quarantine 实现 | 线程安全 ring buffer；瓶颈在**内存碎片化**，非锁争抢 |
| vs ASan（SPEC 2017） | 约 **2x 更快** |
| vs FloatZone | 约 6pp 更快（Intel LAM） |
| Arm TBI 开销 | 54%；quarantine 仅 5.1pp |
| Fuzzing 吞吐量提升 vs ASan-- | geomean **33.7%** |
| Juliet 检测率 | **100%**（所有类别） |
| 元数据查找 | **O(1)**：base-8 直接读取 |
| 检查复杂度 | **O(1)**：一次比较 |
| 隐式 tag 位宽 | 6 bits（bits 41-46），编码 64 个 size class |
| 时序检测确定性 | **概率性**（受 quarantine 容量限制，对象复用后漏报） |
| 最大对象大小（隐式） | 2TB |
| 最小 size class | 32 bytes（2^5） |

---

## 10. 未穷尽的优化空间（论文提出的未来工作）

1. **Intel LAM 的 15 位标签充分利用**：可直接编码 size class 而无需 log2 编码，甚至可能解除 2 的幂限制
2. **大 size class 的 redzone 空间复用**：将小对象放置在大对象的 redzone 中，降低内存开销
3. **跨过程 check 合并**（interprocedural check merging）
4. **Check 合并与 metadata caching 的交叉优化**
5. **跨非循环访问的 metadata 复用**
6. **更宽松的别名分析**以找到更多指向同一对象的指针

---
