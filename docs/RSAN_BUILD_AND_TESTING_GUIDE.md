# RSan Build & Testing Guide

## Architecture

Three independent instances, no shared build artifacts:

| Instance | Compiler | TCMalloc | Linker | Build Dir |
|----------|----------|----------|--------|-----------|
| `baseline_O2` | `RSAN_ORIG_C` (worktree) | `RSAN_ORIG_TC_BASE_BUILD` | N/A | `$RSAN_ORIG` |
| `rsan-orig_O2` | `RSAN_ORIG_C` (worktree) | `RSAN_ORIG_TC_IMPL_BUILD` | `$RSAN_ORIG/linker-implicit/` | `$RSAN_ORIG` |
| `mixsan_O2` | `RSAN_MIX_C` (main dir) | `RSAN_MIX_TC_IMPL_BUILD` | `$RSAN_TOP/linker-implicit/` | `$RSAN_TOP` |

```
RSAN_TOP  = /home/hahafish/rangesanitizer          # SmartMixSan-LAM dev
RSAN_ORIG = /home/hahafish/rangesanitizer-original  # original RSan (frozen)
```

All env vars are in `env.sh`. Every instance reads from a separate set of variables — changing one never affects the others.

---

## Quick Start

```bash
source env.sh
```

## Build

### Original RSan (baseline / rsan-orig)

Already pre-built in worktree. Rebuild only if source changes:

```bash
cd $RSAN_ORIG
source env.sh
./install-all.sh
```

### SmartMixSan (mixsan)

Build LLVM:

```bash
mkdir -p $RSAN_MIX_LLVM_BUILD && cd $RSAN_MIX_LLVM_BUILD
cmake -DLLVM_ENABLE_PROJECTS="clang;lld" \
      -DLLVM_ENABLE_RUNTIMES="compiler-rt" \
      -DCMAKE_BUILD_TYPE=Release -GNinja \
      -DLLVM_PARALLEL_LINK_JOBS=1 \
      -DLLVM_TARGETS_TO_BUILD=X86 \
      -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON \
      -DCLANG_ENABLE_STATIC_ANALYZER=OFF \
      -DCLANG_ENABLE_ARCMT=OFF \
      $RSAN_MIX_LLVM
ninja -j $(nproc)
```

Build TCMalloc:

```bash
cd $RSAN_MIX_TC_IMPL
autoreconf -i && ./autogen.sh || true
mkdir -p $RSAN_MIX_TC_IMPL_BUILD && cd $RSAN_MIX_TC_IMPL_BUILD
CFLAGS="-g -O2 -mbmi2" CXXFLAGS="-g -O2 -mbmi2" \
  ../tcmalloc-implicit/configure --prefix=$RSAN_MIX_TC_IMPL_BUILD
make -j $(nproc) && make install
```

Build linker components:

```bash
cd $RSAN_TOP/linker-implicit/globals && python3 generate_linker_script.py
cd $RSAN_TOP/linker-implicit/libdl && ./run.sh
```

### Incremental Rebuild

After modifying mixsan source, rebuild only the changed component:

```bash
source env.sh

# LLVM pass modified (SafeStack.cpp etc.)
cd $RSAN_MIX_LLVM_BUILD && ninja -j $(nproc)

# TCMalloc modified (tcmalloc.cc, common.h etc.)
cd $RSAN_MIX_TC_IMPL_BUILD && make -j $(nproc) && make install

# Linker script modified
cd $RSAN_TOP/linker-implicit/globals && python3 generate_linker_script.py

# Dynamic linker modified
cd $RSAN_TOP/linker-implicit/libdl && ./run.sh
```

`ninja` and `make` automatically detect changed files and rebuild only affected `.o` files.

---

## SPEC CPU 2006

Path: `$RSAN_SPEC2006` (`/media/hahafish/数据/ForUbuntu/speccpu2006`)

### Build

```bash
source env.sh

# Single instance
python3 setup.py build spec2006 baseline_O2 --parallel=proc --jobs=14
python3 setup.py build spec2006 rsan-orig_O2 --parallel=proc --jobs=14
python3 setup.py build spec2006 mixsan_O2 --parallel=proc --jobs=14

# Multiple instances
python3 setup.py build spec2006 baseline_O2 rsan-orig_O2 --jobs=14

# Specific benchmarks
python3 setup.py build spec2006 baseline_O2 --benchmarks all_c all_cpp --jobs=14
python3 setup.py build spec2006 baseline_O2 --benchmarks 400.perlbench 403.gcc
```

### Run

```bash
# Quick test (test workload, 1 iteration)
python3 setup.py run spec2006 baseline_O2 --test

# Full reference run (3 iterations)
python3 setup.py run spec2006 baseline_O2 rsan-orig_O2 --iterations 3

# Specific benchmarks
python3 setup.py run spec2006 baseline_O2 --benchmarks 400.perlbench 403.gcc -i 3
```

### Report

```bash
# Latest run summary
python3 setup.py report spec2006 results/last --field runtime:median maxrss:median

# With overhead vs baseline
python3 setup.py report spec2006 results/last --overhead baseline_O2 \
    --field runtime:median maxrss:median overhead:median

# CSV export
python3 setup.py report spec2006 results/last --csv
```

### Benchmark Sets

| Set | Count | Contents |
|-----|-------|----------|
| `all_c` | 12 | C benchmarks |
| `all_cpp` | 7 | C++ benchmarks |
| `int` | 12 | Integer |
| `fp` | 18 | Floating-point |
| `veripatch` | 16 | Subset verified with RSan patches |

---

## SPEC CPU 2017

Path: `$RSAN_SPEC2017` (`/home/hahafish/speccpu2017`)

Default benchmarks: `intspeed_pure_c`, `intspeed_pure_cpp`, `fpspeed_pure_c` (12 total)

### Build

```bash
source env.sh

python3 setup.py build spec2017 baseline_O2 --parallel=proc --jobs=14
python3 setup.py build spec2017 rsan-orig_O2 --parallel=proc --jobs=14
python3 setup.py build spec2017 mixsan_O2 --parallel=proc --jobs=14

```

### Run

```bash
python3 setup.py run spec2017 baseline_O2 --test
python3 setup.py run spec2017 baseline_O2 rsan-orig_O2 --iterations 3
```

### Report

```bash
python3 setup.py report spec2017 results/last --field runtime:median maxrss:median
```

### Benchmark Sets

| Set | Count | Contents |
|-----|-------|----------|
| `intspeed_pure_c` | 5 | 600.perlbench_s, 602.gcc_s, 605.mcf_s, 625.x264_s, 657.xz_s |
| `intspeed_pure_cpp` | 4 | 620.omnetpp_s, 623.xalancbmk_s, 631.deepsjeng_s, 641.leela_s |
| `fpspeed_pure_c` | 3 | 619.lbm_s, 638.imagick_s, 644.nab_s |
| `specspeed` | 20 | All speed benchmarks |

---

## Juliet Test Suite

```bash
source env.sh

# Build and run all three instances (O0 required — O2 hides bugs)
python3 setup.py run juliet baseline_O0 rsan-orig_O0 mixsan_O0 --build \
    --parallel=proc --parallelmax=$(nproc) \
    --cwe 121 122 124 126 127 415 416

# Single instance
python3 setup.py run juliet mixsan_O0 --build \
    --parallel=proc --parallelmax=$(nproc) \
    --cwe 415

# ASan comparison
python3 setup.py run juliet asan_O0 --build \
    --parallel=proc --parallelmax=$(nproc) \
    --cwe 121 122 415 416
```

Expected output:
```
[INFO] CWE415: Passed 799/799 GOOD tests
[INFO] CWE415: Passed 799/799 BAD tests
```

---

## MemTag Test Suite

Dedicated test suite for evaluating MixSan's 6-bit MemTag mechanism against original RSan. Covers capability gaps that Juliet and SPEC do not address: UAF-after-reuse, non-linear OOB with MemTag as second defense, double-free after slot reuse, and alloc/free performance under quarantine vs MemTag.

**Location**: `tests/memtag/`

### Test Categories

| Category | Focus | RSan→MixSan Differentiation |
|----------|-------|---------------------------|
| `cat1_uaf_reuse` | UAF after object reuse | RSan: quarantine window内DETECT, 窗口外(bound恢复)→MISS. MixSan: MemTag mismatch→DETECT (62/63) |
| `cat2_double_free` | Double-free after slot reuse | RSan: slot复用后bound≠0→`*meta_ptr==0`检查失效→MISS. MixSan: MemTag mismatch→DETECT |
| `cat3_nonlinear_oob` | OOB skipping redzone into adjacent object | RSan: spatial check通过→MISS. MixSan: Stage 2 MemTag充当第二道防线→DETECT |
| `cat4_realloc` | Realloc shrink/grow + UAF | MixSan正确保持/更新MemTag |
| `cat5_edge_cases` | MemTag wrap-around, zero-skip, mmap | 边界行为验证 |
| `cat6_perf_micro` | Single/multi-thread alloc/free stress | RSan quarantine锁 vs MixSan lock-free MemTag |

### Mechanism Background

**RSan Quarantine** (original): 256MB process-level ring buffer with `pthread_mutex_lock`. Detection guaranteed within quarantine window, inevitably fails once object leaves quarantine and slot is reused (bound restored to non-zero).

**MixSan MemTag** (6-bit, LAM U57 bits 62-57): Each `free()` increments MemTag by +7 (mod 64, skip 0). 63-step full cycle, collision probability 1/63 per realloc. No quarantine dependency — temporal safety persists throughout process lifetime.

### Quick Start

```bash
source env.sh
cd tests/memtag

# Build all three variants
make all

# Run and compare
./run_tests.sh

# JSON output for scripting
./run_tests.sh --json
```

Each `.c` file compiles into three executables:
- `*_baseline`: no sanitizer, baseline tcmalloc
- `*_rsan`: RSan-Orig (SafeStack + implicit SizeTag + quarantine)
- `*_mixsan`: MixSan (SafeStack + implicit SizeTag + MemTag, no quarantine)

### Core Differentiating Tests

The most valuable tests are those where **RSan returns 0 (MISS)** but **MixSan returns 133 (DETECT)**:

| Test | RSan | MixSan | Why |
|------|------|--------|-----|
| `uaf_reuse_exhaust` | MISS | DETECT | 大分配(>256MB)耗尽quarantine, victim被驱逐复用, bound恢复. MixSan MemTag mismatch. |
| `oob_skip_redzone` | MISS | DETECT | OOB跳过redzone命中相邻对象合法区间, spatial check通过. MixSan Stage 2 MemTag mismatch. |
| `df_interleaved` | MISS (窗口外) | DETECT | free→alloc复用slot→再次free. RSan bound≠0失效. MixSan MemTag mismatch. |

### Interpreting Results

- **exit 0** → PASS (no bug, or bug missed)
- **exit 133** → DETECT (SIGTRAP — sanitizer caught the bug)
- **exit other** → CRASH (unexpected)

### MemTag 1/63 Collision Boundary

The MemTag mechanism has a theoretical 1/63 detection gap. Collision occurs when:
- **(UAF)**: A slot is freed and reallocated exactly 63 times, completing a full MemTag cycle.
- **(OOB)**: Two live objects land at offsets satisfying `(offset_a / slot_size) % 63 == (offset_b / slot_size) % 63`, requiring them to be exactly 63×slot_size apart — vanishingly rare in practice.

This boundary cannot be constructed as a deterministic single-shot test; it is documented as a theoretical property.

### Performance Microbenchmarks

`cat6_perf_micro` tests are not run automatically. Execute manually with `/usr/bin/time -v`:

```bash
# Single-thread: quarantine vs MemTag alloc/free overhead
/usr/bin/time -v cat6_perf_micro/single_thread_stress_baseline
/usr/bin/time -v cat6_perf_micro/single_thread_stress_rsan
/usr/bin/time -v cat6_perf_micro/single_thread_stress_mixsan

# Multi-thread: process-level quarantine lock vs lock-free MemTag
/usr/bin/time -v cat6_perf_micro/multi_thread_stress_baseline
/usr/bin/time -v cat6_perf_micro/multi_thread_stress_rsan
/usr/bin/time -v cat6_perf_micro/multi_thread_stress_mixsan
```

Expected trends:
- **Runtime**: `baseline ≈ mixsan < rsan` (quarantine lock + fragmentation overhead)
- **RSS**: `baseline ≈ mixsan << rsan` (quarantine holds up to 256MB)
- **Multi-thread**: `baseline ≈ mixsan <<< rsan` (process-level mutex serializes all frees)

### Running via setup.py

```bash
source env.sh

# Build and run all three instances
python3 setup.py run memtag_test baseline_O0 rsan-orig_O0 mixsan_O0 --build

# Report
python3 setup.py report memtag_test results/last --field detected missed verdict
```

---

## Comparison Experiments

The testing system in `setup.py` is designed for systematic A/B comparison between baseline (no sanitizer), rsan-orig (original RSan), and mixsan (SmartMixSan-LAM). Each *instance* is an independent toolchain configuration — different compiler, tcmalloc build, linker script, and flags — managed as a Python class.

### Instance Architecture

Each instance encapsulates a complete build configuration:

| Component | `Baseline` | `RSanOrig` | `MixSan` |
|-----------|-----------|------------|----------|
| **Compiler** | `RSAN_ORIG_C` (worktree) | `RSAN_ORIG_C` (worktree) | `RSAN_MIX_C` (main dir) |
| **TCMalloc** | baseline (no tagging) | implicit tagging | implicit + MemTag |
| **SafeStack** | no | yes | yes (3-stage) |
| **Linker script** | none | worktree `linker-implicit/` | main dir `linker-implicit/` |
| **Purpose** | performance reference | correctness/safety baseline | development target |

The `Setup` class in `setup.py` registers five instance types at both O0 and O2:

```python
# Primary comparison targets
setup.add_instance(Baseline("O2"))      # → baseline_O2
setup.add_instance(RSanOrig("O2"))      # → rsan-orig_O2
setup.add_instance(MixSan("O2"))        # → mixsan_O2

# Additional reference points
setup.add_instance(RSanExplicit("O2"))  # → rsan-expl_O2
setup.add_instance(ASan("O2"))          # → asan_O2
```

The `configure(ctx)` method sets compiler, flags, and linker options atomically per instance, so running multiple instances never contaminates state.

### Full Comparison Workflow

A complete comparison experiment has three phases — build, run, report — each managed by a `setup.py` subcommand.

**Phase 1: Build all instances**

```bash
source env.sh

# Build all three primary instances for SPEC2006 (parallel per-benchmark, 14 jobs per compile)
python3 setup.py build spec2006 baseline_O2 rsan-orig_O2 mixsan_O2 \
    --parallel=proc --jobs=14

# Same for SPEC2017
python3 setup.py build spec2017 baseline_O2 rsan-orig_O2 mixsan_O2 \
    --parallel=proc --jobs=14

# Juliet uses O0 (optimizations hide memory bugs)
python3 setup.py build juliet baseline_O0 rsan-orig_O0 mixsan_O0 \
    --parallel=proc --parallelmax=$(nproc)
```

`--parallel=proc` runs each benchmark's build as an independent process. `--jobs=14` passes `-j14` to each individual compilation. This saturates CPU during the early phase; later phases naturally serialize as fewer benchmarks remain.

**Phase 2: Run all instances**

```bash
# SPEC2006 — 3 iterations for statistical stability
python3 setup.py run spec2006 baseline_O2 rsan-orig_O2 mixsan_O2 \
    --iterations 3

# SPEC2017 — 3 iterations
python3 setup.py run spec2017 baseline_O2 rsan-orig_O2 mixsan_O2 \
    --iterations 3

# Quick smoke test (test workload, 1 iteration) before full run
python3 setup.py run spec2006 baseline_O2 rsan-orig_O2 mixsan_O2 --test

# Juliet — build and run together, O0 required for bug detection
python3 setup.py run juliet baseline_O0 rsan-orig_O0 mixsan_O0 \
    --build --parallel=proc --parallelmax=$(nproc) \
    --cwe 121 122 124 126 127 415 416
```

Each benchmark's stdout/stderr lands in `results/run.<timestamp>/<target>/<instance>/<benchmark>`. A `results/last/` symlink always points to the most recent run.

**Phase 3: Report and compare**

```bash
# Basic summary — runtime and memory per benchmark per instance
python3 setup.py report spec2006 results/last \
    --field runtime:median maxrss:median

# Overhead vs baseline — the key comparison table
python3 setup.py report spec2006 results/last \
    --overhead baseline_O2 \
    --field runtime:median maxrss:median overhead:median

# SPEC2017 with geometric mean aggregate row
python3 setup.py report spec2017 results/last \
    --overhead baseline_O2 \
    --field runtime:median maxrss:median \
    --aggregate geomean

# CSV export for external analysis
python3 setup.py report spec2006 results/last \
    --overhead baseline_O2 \
    --field runtime:median maxrss:median \
    --csv > comparison.csv

# Inspect specific benchmark results across instances
python3 setup.py report spec2006 results/last \
    --filter 400.perlbench 403.gcc \
    --field runtime:median:stdev status

# Juliet per-CWE pass/fail comparison
python3 setup.py report juliet results/last \
    --field good_passed bad_passed good_total bad_total
```

### How `--baseline` / `--overhead` Works

The report command computes overhead as a ratio:

```
overhead = instance_aggregate / baseline_aggregate
```

For each `(benchmark, field)` pair:
1. All iterations for that instance are aggregated (median, mean, etc.)
2. The baseline instance aggregate is computed separately
3. Non-baseline instances are divided by the baseline value

A value of `1.0` means identical to baseline. `1.5` means 50% slower. This works for any numeric field — runtime, maxrss, cache misses, etc.

### Interpreting Results

**Performance comparison** (SPEC2006/2017):
- `baseline_O2` is the speed-of-light: fastest possible with vanilla tcmalloc, no instrumentation
- `rsan-orig_O2` shows the cost of SafeStack + implicit SizeTag checks
- `mixsan_O2` shows the additional cost of MemTag temporal safety on top of RSan
- The key metric is **mixsan overhead vs rsan-orig**: this isolates the MemTag cost

**Safety comparison** (Juliet CWE):
- All instances should pass all GOOD tests (true negatives — no false positives)
- On BAD tests (true positives — actual bugs):
  - `baseline` typically detects 0 (no instrumentation)
  - `rsan-orig` detects spatial bugs (CWE121, CWE122, CWE124, CWE126, CWE127)
  - `mixsan` additionally detects temporal bugs (CWE415, CWE416)
  - `asan` provides a reference for expected detection rates

**Example output:**
```
benchmark         baseline_O2  rsan-orig_O2  mixsan_O2
                   runtime      runtime:over  runtime:over
400.perlbench      321.5        366.3:1.14    378.9:1.18
403.gcc            245.1        282.3:1.15    291.7:1.19
...
geomean            1.00         1.12          1.17
```

This tells you: RSan adds ~12% overhead, MixSan adds an additional ~5% on top.

### Running Historical Comparisons

Since `results/last` always points to the latest run, you can compare any past run directory:

```bash
# List all past runs
ls results/

# Report a specific past run
python3 setup.py report spec2006 results/run.2025-05-20.14-30-00 \
    --overhead baseline_O2 \
    --field runtime:median

# Compare two runs (run report separately, diff manually)
python3 setup.py report spec2006 results/run.2025-05-20.14-30-00 \
    --field runtime:median > old.txt
python3 setup.py report spec2006 results/last \
    --field runtime:median > new.txt
diff old.txt new.txt
```

### One-Liner: Build, Run, Report

```bash
source env.sh

# SPEC2006 full cycle
python3 setup.py build spec2006 baseline_O2 rsan-orig_O2 mixsan_O2 --parallel=proc --jobs=14 && \
python3 setup.py run spec2006 baseline_O2 rsan-orig_O2 mixsan_O2 --iterations 3 && \
python3 setup.py report spec2006 results/last --overhead baseline_O2 \
    --field runtime:median maxrss:median --aggregate geomean

# SPEC2017 full cycle
python3 setup.py build spec2017 baseline_O2 rsan-orig_O2 mixsan_O2 --parallel=proc --jobs=14 && \
python3 setup.py run spec2017 baseline_O2 rsan-orig_O2 mixsan_O2 --iterations 3 && \
python3 setup.py report spec2017 results/last --overhead baseline_O2 \
    --field runtime:median maxrss:median --aggregate geomean

# Juliet full cycle
python3 setup.py run juliet baseline_O0 rsan-orig_O0 mixsan_O0 \
    --build --parallel=proc --parallelmax=$(nproc) \
    --cwe 121 122 124 126 127 415 416 && \
python3 setup.py report juliet results/last --field good_passed bad_passed

# MemTag test suite full cycle
python3 setup.py run memtag_test baseline_O0 rsan-orig_O0 mixsan_O0 --build && \
python3 setup.py report memtag_test results/last --field detected missed verdict
```

---

## MixSan Performance Optimization History

MixSan has been systematically optimized from an initial 6.00× overhead down to 1.45× (faster than RSan's 1.52×). All measurements on SPEC2006 429.mcf (baseline=133.7s, rsan-orig=203.7s).

### Optimization Journey

| Step | mcf Runtime | vs Baseline | Key Change |
|------|-----------|-------------|------------|
| mixsan (initial) | 802.6s | 6.00× | Original 3-stage check + quarantine-based MemTag |
| TSC+NOT tcmalloc | 232.3s | 1.74× | `rdtsc()` replaces `old_meta=*meta_ptr` in malloc; `*meta=~meta` in free; unified `abs(meta-ptr)>(1<<56)` check |
| + simplified SafeStack | 224.3s | 1.68× | Remove `PTR_GET_MEMTAG` extraction from inline checks |
| + unified check in pass | 222.1s | 1.66× | Single `Meta-(Ptr+n)>(THRESHOLD-n)` replaces temporal+spatial OR |
| + remove MetaMemTag | 197.5s | 1.48× | Drop redundant 3rd return value (identical to EndOfObj) |
| **final** | **194.1s** | **1.45×** | Re-optimized after revert; `tuple<Value*,Value*>` ABI critical |

### Key Lessons

1. **Memory access dominates** (570s): The `old_meta = *meta_ptr` read in malloc hot path contributed 85% of overhead. Replaced with `rdtsc()` (~20 cycles, pure register).
2. **Register ABI matters** (24s): Returning `tuple<Value*,Value*>` (RAX,RDX pair) vs single `Value*` (RAX only) changes caller register allocation — 2-value return is faster even with an unused Tag value.
3. **Unify checks** (2s): Mathematical unification of temporal+spatial into one subtraction eliminates `select`/`cmov` from branchless abs-diff.
4. **64-bit immediates hurt**: `PTR_GET_TAG` via `(x>>41)&0xFFFF` avoids the 10-byte `movabs` for `~MEMTAG_BITS`.

### Source Locations

| Component | File | Key Changes |
|-----------|------|-------------|
| MemTag macros | `tcmalloc-implicit/src/common.h` | `MEMTAG_FROM_TSC()`, `MEMTAG_CHECK()`, `PTR_GET_TAG(x>>41&0xFFFF)` |
| malloc/free | `tcmalloc-implicit/src/tcmalloc.cc` | 5 malloc paths use `MEMTAG_FROM_TSC()`, free uses `~meta` |
| swiftsan_check_n | `tcmalloc-implicit/src/tcmalloc.cc` | Unified `meta-(target+n) > (THRESHOLD-n)` |
| Inline checks | `llvm-project-16/llvm/lib/CodeGen/SafeStack.cpp` | InsertCheck/InsertCheckRange/InsertCheckMeta unified |

---

## Examples (Quick Validation)

```bash
source env.sh
cd examples
./test-implicit.sh   # x86 implicit tagging
```

Expected: valid access exits 0, out-of-bounds / use-after-free triggers SIGTRAP.

---

## All Instances

```bash
source env.sh

# Primary
python3 setup.py build spec2006 baseline_O2 rsan-orig_O2 mixsan_O2 --jobs=14

# Additional comparisons
python3 setup.py build spec2006 rsan-expl_O2 asan_O2 --jobs=14
```

| Instance | Class | Sanitizer | TCMalloc |
|----------|-------|-----------|----------|
| `baseline_O0/O2` | `Baseline` | none | baseline (worktree) |
| `rsan-orig_O0/O2` | `RSanOrig` | SafeStack + implicit | implicit (worktree) |
| `mixsan_O0/O2` | `MixSan` | SafeStack + implicit | implicit (main dir) |
| `rsan-expl_O0/O2` | `RSanExplicit` | SafeStack + explicit | explicit (worktree) |
| `asan_O0/O2` | `ASan` | ASan | system default |
| `memtag_test` | `MemTagTest` | target (not instance) | N/A (uses make) |

---

## Directory Layout

```
/home/hahafish/
  rangesanitizer/                # RSAN_TOP  (SmartMixSan-LAM)
    env.sh
    setup.py
    infra/                       # test harness
    llvm-project-16/             # LLVM source (tracked)
    tcmalloc-implicit/           # TC source (tracked)
    llvm-build/                  # [mixsan] LLVM build
    tcmalloc-impl-build/         # [mixsan] TC build
    build/                       # [mixsan] infra cache
    tests/memtag/                # MemTag test suite (MixSan vs RSan)
    docs/

  rangesanitizer-original/       # RSAN_ORIG  (frozen original)
    env.sh
    llvm-build/                  # [baseline/rsan-orig] LLVM build
    tcmalloc-baseline/           # gperftools 2.15 source
    tcmalloc-baseline-build/     # [baseline] TC build
    tcmalloc-impl-build/         # [rsan-orig] TC build
    build/                       # [baseline/rsan-orig] infra cache

  speccpu2017/                   # symlink -> /media/.../speccpu2017
```

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| SPEC2006 `447.dealII` compile error | Patches applied automatically by setup.py |
| SPEC2006 `483.xalancbmk` compile error | `#include <cstring>` added to `PlatformDefinitions.hpp` |
| SPEC2006 "not installed" | Run `install.sh -d <dir>` in SPEC directory |
| SPEC2017 DTD/XML error with non-ASCII path | Use ASCII symlink (`/home/hahafish/speccpu2017`) |
| ASan random crash | `sudo sysctl vm.mmap_rnd_bits=28` |
| Juliet slow | `sudo systemctl disable --now apport.service` |
| `mixsan` says compiler not found | Build LLVM for mixsan first (see Build section) |
