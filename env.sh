# Execute this script with: `source env.sh`

# Change Me! depending on where you cloned the RSan repo
export RSAN_TOP=/home/ubuntu/floris/rangesanitizer

# Change Me! depending on where you installed SPEC CPU 2006
export RSAN_SPEC2006=/home/ubuntu/floris/rangesanitizer/spec2006

# LLVM/Clang
export RSAN_LLVM=$RSAN_TOP/llvm-project-16/llvm
export RSAN_LLVM_BUILD=$RSAN_TOP/llvm-build
export RSAN_C=$RSAN_LLVM_BUILD/bin/clang
export RSAN_CXX=$RSAN_LLVM_BUILD/bin/clang++

# TCMalloc
export RSAN_TC_BASE=$RSAN_TOP/tcmalloc-baseline
export RSAN_TC_BASE_BUILD=$RSAN_TOP/tcmalloc-baseline-build
export RSAN_TC_IMPL=$RSAN_TOP/tcmalloc-implicit
export RSAN_TC_IMPL_BUILD=$RSAN_TOP/tcmalloc-impl-build
export RSAN_TC_EXPL=$RSAN_TOP/tcmalloc-explicit
export RSAN_TC_EXPL_BUILD=$RSAN_TOP/tcmalloc-expl-build

# Implicit tagging linking
export RSAN_LINKER_SCRIPT=$RSAN_TOP/linker-implicit/globals/linkglobals.ld
export RSAN_DYNAMIC_LINKER=$RSAN_TOP/linker-implicit/libdl/pld.so

# Infra
export RSAN_INFRA=$RSAN_TOP/infra

# Suggested for stable benchmarking (sudo)
# echo "performance" | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
