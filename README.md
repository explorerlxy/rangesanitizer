# MixSan

**MixSan** extends [RangeSanitizer (RSan)](https://download.vusec.net/papers/rsan_sec25.pdf) with a 6-bit identity tag on Intel LAM U57. Spatial bounds and object identity are stored in one fused metadata word and checked with a single subtraction.

This branch is `MixSan-LAM`. The GitHub default branch `master` is the original RSan fork. Clone with `-b MixSan-LAM`.

The identity tag is probabilistic (6 bits, collision \(1/64\) under a uniform-tag model). MixSan is a testing/fuzzing sanitizer, not a deterministic production defense.

## Unified check

```
(fused_meta - (tagged_ptr + access_size)) >> 56 != 0  →  error
```

Implemented in `llvm-project-16/llvm/lib/CodeGen/SafeStack.cpp` and `tcmalloc-implicit/src/common.h` (`MEMTAG_CHECK`).

Bit layout (Intel LAM U57):

```
Bit:  63    62–57     56–47      46–41     40–0
     [sign] [MemTag] [available] [SizeTag] [offset]
```

The allocator writes a tag from `rdtsc() & 0x3F` into bits 62–57 of the heap pointer and into the underflow metadata word.

## Repositories

| Artifact | Location |
|----------|----------|
| MixSan (this tree) | https://github.com/explorerlxy/rangesanitizer/tree/MixSan-LAM |
| MixSan (Naive), three-stage check | commit [`412147b`](https://github.com/explorerlxy/rangesanitizer/tree/412147bcad69e73ec66d14768d415de8a642a7bf) |
| Custom 97-test suite (\(N=10^{4}\) MixSan counts) | https://github.com/explorerlxy/97-costum-benchmark |
| SPEC / Larson / single-run suite logs | `results/` in this tree |

## Contents

| Path | Description |
|------|-------------|
| `examples/` | OOB, UAF, and identity-tag mismatch smoke tests |
| `infra/` | Benchmark driver used by `setup.py` |
| `linker-implicit/` | Linker script and custom dynamic linker |
| `llvm-project-16/` | LLVM 16.0.6 with MixSan checks in `SafeStack.cpp` |
| `tcmalloc-implicit/` | TCMalloc 2.15 with fused metadata (LAM / implicit tagging) |
| `tcmalloc-explicit/` | Explicit-tagging TCMalloc (Arm TBI), inherited from RSan |
| `results/` | Paper evaluation summaries and raw logs |
| `setup.py` | Instance × target harness |
| `env.sh` | Environment (`source`, do not execute) |

## Dependencies

Paper measurements: Intel Core Ultra 5 245K, Ubuntu 24.04, Linux 6.11, LAM U57 enabled in the kernel. MixSan temporal checks need LAM U57. Without it, tagged pointers typically segfault rather than trap.

```bash
sudo apt install ninja-build cmake gcc-9 autoconf2.69 bison build-essential flex texinfo libtool zlib1g-dev unzip gawk
pip3 install psutil terminaltables
```

## Quick start

```bash
git clone -b MixSan-LAM --recurse-submodules https://github.com/explorerlxy/rangesanitizer.git
cd rangesanitizer
source env.sh
```

`env.sh` sets `RSAN_TOP` from the script location. For SPEC, set `RSAN_SPEC2006` (or put it in an untracked `env.local.sh`). For `rsan-orig_*` / `baseline_*` in `setup.py`, point `RSAN_ORIG` at an original RSan checkout (`master` on this GitHub repo, or [vusec/rangesanitizer](https://github.com/vusec/rangesanitizer)).

### Build MixSan

```bash
mkdir -p $RSAN_MIX_LLVM_BUILD && cd $RSAN_MIX_LLVM_BUILD
cmake -DLLVM_ENABLE_PROJECTS="clang;lld" -DLLVM_ENABLE_RUNTIMES="compiler-rt" \
  -DCMAKE_BUILD_TYPE=Release -GNinja -DLLVM_PARALLEL_LINK_JOBS=1 \
  -DLLVM_TARGETS_TO_BUILD=X86 -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON \
  -DCLANG_ENABLE_STATIC_ANALYZER=OFF -DCLANG_ENABLE_ARCMT=OFF \
  $RSAN_MIX_LLVM
ninja -j $(nproc)

mkdir -p $RSAN_MIX_TC_IMPL_BUILD && cd $RSAN_MIX_TC_IMPL_BUILD
CFLAGS="-g -O2 -mbmi2" CXXFLAGS="-g -O2 -mbmi2" \
  $RSAN_MIX_TC_IMPL/configure --prefix=$RSAN_MIX_TC_IMPL_BUILD
make -j $(nproc) && make install

cd $RSAN_TOP/linker-implicit/globals && python3 generate_linker_script.py
cd $RSAN_TOP/linker-implicit/libdl && ./run.sh
```

If `configure` is missing under `tcmalloc-implicit/`, run `autoreconf -i` there first.

### Smoke test

```bash
cd $RSAN_TOP/examples
./test-implicit.sh
```

Valid access exits 0. Out-of-bounds, use-after-free, and identity-tag mismatch raise SIGTRAP (exit 133).

## Paper benchmarks

```bash
source env.sh

# SPEC CPU2006 (needs RSAN_SPEC2006 and, for RSan/ASan/baseline, RSAN_ORIG)
python3 setup.py build spec2006 baseline_O2 rsan-orig_O2 mixsan_O2 --parallel=proc --jobs=14
python3 setup.py run spec2006 baseline_O2 rsan-orig_O2 mixsan_O2 --iterations 3
python3 setup.py report spec2006 results/last --overhead baseline_O2 \
    --field runtime:median maxrss:median --aggregate geomean

# Juliet (use -O0)
python3 setup.py run juliet mixsan_O0 --build --parallel=proc --parallelmax=$(nproc) \
    --cwe 121 122 124 126 127 415 416
```

Custom 97-test suite (Table 4 / Table 5 in the manuscript): clone [97-costum-benchmark](https://github.com/explorerlxy/97-costum-benchmark), `source` this tree's `env.sh`, then follow that repository's README (`run_frequency.py --n 10000`).

## MixSan (Naive)

Commit `412147bcad69e73ec66d14768d415de8a642a7bf` keeps the three-stage check behind `-mllvm -mixsan-naive-check` in `SafeStack.cpp` and `ENABLE_MEMTAG_NAIVE` in `tcmalloc-implicit/src/common.h` (default off). Checkout that commit in a separate directory, enable both switches, and rebuild LLVM and TCMalloc. The default on that commit is still the unified check.

## Evaluation data

See `results/README.md`. Summary JSON files:

- `results/spec2006/runtime_canonical.json`
- `results/spec2006/maxrss_canonical.json`
- `results/tcmallocTest/mimalloc-bench/larson_summary.json`
- `results/memtag/results.json` (single-run suite verdicts)
- `results/rdtsc_tag/summary_compact.json`

The \(N=10^{4}\) MixSan frequency campaign lives in the 97-test repository under `results/`.

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| `env.sh` variables not set | `source env.sh`, not `./env.sh` |
| Clone looks like original RSan | You are on `master`; use `-b MixSan-LAM` |
| MixSan compiler not found | Build LLVM first |
| ASan random crash | `sudo sysctl vm.mmap_rnd_bits=28` |
| Juliet very slow | `sudo systemctl disable --now apport.service` |
| Tagged-pointer segfault | Kernel must enable LAM U57 |
