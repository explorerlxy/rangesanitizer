#!/bin/bash

# Sanity check: was env.sh loaded?
if [ -z $RSAN_C ]
then
    echo $RSAN_C
    echo "Environment not set up! Execute source env.sh"
    exit 1
fi


# Assumes implicit tagging (x86)

RSAN_CFLAGS="-O2 -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free -g -flto=full -fsanitize=safe-stack -mbmi2"
RSAN_LDFLAGS="-fuse-ld=lld -fsanitize=safe-stack -no-pie -T $RSAN_LINKER_SCRIPT -z max-page-size=0x1000 -Wl,--dynamic-linker=$RSAN_DYNAMIC_LINKER"
RSAN_TCMALLOC="-L$RSAN_TC_IMPL_BUILD/lib/ -Wl,-rpath -Wl,$RSAN_TC_IMPL_BUILD/lib/ -ltcmalloc_minimal"

echo -e "Compiling first test program..."
$RSAN_C oob.c $RSAN_CFLAGS -o oob $RSAN_LDFLAGS $RSAN_TCMALLOC
echo -e "Done."

echo -e "\nExecuting out-of-bounds testcase with valid index (expected: normal run)"
./oob 30

echo -e "\nExecuting out-of-bounds testcase with invalid index (expected: crash)"
./oob 40


echo -e "\nCompiling second test program..."
$RSAN_C uaf.c $RSAN_CFLAGS -o uaf $RSAN_LDFLAGS $RSAN_TCMALLOC
echo -e "Done."

echo -e "\nExecuting use-after-free testcase (expected: crash)"
./uaf
