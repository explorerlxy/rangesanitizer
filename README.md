# RangeSanitizer
**RangeSanitizer** (RSan) detects spatial and temporal memory errors in C/C++ programs using efficient range checks.  

## Contents

| File | Description |
|------|-------------|
| `examples` | Example C programs to test the basic functionality of RSan |
| `infra` | Infrastructure to run benchmarks with compiler/allocator instrumentation |
| `linker-implicit` | Linker script (globals) and custom dynamic linker for implicit tagging |
| `llvm-project-16` | Modified LLVM 16.0.6 for compiler instrumentation |
| `tcmalloc-implicit` | Modified TCMalloc 2.15 for memory allocation with metadata (implicit tagging) |
| `tcmalloc-explicit` | Modified TCMalloc 2.15 for memory allocation with metadata (explicit tagging) |
| `env.sh` | Script to configure the running environment |
| `install-all.sh` | Script to automatically install RSan |
| `setup.py` | Script to drive the instrumentation infra |

## Dependencies
Tested on:  
* (x86) - i9-13900K - Ubuntu 22.04 - glibc 2.35 - Stock Linux kernel v5.15  
* (Arm) - Apple M2 - Debian 12 - glibc 2.36 - Asahi Linux kernel v6.4.0  

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
./install-all.sh
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
### SPEC CPU
TODO

### Juliet Test Suite
Execute the following command (assuming implicit tagging as target: `rsan-impl_O0`, otherwise: `rsan-expl_O0`)  
Note that we run Juliet with optimization flag `-O0`, because standard optimizations (e.g., `-O2`) hide bugs in the test cases.  
`python3 setup.py run juliet rsan-impl_O0 --build --parallel=proc --parallelmax=$(nproc) --cwe 121 122 124 126 127 415 416`

Expected output (in any order):  
```
[INFO] CWE127: Passed 1001/1001 GOOD tests
[INFO] CWE127: Passed 1001/1001 BAD tests
[INFO] CWE121: Passed 2885/2885 GOOD tests
[INFO] CWE121: Passed 2885/2885 BAD tests
[INFO] CWE416: Passed 374/374 GOOD tests
[INFO] CWE416: Passed 374/374 BAD tests
[INFO] CWE124: Passed 1001/1001 GOOD tests
[INFO] CWE124: Passed 1001/1001 BAD tests
[INFO] CWE126: Passed 657/657 GOOD tests
[INFO] CWE126: Passed 657/657 BAD tests
[INFO] CWE415: Passed 799/799 GOOD tests
[INFO] CWE415: Passed 799/799 BAD tests
[INFO] CWE122: Passed 3365/3365 GOOD tests
[INFO] CWE122: Passed 3365/3365 BAD tests
```

Feel free to also test Juliet with AddressSanitizer (ASan): change the target of setup.py from `rsan-impl_O0` to `asan_O0`.
