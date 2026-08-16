# MixSan evaluation artifacts

Summary JSON files for the MixSan-LAM paper numbers. Raw logs remain next to them.

| File | Contents |
|------|----------|
| `spec2006/runtime_canonical.json` | SPEC CPU2006 per-iteration runtimes and geomean extras |
| `spec2006/maxrss_canonical.json` | SPEC CPU2006 peak RSS and geomean extras |
| `spec2006/raw_logs/` | SPEC `runspec` / instance logs |
| `tcmallocTest/mimalloc-bench/larson_summary.json` | Larson 10-run means, CV, min/max |
| `tcmallocTest/mimalloc-bench/data/` | Per-run Larson `Throughput =` lines |
| `memtag/results.json` | Single-run DETECT/PASS for the 97-test programs |
| `rdtsc_tag/summary_compact.json` | `rdtsc` 6-bit tag uniformity (\(\chi^2\), TVD) |

The \(N=10^{4}\) MixSan frequency tables used in the manuscript are in the standalone suite: https://github.com/explorerlxy/97-costum-benchmark (`results/detection_frequency_n10000.json`, `results/table4_mixsan_n10000.json`).
