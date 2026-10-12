#!/usr/bin/env python3
"""table.py - markdown tables from bench/json/run.sh results.

usage: table.py <results file> [--variant default|native]

For each measurement (variant, library, mode, input) a run gives one time
per sample; its median and MAD (median absolute deviation) are taken, and
with several runs (--repeat) the median of the run medians is reported, with
the spread between runs. Throughput is input bytes / median time, in GB/s
(10^9 bytes per second). A cell reads "1.23 ±2%" (MAD relative to the
median); "-" means the library has no such mode.
"""
import collections
import statistics
import sys

LIBS = ["iron", "iron_o3", "simdjson_dom", "simdjson_od", "yyjson", "rapidjson"]
LIB_NAMES = {"iron": "Iron (-O2)", "iron_o3": "Iron (-O3)", "simdjson_dom": "simdjson DOM",
             "simdjson_od": "simdjson On-Demand", "yyjson": "yyjson", "rapidjson": "RapidJSON"}
MODES = [("parse", "Parse to a tree"), ("validate", "Validation only"), ("serialize", "Serialize a parsed tree"),
         ("traverse", "Parse and visit every value"), ("parse_cold", "Parse, cold caches"),
         ("small_parse", "Many small documents: parse each"), ("small_validate", "Many small documents: validate each"),
         ("small_select", "Many small documents: parse, read 3 fields"),
         ("small_serialize", "Many small documents: serialize each")]
CORPORA = ["twitter.json", "citm_catalog.json", "canada.json", "gsoc-2018.json", "mesh.json", "numbers.json",
           "small.ndjson"]


def mad(xs):
    m = statistics.median(xs)
    return statistics.median(abs(x - m) for x in xs)


def main():
    path = sys.argv[1]
    variant = sys.argv[sys.argv.index("--variant") + 1] if "--variant" in sys.argv else "default"
    runs = collections.defaultdict(list)       # (lib, mode, corpus) -> [(median, mad)]
    sizes = {}
    header = []
    for line in open(path):
        if line.startswith("#"):
            header.append(line.strip("# \n"))
            continue
        f = line.split()
        if len(f) != 7 or f[0] != "RESULT" or f[1] != variant:
            continue
        _, _, lib, mode, corpus, nbytes, times = f
        ts = [int(t) for t in times.split(",") if t]
        runs[(lib, mode, corpus)].append((statistics.median(ts), mad(ts)))
        sizes[corpus] = int(nbytes)
    if header:
        print("<!-- %s -->" % " | ".join(dict.fromkeys(header)))
    for mode, title in MODES:
        rows = [c for c in CORPORA if any((l, mode, c) in runs for l in LIBS)]
        if not rows:
            continue
        libs = [l for l in LIBS if any((l, mode, c) in runs for c in rows)]
        print("\n**%s** (GB/s, median, ±MAD)\n" % title)
        print("| input | " + " | ".join(LIB_NAMES[l] for l in libs) + " | Iron / best |")
        print("|---|" + "---:|" * (len(libs) + 1))
        for c in rows:
            cells, best, iron = [], 0.0, None
            for l in libs:
                rs = runs.get((l, mode, c))
                if not rs:
                    cells.append("-")
                    continue
                med = statistics.median(r[0] for r in rs)
                rel = statistics.median(r[1] / r[0] for r in rs) * 100
                gbs = sizes[c] / med
                spread = ""
                if len(rs) > 1:
                    lo, hi = min(r[0] for r in rs), max(r[0] for r in rs)
                    spread = " (runs ±%.0f%%)" % ((hi - lo) / 2 / med * 100)
                cells.append("%.3f ±%.0f%%%s" % (gbs, rel, spread))
                if l.startswith("iron"):
                    iron = max(iron or 0.0, gbs)
                else:
                    best = max(best, gbs)
            ratio = "%.2fx" % (iron / best) if iron and best else "-"
            print("| %s (%.1f MB) | %s | %s |" % (c, sizes[c] / 1e6, " | ".join(cells), ratio))


if __name__ == "__main__":
    main()
