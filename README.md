# RangeSanitizer
**RangeSanitizer** (RSan) detects spatial and temporal memory errors in C/C++ programs using efficient range checks.  

**Paper**: https://download.vusec.net/papers/rsan_sec25.pdf

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
sudo apt install ninja-build cmake gcc-9 autoconf2.69 bison build-essential flex texinfo libtool zlib1g-dev unzip gawk
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
Use the following command to run SPEC CPU 2006.  
Note that you need to provide your own SPEC installation directory in `env.sh`.    
```
python3 setup.py run spec2006 baseline_O2 rsan-impl_O2 asan_O2 --build --parallel=proc --parallelmax=1
```
This command will run the baseline, RSan, and ASan.  
To view the results (runtime and memory overhead), use the following command:  
```
python3 setup.py report spec2006 results/last --field runtime:median maxrss:median
```

This is an example expected output (results may vary across devices):
```
+ spec2006 aggregated data ------------------------------------------------+
|               asan_O2          baseline_O2         rsan-impl_O2          |
|               runtime maxrss   runtime     maxrss  runtime      maxrss   |
|benchmark      median  median   median      median  median       median   |
+--------------------------------------------------------------------------+
|400.perlbench  442      5490512 107         1238072 208           5273852 |
|401.bzip2      297      3579584 196         3470256 300           3856296 |
|403.gcc        242     13467776  82.8       4525712 216          13841236 |
|429.mcf        124      1935468  95.9       1725320 124           1759048 |
|433.milc       175       982460 121          719884 132           1556064 |
|444.namd       184        61160 122           54832 173            124184 |
|445.gobmk      287      1375380 179          183152 261           1613240 |
|447.dealII     207      1769588  86.7        944444 124           2013068 |
|450.soplex     121      1272888  77.9        948388 107           1451692 |
|453.povray     101       236172  40.7         11944  73.9          242576 |
|456.hmmer      283       814040 103           65452 142            960664 |
|458.sjeng      357       184600 203          185952 300            216424 |
|462.libquantum 140       366636 110          138812 118            356540 |
|464.h264ref    362       725260 158          136008 236            813456 |
|470.lbm         95.9     476124  78.1        426032  78.7          453484 |
|471.omnetpp    236       777204  82.5        165468 186            870932 |
|473.astar      209      1489268 145          491808 209           1974180 |
|482.sphinx3    275       412984 165           51064 204            582040 |
|483.xalancbmk  150      1418344  44.7        467044 111           2066312 |
+--------------------------------------------------------------------------+
```

Geomean overhead aggregates can be calculated by dividing each entry by the baseline entry, and then taking the geomean of the decimal multipliers.  
For the runtime results of the output above, this gives `51%` overhead for RSan, and `94%` for ASan:  
| Benchmark      | **Base** | **RSan** | **RSan/Base** | **ASan** | **ASan/Base** |
|----------------|:--------:|:--------:|:--------:|:--------:|:--------:|
| 400.perlbench  |    107   |    208   |   1.94   |    442   |   4.13   |
| 401.bzip2      |    196   |    300   |   1.53   |    297   |   1.52   |
| 403.gcc        |   82.8   |    216   |   2.61   |    242   |   2.92   |
| 429.mcf        |   95.9   |    124   |   1.29   |    124   |   1.29   |
| 433.milc       |    121   |    132   |   1.09   |    175   |   1.45   |
| 444.namd       |    122   |    173   |   1.42   |    184   |   1.51   |
| 445.gobmk      |    179   |    261   |   1.46   |    287   |   1.60   |
| 447.dealII     |   86.7   |    124   |   1.43   |    207   |   2.39   |
| 450.soplex     |   77.9   |    107   |   1.37   |    121   |   1.55   |
| 453.povray     |   40.7   |   73.9   |   1.82   |    101   |   2.48   |
| 456.hmmer      |    103   |    142   |   1.38   |    283   |   2.75   |
| 458.sjeng      |    203   |    300   |   1.48   |    357   |   1.76   |
| 462.libquantum |    110   |    118   |   1.07   |    140   |   1.27   |
| 464.h264ref    |    158   |    236   |   1.49   |    362   |   2.29   |
| 470.lbm        |   78.1   |   78.7   |   1.01   |   95.9   |   1.23   |
| 471.omnetpp    |   82.5   |    186   |   2.25   |    236   |   2.86   |
| 473.astar      |    145   |    209   |   1.44   |    209   |   1.44   |
| 482.sphinx3    |    165   |    204   |   1.24   |    275   |   1.67   |
| 483.xalancbmk  |   44.7   |    111   |   2.48   |    150   |   3.36   |
| **geomean**    |          |          | **1.51** |          | **1.94** |


You can also view the results of other runs by changing `results/last` into a specific `results` directory, e.g. `results/run.2025-01-07.11-49-17`.  




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

## Troubleshooting
If ASan randomly crashes sometimes, this is likely due to [recent changes](https://llbit.github.io/programming/2024/03/19/aslr-asan-problem.html) to ASLR in the Linux kernel.  
Currently, the workaround is:
`sudo sysctl vm.mmap_rnd_bits=28`

If Juliet runs very slowly, it could be due to core dumps being processed on every buggy (crashing) binary. Consider disabling apport:
```
sudo systemctl disable --now apport.service
sudo service apport stop
```
