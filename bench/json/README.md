# JSON benchmarks

Throughput of the `json` module against simdjson (DOM and On-Demand),
yyjson and RapidJSON, on the standard corpora and on a file of many small
documents. It is the baseline a rewrite of the module is measured against;
the current figures are in [RESULTS.md](RESULTS.md).

```sh
bench/json/run.sh build/ironc --native --repeat 3 --cpu 2 --out results.txt
bench/json/table.py results.txt                    # markdown tables, default build
bench/json/table.py results.txt --variant native   # the -march=native build
```

`run.sh --quick` is a smoke test (twitter.json and the small documents, few
samples). `fetch.sh` downloads everything into `$JSON_BENCH_DIR` (default
`~/.cache/iron-json-bench`) and checks it against `SHA256SUMS`.

## Inputs

| File | Size | What it stresses |
|---|---:|---|
| twitter.json | 0.63 MB | API response: strings with escapes and non-ASCII text, nested objects |
| citm_catalog.json | 1.7 MB | many small objects, integer heavy, repeated keys |
| canada.json | 2.3 MB | GeoJSON: nearly all floating point numbers |
| gsoc-2018.json | 3.3 MB | long strings, few numbers |
| mesh.json | 0.72 MB | arrays of floats and integers |
| numbers.json | 0.15 MB | one array of floats |
| small.ndjson | 13 MB | 40000 documents of 150 B to 2 KB, one per line: user records, orders and events, each parsed on its own as a service handles request bodies |

The corpora come from simdjson-data at a pinned commit; `gen_small.py`
generates small.ndjson deterministically.

## Modes

| Mode | Iron | simdjson DOM | simdjson On-Demand | yyjson | RapidJSON |
|---|---|---|---|---|---|
| parse | `Json.parse` (tree) | `dom::parser::parse` | - (lazy: see traverse) | `yyjson_read` | `Document::Parse` |
| validate | `JsonReader` (scan to a tape, no tree) | `parse` (its tape is the validation) | - | `yyjson_read` (no validate-only API) | SAX `Reader` with a null handler |
| serialize | `to_string()` of the parsed tree | `minify` of the parsed DOM | - | `yyjson_write` | `Writer` over the parsed `Document` |
| traverse | parse, then visit every value | parse, visit | iterate, visit (materializing every scalar) | read, visit | parse, visit |
| small_parse / small_validate | the same per document | same, parser reused | - | same | same |
| small_select | parse, read `id`, `email`, `active` | same | iterate, find the three fields | `yyjson_obj_get` | `operator[]` |
| small_serialize | serialize each parsed document | - (one DOM per parser) | - | same | same |

Every timed iteration includes freeing what it built (an Iron tree is
dropped inside the timed block). simdjson gets its inputs padded before
timing, as its API requires. RapidJSON parses with full precision and UTF-8
validation (`kParseFullPrecisionFlag | kParseValidateEncodingFlag`) so that
it does the work the other parsers do; a new `Document` per small document
means a new allocator each time, as a handler that does not pool them would.

## Method

- **Compilers.** Everything is built by the Iron toolchain's pinned clang
  (the one `ironc` uses). The references are compiled at `-O3` with the
  default target (x86-64 baseline on Linux, apple-m1 on macOS), as a release
  binary would be. The Iron program is built as `ironc build --release`
  builds it (`-O2`), and also at `-O3` through a wrapper toolchain that
  appends flags (`IRON_TOOLCHAIN`). `--native` repeats everything with
  `-march=native` (`-mcpu=native` on arm64).
- **Timing.** Each measurement is its own process: read the input, run the
  operation `warmup` times (default 5), then time `samples` iterations
  (default 25) with a monotonic clock. A cell reports bytes / median time in
  GB/s (10^9 bytes per second) and the median absolute deviation relative to
  the median. With `--repeat R` the sweep runs R times and a cell shows the
  median of the run medians and the spread between runs.
- **Caches.** By default caches are warm: the same input is parsed again and
  again (twitter.json and smaller fit in L2; the larger corpora do not fit in
  L1/L2 either way). `--cold` adds a parse measurement that sweeps a 128 MiB
  buffer before each sample.
- **CPU.** On Linux `--cpu N` pins every process to one core with taskset.
  macOS cannot pin threads to a core; run there with nothing else heavy
  running, and compare repeated runs. Results files record the date, kernel,
  pinning and compiler.

## Files

| File | Role |
|---|---|
| run.sh | builds every variant and runs the sweep, appending RESULT lines |
| capture.sh | validates each benchmark process result and rejects failed or incomplete sweeps |
| table.py | median / MAD / GB/s tables in markdown from a results file |
| fetch.sh, SHA256SUMS | pinned downloads |
| gen_small.py | small.ndjson |
| bench_iron.iron | the Iron side |
| bench_ref.cpp | simdjson, yyjson and RapidJSON |
