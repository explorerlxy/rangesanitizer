# RangeSanitizer
**RangeSanitizer** (RSan) detects spatial and temporal memory errors in C/C++ programs using efficient range checks.  

## Dependencies
Tested on:  
- (x86) - i9-13900K - Ubuntu 22.04 - glibc 2.35 - Stock Linux kernel v5.15  
- (Arm) - Apple M2 - Debian 12 - glibc 2.36 - Asahi Linux kernel v6.4.0  

```
sudo apt install ninja-build cmake gcc-9 autoconf2.69 bison build-essential flex texinfo libtool zlib1g-dev
pip3 install psutil terminaltables
```

## How to install
```
git clone https://github.com/vusec/rangesanitizer.git --recurse-submodules  
```

Edit `env.sh` and update `RSAN_TOP` with the full path where you cloned this repository.  

(OPTIONAL) To run SPEC benchmarks, update the variable `RSAN_SPEC2006` with the full path of your SPEC installation.  

Then, load the environment in your current shell:
```
source env.sh
```

**IMPORTANT**: always ensure to load `env.sh` in your terminal before doing any of the following steps

Finally, let's install everything. Building LLVM can take some time.  
If building LLVM crashes, it may be OOM. Try building with less cores.  
The installation script installs RSan with implicit tagging on x86 or explicit tagging on Arm.  
On Arm, we assume the system has Arm TBI (Top Byte Ignore) available.  

```
./install.sh
```

## Test if RSan is working
Compile the example programs in the `examples` directory.  
On x86: run `test-implicit.sh`  
On Arm: run `test-explicit.sh`

```
cd examples
./test-implicit.sh
```

Currently, RSan triggers a software breakpoint upon memory errors, for convenient debugging and immediate aborts outside of a debugger.

Expected output:
```
Compiling first test program...
Done.

Executing out-of-bounds testcase with valid index (expected: normal run)
obj allocated at: 0xc00000040c0
access at index 30...
0

Executing out-of-bounds testcase with invalid index (expected: crash)
obj allocated at: 0xc00000040c0
access at index 40...
./test-implicit.sh: line 26: 1301175 Trace/breakpoint trap   ./oob 40

Compiling second test program...
Done.

Executing use-after-free testcase (expected: crash)
obj allocated at: 0xc0000004080
obj 0xc0000004080 has been deallocated
use after free...
./test-implicit.sh: line 34: 1301211 Trace/breakpoint trap   ./uaf
```

## Benchmarks
TODO
