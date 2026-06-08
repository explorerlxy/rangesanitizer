# MixSan MemTag 测试套件

> 618 个 C/C++ 测试用例，系统性评估 MixSan 6-bit MemTag 机制相比 RSan quarantine 的性能与能力提升。
> 融入 `env.sh` + `setup.py` 测试系统，与 `spec2006`/`spec2017`/`juliet` 并列。

---

## 1. 架构集成

测试套件已完全融入项目测试基础设施：

| 集成点 | 方式 |
|--------|------|
| **env.sh** | Makefile 引用 `$RSAN_ORIG_C`、`$RSAN_MIX_C`、`$RSAN_*_TC_IMPL_BUILD` 等全部环境变量 |
| **setup.py** | `MemTagTest` 类注册为 `memtag_test` target，与 `spec2006`/`juliet` 并列 |
| **构建** | `python3 setup.py build memtag_test mixsan_O0 --parallel=proc` |
| **运行** | `python3 setup.py run memtag_test baseline_O0 rsan-orig_O0 mixsan_O0 --build` |
| **报告** | `python3 setup.py report memtag_test results/last --field detected missed` |
| **独立使用** | `make all && ./run_tests.sh`（不依赖 setup.py） |

### 快速开始

```bash
source env.sh
cd tests/memtag

# 通过 setup.py（推荐，可并行构建）
python3 setup.py run memtag_test baseline_O0 mixsan_O0 --build --parallel=proc
python3 setup.py report memtag_test results/last --field detected missed verdict

# 或独立构建运行
make all && ./run_tests.sh
```

---

## 2. 测试集总览

```
tests/memtag/
├── Makefile                          # 三变体构建 (baseline/rsan/mixsan)
├── run_tests.sh                      # 批量运行 + 对比报告
├── generate_tests.py                 # 参数化测试生成器
├── templates/                        # 6 个 C 代码模板
│   ├── uaf_exhaust.c.tmpl
│   ├── oob_skip.c.tmpl
│   ├── multi_cycle.c.tmpl
│   ├── df_reuse.c.tmpl
│   ├── realloc_chain.c.tmpl
│   └── thread_race.c.tmpl
│
├── cat1_uaf_reuse/            ( 2 C)   UAF 跨复用检测
├── cat2_double_free/          ( 1 C)   DF after slot reuse
├── cat3_nonlinear_oob/        ( 3 C)   非线性越界 + MemTag 第二防线
├── cat4_realloc/              ( 2 C)   Realloc 场景
├── cat5_edge_cases/           ( 3 C)   边界条件 (wrap/zero-skip/mmap)
├── cat6_perf_micro/           ( 2 C)   性能微基准 (ST/MT)
├── cat7_spatial_variants/     ( 6 C)   空间错误变体
├── cat8_cpp_temporal/         ( 5 C++) C++ 时序错误
├── cat9_alloc_patterns/       ( 4 C)   特殊分配模式
├── cat10_data_structures/     ( 4 C)   数据结构错误
├── cat11_memtag_stats/        ( 3 C)   MemTag 统计验证
├── cat12_thread/              ( 3 C)   多线程场景
│
├── gen_uaf_exhaust/           (45 C)   参数化 UAF 耗尽 quarantine
├── gen_oob_skip/              (80 C)   参数化 OOB 跨 redzone
├── gen_multi_cycle/           (60 C)   参数化多轮复用 UAF
├── gen_df_reuse/              (50 C)   参数化 DF after reuse
├── gen_realloc_chain/         (50 C)   参数化 realloc 链
├── gen_thread_race/           (48 C)   参数化多线程竞态
├── gen_cpp_patterns/          (60 C++) 参数化 C++ 继承层次
├── gen_size_mix/              (50 C)   参数化跨 size class 混合
├── gen_ds_variants/           (37 C)   参数化数据结构规模
├── gen_alloc_funcs/           (50 C)   参数化分配函数变体
└── gen_statistical/           (50 C)   参数化统计检测率梯度
```

**总计**: 38 手工精编 + 580 参数化生成 = **618 个测试用例**（23 个目录）

---

## 3. 测试机制背景

### RSan Quarantine（原始）：有明确边界的确定性检测

- 实现：进程级 256MB ring buffer，`pthread_mutex_lock` 保护
- 所有 size class 共享同一个 quarantine
- 窗口内（quarantine 未满）：`bound=0` → 必然检测
- 窗口外（quarantine 满，对象被 `real_free`）：bound 恢复 → 必然漏报

### MixSan MemTag（增强）：贯穿进程生命周期的高概率检测

- 6-bit MemTag：Intel LAM U57 bits [62:57]，取值范围 [1, 63]，0 保留
- 每次 `free()`：`new = (old + 7) & 0x3F`；若 `new == 0` 则 `new = 7`
- `gcd(7, 64) = 1`，skip-0 规则绕过 0 号空洞 → **63 次 free 后回绕**
- 检测概率：**62/63 ≈ 98.4%**，贯穿进程全生命周期
- 三阶段检查：SizeTag gate → Temporal (MemTag match) → Spatial (bound check)

### MemTag 对非线性越界的增强（核心差异）

RSan 的 spatial check 仅依赖 redzone：
- 越界命中 redzone → spatial check 触发 → 检测
- 越界跳过 redzone，落入其他对象合法区间 → spatial check 通过 → **漏报**

MixSan 加入 MemTag 后：
- Stage 2 检查 `PTR_GET_MEMTAG(ptr) == PTR_GET_MEMTAG(meta)`
- 即使越界落入其他对象，只要 MemTag 不同 → 检测
- 检测概率 ≈ `(total_allocated - same_memtag_blocks) / total_allocated` → 接近 100%

---

## 4. 手工精编用例详述（12 类，38 个）

### cat1_uaf_reuse — UAF 跨复用检测（★ 核心差异）

| 测试 | 目的 | RSan | MixSan |
|------|------|------|--------|
| `uaf_reuse_exhaust.c` | 大分配（32MB×10=320MB）耗尽 256MB quarantine，victim 必然被驱逐复用 → 旧指针访问 | MISS | **DETECT (62/63)** |
| `uaf_reuse_same_size.c` | free 后立即分配同大小对象，旧指针访问 | 窗口依赖 | **DETECT (62/63)** |

### cat2_double_free — Double-Free after Slot Reuse

| 测试 | 目的 | RSan | MixSan |
|------|------|------|--------|
| `df_interleaved.c` | free→中间 alloc 复用 slot→再次 free。RSan `*meta_ptr==0` 检查在 slot 复用后失效 | MISS (复用后) | **DETECT (62/63)** |

> 连续两次 `free(p)` 无区分度（两者都在第二次 SIGTRAP），真正区分场景是中间有复用。

### cat3_nonlinear_oob — MemTag 第二道防线（★ 核心差异）

| 测试 | 目的 | RSan | MixSan |
|------|------|------|--------|
| `oob_skip_redzone.c` | OOB 越过 redzone 命中相邻对象合法区间 | MISS (spatial 通过) | **DETECT (62/63)** |
| `oob_far_jump.c` | 大跨度越界命中不同 size class 对象 | MISS (spatial 通过) | **DETECT (62/63)** |
| `oob_memtag_second_defense.c` | 多对象场景验证 MemTag 为 spatial check 提供第二道防线 | MISS | **DETECT (62/63)** |

### cat4_realloc — Realloc 场景

| 测试 | 目的 | RSan | MixSan |
|------|------|------|--------|
| `realloc_shrink.c` | realloc 缩小后 MemTag 保持 + bound 更新 | DETECT (spatial) | DETECT (spatial) |
| `realloc_grow_move.c` | realloc 扩大搬迁后原指针 UAF | 窗口依赖 | **DETECT (62/63)** |

### cat5_edge_cases — 边界条件

| 测试 | 目的 | RSan | MixSan |
|------|------|------|--------|
| `memtag_wrap.c` | 同 slot 65 次 free 验证回绕行为 | N/A | DETECT (62/63) |
| `memtag_zero_skip.c` | `ptr_memtag==0` → 跳过 Stage 2，不误报 | PASS | **PASS** |
| `large_mmap_alloc.c` | 大对象（mmap 路径）的 MemTag 行为 | PASS | PASS |

### cat6_perf_micro — 性能微基准

| 测试 | 测量目标 |
|------|---------|
| `single_thread_stress.c` | 500K alloc/free — quarantine 锁+入队 vs MemTag 原子更新 |
| `multi_thread_stress.c` | 8 线程×100K — 进程级 `pthread_mutex_lock` 串行化 vs 无锁 MemTag |

### cat7_spatial_variants — 空间错误变体

| 测试 | 漏洞模式 | RSan | MixSan |
|------|---------|------|--------|
| `off_by_one.c` | 单字节越界 | 布局依赖 | **DETECT (62/63)** |
| `negative_index_oob.c` | 负索引 underflow | 布局依赖 | **DETECT (62/63)** |
| `struct_field_overflow.c` | 结构体字段间溢出 | MISS | MISS (已知局限) |
| `strcpy_overflow.c` | strcpy 越界写入 | 溢出量依赖 | **DETECT (62/63)** |
| `large_stride_oob.c` | wild pointer 大跨度越界 | MISS | **DETECT (62/63)** |
| `loop_oob.c` | 循环索引未检查上界 | 优化依赖 | **DETECT (62/63)** |

### cat8_cpp_temporal — C++ 时序错误

| 测试 | 漏洞模式 | RSan | MixSan |
|------|---------|------|--------|
| `new_delete_uaf.cpp` | new/delete 后通过悬垂指针访问成员 | 窗口依赖 | **DETECT (62/63)** |
| `virtual_call_after_free.cpp` | 对象释放后调用虚函数（vtable 悬垂） | 窗口依赖 | **DETECT (62/63)** |
| `vector_iterator_uaf.cpp` | vector realloc 后旧迭代器悬垂 | 窗口依赖 | **DETECT (62/63)** |
| `array_new_delete_mismatch.cpp` | `new[]` 用 `delete` 释放后 UAF | 窗口依赖 | **DETECT (62/63)** |
| `member_function_uaf.cpp` | 非虚成员函数访问 this->data（回调悬垂） | 窗口依赖 | **DETECT (62/63)** |

### cat9_alloc_patterns — 特殊分配模式

| 测试 | 验证点 |
|------|--------|
| `calloc_uaf.c` | calloc 路径 MemTag 正确性 |
| `memalign_reuse_uaf.c` | memalign 对齐分配路径 |
| `realloc_chain.c` | 多次 realloc 链 — 中间指针有唯一 MemTag |
| `zero_size_edge.c` | malloc(0) 边界行为 |

### cat10_data_structures — 数据结构中的内存错误

| 测试 | 模式 | RSan | MixSan |
|------|------|------|--------|
| `linked_list_uaf.c` | 节点释放后未摘除 → 遍历 UAF | 窗口依赖 | **DETECT (62/63)** |
| `tree_double_free.c` | 递归释放 + 再次 free 根节点 | 复用后 MISS | **DETECT (62/63)** |
| `ring_buffer_oob.c` | 索引未取模 → OOB 写入 | 空间依赖 | **DETECT (62/63)** |
| `hash_table_uaf.c` | 条目删除后仍被 lookup 返回 | 窗口依赖 | **DETECT (62/63)** |

### cat11_memtag_stats — MemTag 统计验证

| 测试 | 目的 |
|------|------|
| `uaf_detection_rate.c` | 单次运行验证 62/63 检测率（需多进程统计） |
| `multi_cycle_reuse.c` | 50 轮 alloc/free 后 UAF — quarantine 必然失效，MemTag 持续 |
| `memtag_independence.c` | 10 种 size class 验证 MemTag 正确性和独立性 |

### cat12_thread — 多线程场景

| 测试 | 目的 | 额外优势 |
|------|------|---------|
| `thread_uaf_race.c` | Thread-A 释放，Thread-B 仍在使用 | MemTag 无全局锁 |
| `thread_df_race.c` | 两个线程同时释放同一指针 | MemTag 无全局锁 |
| `parallel_alloc_free.c` | 4 线程并行 alloc/free 正确性验证 | 并发压力测试 |

---

## 5. 参数化生成用例（11 类，580 个）

通过 `generate_tests.py` 从 6 个 C 模板 + C++ 内联生成，覆盖多维参数空间。

### 生成器参数空间

| 生成类别 | 数量 | 参数维度 |
|----------|------|---------|
| `gen_uaf_exhaust` | 45 | chunk 大小 (8/32MB) × chunk 数量 (2-75) × victim 大小 (8B-128KB) |
| `gen_oob_skip` | 80 | 10 种 alloc 大小组合 × 8 种偏移距离 (从刚好出界到远跨) |
| `gen_multi_cycle` | 60 | 1-60 轮 alloc/free 周期 (quarantine 窗口内→远超窗口) |
| `gen_df_reuse` | 50 | 0-49 次中间分配 (RSan 检测率从高到低) |
| `gen_realloc_chain` | 50 | 链长度 (2-11) × 悬垂指针索引 (逐步增大搬迁压力) |
| `gen_thread_race` | 48 | 线程数 (1-8) × UAF/DF 模式 × 4 次重复 |
| `gen_cpp_patterns` | 60 | 继承深度 (1-9) × 字段数 (1-13) + 额外分配次数 (10-100) |
| `gen_size_mix` | 50 | 10 种大小组合 × 对象数量 (5-30) |
| `gen_ds_variants` | 37 | 链表长度 (2-256)、树深度 (2-16)、ring buffer (4-256)、哈希表 (4-256) |
| `gen_alloc_funcs` | 50 | malloc/calloc × 大小 (8B-8KB) × 额外分配次数 (5/10/20) |
| `gen_statistical` | 50 | 5-54 次中间分配 (细粒度检测率梯度) |

### 使用生成器

```bash
cd tests/memtag
python3 generate_tests.py    # 重新生成（已生成 618 个，按需执行）
```

---

## 6. 预期结果矩阵

| 场景 | Baseline | RSan (quarantine) | MixSan (MemTag) |
|------|----------|-------------------|-----------------|
| UAF quarantine 窗口外 | MISS | **MISS** | **DETECT (62/63)** |
| 非线性 OOB 跨 redzone | MISS | **MISS** | **DETECT (62/63)** |
| DF after slot reuse | MISS | **MISS** | **DETECT (62/63)** |
| C++ virtual call UAF | MISS | MISS (窗口外) | **DETECT (62/63)** |
| 多轮复用 UAF (>10 cycles) | MISS | **MISS** | **DETECT (62/63)** |
| 多线程 UAF race | MISS | 窗口依赖 | **DETECT (62/63)** |
| MemTag zero-skip | N/A | N/A | **PASS** (不误报) |
| Intra-object overflow | MISS | MISS | MISS (已知局限) |

### 解读

- **exit 0** → PASS（无 bug 或 bug 被漏检）
- **exit 133** → DETECT（SIGTRAP — sanitizer 捕获）
- 最有价值的用例：RSan **PASS (MISS)** 而 MixSan **DETECT**

---

## 7. MemTag 1/63 碰撞边界

MemTag 的 `+7 mod 64 (skip 0)` 产生 63 阶完整循环（`gcd(7,64)=1`）。碰撞仅在：

- **(UAF)**：同一 slot 被重新分配 63 次后，MemTag 回到初始值
- **(OOB)**：两个同时存活对象在 size class 区域内精确相隔 `63 × slot_size` 字节

此边界无法构造为确定性单次测试，作为理论性质文档化。

---

## 8. 与 SPEC / Juliet 的关系

| 测试套件 | 定位 | 用例数 | MixSan vs RSan 区分 |
|---------|------|--------|-------------------|
| **SPEC CPU 2006/2017** | 宏观性能基准 | 31 benchmarks | 量化整体 overhead |
| **Juliet Test Suite** | 标准 CWE 检测率 | ~10,000 | 覆盖标准漏洞类别 |
| **MemTag Test Suite** | MemTag 专属能力验证 | 618 | 针对性验证 MemTag 相比 quarantine 的独特优势 |

三者互补：SPEC 测量性能，Juliet 覆盖标准检测，MemTag 套件专攻 MixSan 相比 RSan 的能力边界。

---

## 9. 目录路径

```
tests/memtag/                           # 测试套件根目录
docs/MEMTAG_TEST_SUITE.md               # 本文档
docs/RSAN_BUILD_AND_TESTING_GUIDE.md    # 构建与测试总指南（含 MemTag 测试章节）
setup.py                                # MemTagTest target 定义
env.sh                                  # 环境变量（Makefile 引用）
```

---

*文档创建：2026-05-25*
*测试套件版本：v1.0 (618 tests)*
