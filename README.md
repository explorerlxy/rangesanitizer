# MixSan (Naive)

This snapshot is the **three-stage** MixSan check used as MixSan (Naive) in the paper. It is a descendant of commit `412147bcad69e73ec66d14768d415de8a642a7bf` with the naive path enabled by default and the original RSan README / draft notes removed.

The unified MixSan implementation lives on branch [`MixSan-LAM`](https://github.com/explorerlxy/rangesanitizer/tree/MixSan-LAM). Do not use GitHub `master`; that is original RSan.

## Three-stage check

On every instrumented access:

1. SizeTag gate (skip untagged pointers)
2. 6-bit identity-tag compare (pointer vs fused metadata)
3. Bound compare `(ptr + n) > meta`

Compiler: `InsertCheckNaive` in `llvm-project-16/llvm/lib/CodeGen/SafeStack.cpp` (`-mllvm -mixsan-naive-check`, default **on**).
Runtime: `ENABLE_MEMTAG_NAIVE` in `tcmalloc-implicit/src/common.h` (default **1**).

## Clone

```bash
git clone -b MixSan-Naive --recurse-submodules https://github.com/explorerlxy/rangesanitizer.git mixsan-naive
cd mixsan-naive
source env.sh
```

`env.sh` sets `RSAN_TOP` from the script location and exports `RSAN_MIX_*`. For `rsan-orig_*` / `baseline_*`, set `RSAN_ORIG` to an original RSan tree. For SPEC, set `RSAN_SPEC2006` (or use an untracked `env.local.sh`).

## Build

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

## Smoke test

```bash
cd $RSAN_TOP/examples
./test-implicit.sh
```

Valid access exits 0. Out-of-bounds and use-after-free raise SIGTRAP (exit 133).

## Paper comparison

Unified MixSan, SPEC/Larson summaries, and raw logs: https://github.com/explorerlxy/rangesanitizer/tree/MixSan-LAM  
Custom 97-test suite: https://github.com/explorerlxy/97-costum-benchmark
