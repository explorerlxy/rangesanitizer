#!/bin/bash

set -e
set -x

ROOT_DIR=$(pwd)

# Sanity check: was env.sh loaded?
if [ -z $RSAN_C ]
then
    echo $RSAN_C
    echo "Environment not set up! Execute source env.sh"
    exit 1
fi


# Detect which architecture to build for
ARCH=$(uname -m)
TARGET=""
echo "Detected system architecture: $ARCH"
if [ $ARCH == "x86_64" ]; then
    TARGET="X86"
elif [ $ARCH == "aarch64" ]; then
    TARGET="AArch64"
else
    echo "Unsupported architecture: $ARCH"
    exit 1
fi

# Build LLVM if it does not yet exist
if [[ ! -f $RSAN_C ]]
then
    mkdir -p $RSAN_LLVM_BUILD
    cd $RSAN_LLVM_BUILD
    cmake -DLLVM_ENABLE_PROJECTS="clang;lld" -DLLVM_ENABLE_RUNTIMES="compiler-rt" -DCMAKE_BUILD_TYPE=Release -GNinja -DLLVM_PARALLEL_LINK_JOBS=1 -DLLVM_TARGETS_TO_BUILD=$TARGET -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON -DCLANG_ENABLE_STATIC_ANALYZER=OFF -DCLANG_ENABLE_ARCMT=OFF ../llvm-project-16/llvm
    ninja
fi

# If after compilation we still do not have clang, exit
if [[ ! -f $RSAN_C ]]
then
  echo "Clang was not successfully built... exit"
  exit 1
fi

# Build TCMalloc baseline (clean TCMalloc v2.15)
if [[ ! -f $RSAN_TC_BASE_BUILD/lib/libtcmalloc.a ]]; then
    cd $ROOT_DIR
    git clone https://github.com/gperftools/gperftools.git --branch gperftools-2.15 tcmalloc-baseline
    cd $RSAN_TC_BASE
    autoreconf -i
    ./autogen.sh || true

    mkdir -p $ROOT_DIR/tcmalloc-baseline-build
    cd $RSAN_TC_BASE_BUILD
    ../tcmalloc-baseline/configure --prefix=$ROOT_DIR/tcmalloc-baseline-build
    make -j $(nproc)
    make install
fi

# Build TCMalloc-implicit: currently assumes x86
if [ $ARCH == "x86_64" ] && [[ ! -f $RSAN_TC_IMPL_BUILD/lib/libtcmalloc.a ]]; then
    cd $RSAN_TC_IMPL
    autoreconf -i
    ./autogen.sh || true

    mkdir -p $RSAN_TC_IMPL_BUILD
    cd $RSAN_TC_IMPL_BUILD
    CFLAGS="-g -O2 -mbmi2" CXXFLAGS="-g -O2 -mbmi2" ../tcmalloc-implicit/configure --prefix=$RSAN_TC_IMPL_BUILD
    make -j $(nproc)
    make install

# Build TCMalloc-explicit: currently assumes Arm TBI (Intel LAM support in a different RSan branch)
elif [ $ARCH == "aarch64" ] && [[ ! -f $RSAN_TC_EXPL_BUILD/lib/libtcmalloc.a ]]; then
    cd $RSAN_TC_EXPL
    autoreconf -i
    ./autogen.sh || true

    mkdir -p $RSAN_TC_EXPL_BUILD
    cd $RSAN_TC_EXPL_BUILD
    ../tcmalloc-explicit/configure --prefix=$RSAN_TC_EXPL_BUILD
    make -j $(nproc)
    make install
fi

# Build linker script for implicit tagging (x86)
if [ $ARCH == "x86_64" ] && [[ ! -f $RSAN_LINKER_SCRIPT ]]; then
    cd $ROOT_DIR/linker-implicit/globals
    python3 generate_linker_script.py
fi

# Build dynamic linker for implicit tagging (x86)
if [ $ARCH == "x86_64" ] && [[ ! -f $RSAN_DYNAMIC_LINKER ]]; then
    cd $ROOT_DIR/linker-implicit/libdl
    chmod +x run.sh
    ./run.sh
fi
echo "All done"

