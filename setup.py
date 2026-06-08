#!/usr/bin/env python3
import sys
import os.path
curdir = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(curdir, "infra"))
import infra
from infra.context import Context

# ============================================================
# Baseline / RSan-Orig  —  use worktree (RSAN_ORIG) builds
# ============================================================
CC_ORIG   = os.getenv("RSAN_ORIG_C")
CXX_ORIG  = os.getenv("RSAN_ORIG_CXX")
TC_BASE   = os.getenv("RSAN_ORIG_TC_BASE_BUILD")
TC_IMPL   = os.getenv("RSAN_ORIG_TC_IMPL_BUILD")
LNK_ORIG  = os.getenv("RSAN_ORIG_LINKER_SCRIPT")
DL_ORIG   = os.getenv("RSAN_ORIG_DYNAMIC_LINKER")

# ============================================================
# MixSan (SmartMixSan-LAM)  —  use main dir (RSAN_TOP) builds
# ============================================================
CC_MIX    = os.getenv("RSAN_MIX_C")
CXX_MIX   = os.getenv("RSAN_MIX_CXX")
TC_MIX    = os.getenv("RSAN_MIX_TC_IMPL_BUILD")
LNK_MIX   = os.getenv("RSAN_MIX_LINKER_SCRIPT")
DL_MIX    = os.getenv("RSAN_MIX_DYNAMIC_LINKER")

# Sanity check: at least the orig compiler must exist
if CC_ORIG is None or not os.path.isfile(CC_ORIG):
    print("Error: RSan-Orig C compiler not found! ", CC_ORIG)
    print("Build the worktree LLVM first, or check env.sh")
    exit()

# Baseline CFLAGS and LDFLAGS
STD_CFLAGS = ["-fno-builtin-" + fn for fn in ("malloc", "calloc", "realloc", "free")] + ["-g", "-flto=full", "-Wno-int-conversion", "-Wno-deprecated-non-prototype"]
STD_LDFLAGS = ["-fuse-ld=lld"]

# RSan CFLAGS and LDFLAGS
RSAN_CFLAGS = ["-fsanitize=safe-stack"]
RSAN_LDFLAGS = ["-fsanitize=safe-stack"]

# SPEC CPU paths
SPEC2006_DIR = os.getenv("RSAN_SPEC2006")
SPEC2017_DIR = os.getenv("RSAN_SPEC2017")

def get_tcmalloc_ldflags(build_dir):
    return ["-L"+build_dir+"/lib/", "-Wl,-rpath", "-Wl,"+build_dir+"/lib/", "-ltcmalloc_minimal"]

def _impl_cflags(lnk, dl):
    return ["-no-pie", "-mbmi2"]

def _impl_ldflags(lnk, dl):
    return ["-no-pie", "-T", lnk, "-z", "max-page-size=0x1000", "-Wl,--dynamic-linker="+dl]

# ============================================================
# Instances
# ============================================================

class Baseline(infra.Instance):
    """Vanilla TCMalloc, no sanitizer instrumentation.  Uses worktree builds."""
    name = 'baseline'
    def __init__(self, opt_level):
        self.name += "_" + opt_level
        self.opt = ["-" + opt_level]
    def configure(self, ctx):
        ctx.cc = CC_ORIG
        ctx.cxx = CXX_ORIG
        ctx.cflags += self.opt + STD_CFLAGS
        ctx.cxxflags += self.opt + STD_CFLAGS
        ctx.ldflags += STD_LDFLAGS + get_tcmalloc_ldflags(TC_BASE)

class RSanOrig(infra.Instance):
    """Original RSan with implicit tagging (SafeStack + TCMalloc implicit).
       Uses worktree builds."""
    name = 'rsan-orig'
    def __init__(self, opt_level):
        self.name += "_" + opt_level
        self.opt = ["-" + opt_level]
    def configure(self, ctx):
        ctx.cc = CC_ORIG
        ctx.cxx = CXX_ORIG
        ctx.cflags += self.opt + STD_CFLAGS + RSAN_CFLAGS + _impl_cflags(LNK_ORIG, DL_ORIG)
        ctx.cxxflags += self.opt + STD_CFLAGS + RSAN_CFLAGS + _impl_cflags(LNK_ORIG, DL_ORIG)
        ctx.ldflags += STD_LDFLAGS + get_tcmalloc_ldflags(TC_IMPL) + RSAN_LDFLAGS + _impl_ldflags(LNK_ORIG, DL_ORIG)

class MixSan(infra.Instance):
    """SmartMixSan-LAM: hybrid sanitizer under development.
       Uses main-dir builds (RSAN_TOP)."""
    name = 'mixsan'
    def __init__(self, opt_level):
        self.name += "_" + opt_level
        self.opt = ["-" + opt_level]
    def configure(self, ctx):
        ctx.cc = CC_MIX
        ctx.cxx = CXX_MIX
        ctx.cflags += self.opt + STD_CFLAGS + RSAN_CFLAGS + _impl_cflags(LNK_MIX, DL_MIX)
        ctx.cxxflags += self.opt + STD_CFLAGS + RSAN_CFLAGS + _impl_cflags(LNK_MIX, DL_MIX)
        ctx.ldflags += STD_LDFLAGS + get_tcmalloc_ldflags(TC_MIX) + RSAN_LDFLAGS + _impl_ldflags(LNK_MIX, DL_MIX)

class MixSanCompilerRSanTC(infra.Instance):
    """MixSan compiler (3-stage check) + RSan tcmalloc (quarantine, no MemTag).
       Isolates compiler pass overhead: MemTag=0 → Stage 2 skipped, spatial-only."""
    name = 'mixsan-compiler'
    def __init__(self, opt_level):
        self.name += "_" + opt_level
        self.opt = ["-" + opt_level]
    def configure(self, ctx):
        ctx.cc = CC_MIX
        ctx.cxx = CXX_MIX
        ctx.cflags += self.opt + STD_CFLAGS + RSAN_CFLAGS + _impl_cflags(LNK_ORIG, DL_ORIG)
        ctx.cxxflags += self.opt + STD_CFLAGS + RSAN_CFLAGS + _impl_cflags(LNK_ORIG, DL_ORIG)
        ctx.ldflags += STD_LDFLAGS + get_tcmalloc_ldflags(TC_IMPL) + RSAN_LDFLAGS + _impl_ldflags(LNK_ORIG, DL_ORIG)

class RSanExplicit(infra.Instance):
    """RSan with explicit tagging (for additional comparison). Uses worktree builds."""
    name = 'rsan-expl'
    def __init__(self, opt_level):
        self.name += "_" + opt_level
        self.opt = ["-" + opt_level]
    def configure(self, ctx):
        ctx.cc = CC_ORIG
        ctx.cxx = CXX_ORIG
        ctx.cflags += self.opt + STD_CFLAGS + RSAN_CFLAGS
        ctx.cxxflags += self.opt + STD_CFLAGS + RSAN_CFLAGS
        # explicit TC also lives in worktree
        ctx.ldflags += STD_LDFLAGS + get_tcmalloc_ldflags(
            os.getenv("RSAN_ORIG_TC_EXPL_BUILD", "")) + RSAN_LDFLAGS

class ASan(infra.Instance):
    """AddressSanitizer (for additional comparison). Uses worktree compiler."""
    name = 'asan'
    ASAN_CFLAGS = ["-fsanitize=address", "-fno-sanitize-address-use-after-scope", "-fsanitize-address-use-after-return=never"]
    ASAN_LDFLAGS = ["-fsanitize=address", "-fno-sanitize-address-use-after-scope", "-fsanitize-address-use-after-return=never"]
    def __init__(self, opt_level):
        self.name += "_" + opt_level
        self.opt = ["-" + opt_level]
    def configure(self, ctx):
        ctx.cc = CC_ORIG
        ctx.cxx = CXX_ORIG
        ctx.cflags += self.opt + STD_CFLAGS + self.ASAN_CFLAGS
        ctx.cxxflags += self.opt + STD_CFLAGS + self.ASAN_CFLAGS
        ctx.ldflags += STD_LDFLAGS + self.ASAN_LDFLAGS
    def prepare_run(self, ctx):
        ctx.runenv["ASAN_OPTIONS"] = "alloc_dealloc_mismatch=0,detect_odr_violation=0,detect_leaks=0,detect_stack_use_after_return=0,detect_stack_use_after_scope=0"


# ============================================================
# MemTag Test Suite Target
# ============================================================

class MemTagTest(infra.Target):
    """MixSan MemTag-specific test suite for RSan vs MixSan comparison."""

    name = "memtag_test"
    aggregation_field = "category"

    TEST_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests", "memtag")
    CATS = ["cat1_uaf_reuse", "cat2_double_free", "cat3_nonlinear_oob",
            "cat4_realloc", "cat5_edge_cases",
            "cat7_spatial_variants", "cat8_cpp_temporal", "cat9_alloc_patterns",
            "cat10_data_structures", "cat11_memtag_stats", "cat12_thread",
            "gen_uaf_exhaust", "gen_oob_skip", "gen_multi_cycle",
            "gen_df_reuse", "gen_realloc_chain", "gen_thread_race",
            "gen_cpp_patterns", "gen_size_mix", "gen_ds_variants",
            "gen_alloc_funcs", "gen_statistical"]

    def is_fetched(self, ctx):
        return os.path.isdir(self.TEST_DIR)

    def fetch(self, ctx):
        pass  # tests are in-repo

    def build(self, ctx, instance, pool=None):
        test_dir = self.TEST_DIR
        if not os.path.isdir(test_dir):
            ctx.log.error(f"Test directory not found: {test_dir}")
            return

        # Set up compiler/tcmalloc vars from instance config
        env = os.environ.copy()
        env["CC_BASE"]  = os.getenv("RSAN_ORIG_C", "")
        env["CC_RSAN"]  = os.getenv("RSAN_ORIG_C", "")
        env["CC_MIX"]   = os.getenv("RSAN_MIX_C", "")
        env["TC_BASE"]  = os.getenv("RSAN_ORIG_TC_BASE_BUILD", "")
        env["TC_RSAN"]  = os.getenv("RSAN_ORIG_TC_IMPL_BUILD", "")
        env["TC_MIX"]   = os.getenv("RSAN_MIX_TC_IMPL_BUILD", "")
        env["LNK_RSAN"] = os.getenv("RSAN_ORIG_LINKER_SCRIPT", "")
        env["DL_RSAN"]  = os.getenv("RSAN_ORIG_DYNAMIC_LINKER", "")
        env["LNK_MIX"]  = os.getenv("RSAN_MIX_LINKER_SCRIPT", "")
        env["DL_MIX"]   = os.getenv("RSAN_MIX_DYNAMIC_LINKER", "")

        target = instance.name  # e.g., "baseline_O0", "rsan-orig_O0", "mixsan_O0"
        ctx.log.info(f"Building memtag_test for {target}")

        infra.util.run(ctx, ["make", "-C", test_dir, target], env=env)

    def run(self, ctx, instance, pool=None):
        test_dir = self.TEST_DIR
        target = instance.name
        ctx.log.info(f"Running memtag_test for {target}")

        for cat in self.CATS:
            cat_dir = os.path.join(test_dir, cat)
            if not os.path.isdir(cat_dir):
                continue
            for f in sorted(os.listdir(cat_dir)):
                if not f.endswith(".c"):
                    continue
                name = f[:-2]
                binary = os.path.join(cat_dir, f"{name}_{target}")
                if not os.path.isfile(binary) or not os.access(binary, os.X_OK):
                    continue

                proc = infra.util.run(ctx, [binary], allow_error=True, teeout=False)
                ec = proc.returncode
                verdict = "DETECT" if ec == 133 else ("PASS" if ec == 0 else f"CRASH({ec})")

                outdir = os.path.join(ctx.paths.results, instance.name, cat)
                os.makedirs(outdir, exist_ok=True)
                outfile = os.path.join(outdir, name)
                with open(outfile, "w") as fh:
                    fh.write(f"exit_code: {ec}\n")
                    fh.write(f"verdict: {verdict}\n")
                    fh.write(f"category: {cat}\n")

    def parse_outfile(self, ctx, outfile):
        results = {}
        with open(outfile) as fh:
            for line in fh:
                if ": " in line:
                    k, v = line.strip().split(": ", 1)
                    results[k] = v
        if "exit_code" in results:
            results["exit_code"] = int(results["exit_code"])
            results["detected"]  = 1 if results["exit_code"] == 133 else 0
            results["missed"]    = 1 if results["exit_code"] == 0 else 0
        yield results

    def reportable_fields(self):
        return {
            "verdict":    "DETECT (133), PASS (0), or CRASH(N)",
            "exit_code":  "Process exit code (133 = SIGTRAP)",
            "detected":   "1 if bug was detected, 0 otherwise",
            "missed":     "1 if bug was missed, 0 otherwise",
            "category":   "Test category name",
        }


if __name__ == "__main__":
    setup = infra.Setup(__file__)

    # Primary instances: baseline / rsan-orig / mixsan
    for opt in ("O0", "O2"):
        setup.add_instance(Baseline(opt))
        setup.add_instance(RSanOrig(opt))
        setup.add_instance(MixSan(opt))
        setup.add_instance(MixSanCompilerRSanTC(opt))  # temporary: isolate compiler pass overhead

    # Additional comparison instances
    for opt in ("O0", "O2"):
        setup.add_instance(RSanExplicit(opt))
        setup.add_instance(ASan(opt))

    # SPEC CPU2006
    if SPEC2006_DIR and os.path.exists(SPEC2006_DIR):
        setup.add_target(infra.targets.SPEC2006(
            force_cpu = 0,
            source = SPEC2006_DIR,
            source_type = "installed",
            patches = ["dealII-stddef", "gcc-init-ptr", "omnetpp-invalid-ptrcheck", "asan", "rsan"],
        ))
    else:
        print("Warning! SPEC CPU2006 not found: ", SPEC2006_DIR)

    # SPEC CPU2017
    if SPEC2017_DIR and os.path.exists(SPEC2017_DIR):
        setup.add_target(infra.targets.SPEC2017(
            force_cpu = 0,
            source = SPEC2017_DIR,
            source_type = "installed",
        ))
    else:
        print("Warning! SPEC CPU2017 not found: ", SPEC2017_DIR)

    setup.add_target(infra.targets.Juliet())
    setup.add_target(MemTagTest())

    setup.main()
