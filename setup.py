#!/usr/bin/env python3
import sys
import os.path
curdir = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(curdir, "infra"))
import infra
from infra.context import Context

# LLVM Clang
CC = os.getenv("RSAN_C")
CXX = os.getenv("RSAN_CXX")

# Sanity check
if CC is None or not os.path.isfile(CC):
    print("Error: C compiler not found! ", CC)
    exit()

# Baseline TCMalloc
TC_BASE = os.getenv("RSAN_TC_BASE_BUILD")

# Explicit tagging TCMalloc
TC_EXPL  = os.getenv("RSAN_TC_EXPL_BUILD")

# Implicit tagging TCMalloc
TC_IMPL = os.getenv("RSAN_TC_IMPL_BUILD")

# Implicit tagging Linker
RSAN_LNK = os.getenv("RSAN_LINKER_SCRIPT")
RSAN_DL = os.getenv("RSAN_DYNAMIC_LINKER")

# Baseline CFLAGS and LDFLAGS
STD_CFLAGS = ["-fno-builtin-" + fn for fn in ("malloc", "calloc", "realloc", "free")] + ["-g", "-flto=full", "-Wno-int-conversion", "-Wno-deprecated-non-prototype"]
STD_LDFLAGS = ["-fuse-ld=lld"]

# RSan CFLAGS and LDFLAGS
RSAN_CFLAGS = ["-fsanitize=safe-stack"]
RSAN_LDFLAGS = ["-fsanitize=safe-stack"]

# Additional CFLAGS and LDFLAGS specific for implicit tagging
IMPL_CFLAGS = ["-no-pie", "-mbmi2"]
IMPL_LDFLAGS = ["-no-pie", "-T", RSAN_LNK, "-z", "max-page-size=0x1000", "-Wl,--dynamic-linker="+RSAN_DL]

# SPEC CPU2006
SPEC2006_DIR = os.getenv("RSAN_SPEC2006")

def get_tcmalloc_ldflags(build_dir):
    return ["-L"+build_dir+"/lib/", "-Wl,-rpath", "-Wl,"+build_dir+"/lib/", "-ltcmalloc_minimal"]

class Baseline(infra.Instance):
    name = 'baseline'
    def __init__(self, opt_level):
        self.name += "_" + opt_level
        self.opt = ["-" + opt_level]
    def configure(self, ctx):
        ctx.cc = CC
        ctx.cxx = CXX
        ctx.cflags += self.opt + STD_CFLAGS
        ctx.cxxflags += self.opt + STD_CFLAGS
        ctx.ldflags += STD_LDFLAGS + get_tcmalloc_ldflags(TC_BASE)

class RSanImplicit(infra.Instance):
    name = 'rsan-impl'
    def __init__(self, opt_level):
        self.name += "_" + opt_level
        self.opt = ["-" + opt_level]
    def configure(self, ctx):
        ctx.cc = CC
        ctx.cxx = CXX
        ctx.cflags += self.opt + STD_CFLAGS + RSAN_CFLAGS + IMPL_CFLAGS
        ctx.cxxflags += self.opt + STD_CFLAGS + RSAN_CFLAGS + IMPL_CFLAGS
        ctx.ldflags += STD_LDFLAGS + get_tcmalloc_ldflags(TC_IMPL) + RSAN_LDFLAGS + IMPL_LDFLAGS

class RSanExplicit(infra.Instance):
    name = 'rsan-expl'
    def __init__(self, opt_level):
        self.name += "_" + opt_level
        self.opt = ["-" + opt_level]
    def configure(self, ctx):
        ctx.cc = CC
        ctx.cxx = CXX
        ctx.cflags += self.opt + STD_CFLAGS + RSAN_CFLAGS
        ctx.cxxflags += self.opt + STD_CFLAGS + RSAN_CFLAGS
        ctx.ldflags += STD_LDFLAGS + get_tcmalloc_ldflags(TC_EXPL) + RSAN_LDFLAGS

class ASan(infra.Instance):
    name = 'asan'
    ASAN_CFLAGS = ["-fsanitize=address", "-fno-sanitize-address-use-after-scope", "-fsanitize-address-use-after-return=never"]
    ASAN_LDFLAGS = ["-fsanitize=address", "-fno-sanitize-address-use-after-scope", "-fsanitize-address-use-after-return=never"]
    def __init__(self, opt_level):
        self.name += "_" + opt_level
        self.opt = ["-" + opt_level]
    def configure(self, ctx):
        ctx.cc = CC
        ctx.cxx = CXX
        ctx.cflags += self.opt + STD_CFLAGS + self.ASAN_CFLAGS
        ctx.cxxflags += self.opt + STD_CFLAGS + self.ASAN_CFLAGS
        ctx.ldflags += STD_LDFLAGS + self.ASAN_LDFLAGS
    def prepare_run(self, ctx):
        # ASan sometimes crashes on startup with ASLR enabled. to disable:
        # disable user-space ASLR: echo 0 | sudo tee /proc/sys/kernel/randomize_va_space
        ctx.runenv["ASAN_OPTIONS"] = "alloc_dealloc_mismatch=0,detect_odr_violation=0,detect_leaks=0,detect_stack_use_after_return=0,detect_stack_use_after_scope=0"

if __name__ == "__main__":
    setup = infra.Setup(__file__)

    setup.add_instance(Baseline("O0"))
    setup.add_instance(Baseline("O2"))
    setup.add_instance(RSanImplicit("O0"))
    setup.add_instance(RSanImplicit("O2"))
    setup.add_instance(RSanExplicit("O0"))
    setup.add_instance(RSanExplicit("O2"))
    setup.add_instance(ASan("O0"))
    setup.add_instance(ASan("O2"))

    if os.path.exists(SPEC2006_DIR):
        setup.add_target(infra.targets.SPEC2006(
            force_cpu = 0,
            source = SPEC2006_DIR,
            source_type="installed",
            patches = ["asan", "rsan"],
        ))
    else:
        print("Warning! SPEC CPU2006 not found: ", SPEC2006_DIR)

    setup.add_target(infra.targets.Juliet())

    setup.main()
