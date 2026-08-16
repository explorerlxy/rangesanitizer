# Execute this script with: `source env.sh`
# Do not run it as `./env.sh`.

_RSAN_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# MixSan tree (this clone / MixSan-LAM branch)
export RSAN_TOP="${RSAN_TOP:-$_RSAN_ROOT}"

# Original RSan tree, required only for rsan-orig_* / baseline_* in setup.py.
# Override before sourcing, or keep a sibling checkout named rangesanitizer-original.
if [ -z "${RSAN_ORIG:-}" ]; then
  if [ -d "$_RSAN_ROOT/../rangesanitizer-original" ]; then
    export RSAN_ORIG="$(cd "$_RSAN_ROOT/../rangesanitizer-original" && pwd)"
  fi
fi

# SPEC CPU2006 install (required for the spec2006 target)
# export RSAN_SPEC2006=/path/to/cpu2006

# Optional machine-local overrides (not committed)
if [ -f "$_RSAN_ROOT/env.local.sh" ]; then
  # shellcheck disable=SC1091
  source "$_RSAN_ROOT/env.local.sh"
fi

# ---------------------------------------------------------------------------
# MixSan toolchain (this tree)
# ---------------------------------------------------------------------------
export RSAN_MIX_LLVM=$RSAN_TOP/llvm-project-16/llvm
export RSAN_MIX_LLVM_BUILD=$RSAN_TOP/llvm-build
export RSAN_MIX_C=$RSAN_MIX_LLVM_BUILD/bin/clang
export RSAN_MIX_CXX=$RSAN_MIX_LLVM_BUILD/bin/clang++
export RSAN_MIX_TC_IMPL=$RSAN_TOP/tcmalloc-implicit
export RSAN_MIX_TC_IMPL_BUILD=$RSAN_TOP/tcmalloc-impl-build
export RSAN_MIX_LINKER_SCRIPT=$RSAN_TOP/linker-implicit/globals/linkglobals.ld
export RSAN_MIX_DYNAMIC_LINKER=$RSAN_TOP/linker-implicit/libdl/pld.so

# ---------------------------------------------------------------------------
# Original RSan toolchain (RSAN_ORIG tree)
# ---------------------------------------------------------------------------
if [ -n "${RSAN_ORIG:-}" ]; then
  export RSAN_ORIG_LLVM=$RSAN_ORIG/llvm-project-16/llvm
  export RSAN_ORIG_LLVM_BUILD=$RSAN_ORIG/llvm-build
  export RSAN_ORIG_C=$RSAN_ORIG_LLVM_BUILD/bin/clang
  export RSAN_ORIG_CXX=$RSAN_ORIG_LLVM_BUILD/bin/clang++
  export RSAN_ORIG_TC_BASE=$RSAN_ORIG/tcmalloc-baseline
  export RSAN_ORIG_TC_BASE_BUILD=$RSAN_ORIG/tcmalloc-baseline-build
  export RSAN_ORIG_TC_IMPL=$RSAN_ORIG/tcmalloc-implicit
  export RSAN_ORIG_TC_IMPL_BUILD=$RSAN_ORIG/tcmalloc-impl-build
  export RSAN_ORIG_TC_EXPL=$RSAN_ORIG/tcmalloc-explicit
  export RSAN_ORIG_TC_EXPL_BUILD=$RSAN_ORIG/tcmalloc-expl-build
  export RSAN_ORIG_LINKER_SCRIPT=$RSAN_ORIG/linker-implicit/globals/linkglobals.ld
  export RSAN_ORIG_DYNAMIC_LINKER=$RSAN_ORIG/linker-implicit/libdl/pld.so
fi

# ---------------------------------------------------------------------------
# Legacy aliases used by examples/test-implicit.sh
# ---------------------------------------------------------------------------
export RSAN_LLVM=$RSAN_MIX_LLVM
export RSAN_LLVM_BUILD=$RSAN_MIX_LLVM_BUILD
export RSAN_C=$RSAN_MIX_C
export RSAN_CXX=$RSAN_MIX_CXX
export RSAN_TC_IMPL=$RSAN_MIX_TC_IMPL
export RSAN_TC_IMPL_BUILD=$RSAN_MIX_TC_IMPL_BUILD
export RSAN_LINKER_SCRIPT=$RSAN_MIX_LINKER_SCRIPT
export RSAN_DYNAMIC_LINKER=$RSAN_MIX_DYNAMIC_LINKER
export RSAN_INFRA=$RSAN_TOP/infra
if [ -n "${RSAN_ORIG:-}" ]; then
  export RSAN_TC_BASE=$RSAN_ORIG_TC_BASE
  export RSAN_TC_BASE_BUILD=$RSAN_ORIG_TC_BASE_BUILD
  export RSAN_TC_EXPL=$RSAN_ORIG_TC_EXPL
  export RSAN_TC_EXPL_BUILD=$RSAN_ORIG_TC_EXPL_BUILD
fi
