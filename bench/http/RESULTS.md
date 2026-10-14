# HTTP benchmark baseline

Measured 2026-10-13 at commit `e37a5b42250508fb485943f5a47f1b29d3b318e1`.

| Setting | Value |
|---|---|
| Host | 13th Gen Intel Core i7-13700K, 16 cores / 24 threads |
| OS | Linux x86_64, kernel 5.14.0-570.58.1.el9_6.x86_64 |
| Compiler | Ubuntu clang 18.1.3 |
| Build | CMake Release |
| Affinity | load and parse process CPU 0, server process CPU 1 |
| Sampling | 3 full warmups, 15 timed samples, median and MAD |

Command:

```sh
bench/http/run.sh build-http-release --samples 15 --warmup 3 \
    --cpu 0 --server-cpu 1 --out /tmp/bench-http-baseline.txt
```

### Parse only

| Workload | Iron GB/s | Iron Mmsg/s | pico GB/s | pico Mmsg/s | Iron/pico |
|---|---:|---:|---:|---:|---:|
| Small GET | 0.092 (+/- 2.4%) | 2.636 (+/- 2.4%) | 1.178 (+/- 12.4%) | 33.665 (+/- 12.4%) | 0.078 |
| Browser-like request | 0.200 (+/- 2.5%) | 0.320 (+/- 2.5%) | 1.965 (+/- 3.0%) | 3.149 (+/- 3.0%) | 0.102 |
| 1 MiB chunked upload | 1.918 (+/- 9.8%) | 0.002 (+/- 9.8%) | 21.507 (+/- 1.1%) | 0.021 (+/- 1.1%) | 0.089 |
| 128-request pipeline | 0.080 (+/- 1.7%) | 2.277 (+/- 1.7%) | 1.343 (+/- 5.1%) | 38.371 (+/- 5.1%) | 0.059 |

### Response serialization

| Workload | Iron GB/s | Iron Mmsg/s | C GB/s | C Mmsg/s | Iron/C |
|---|---:|---:|---:|---:|---:|
| 2-byte response | 0.409 (+/- 2.6%) | 3.936 (+/- 2.6%) | 1.749 (+/- 3.8%) | 16.814 (+/- 3.8%) | 0.234 |
| 4 KiB response | 11.345 (+/- 12.3%) | 2.700 (+/- 12.3%) | 44.720 (+/- 1.6%) | 10.645 (+/- 1.6%) | 0.254 |
| 1 MiB response | 10.924 (+/- 2.8%) | 0.010 (+/- 2.8%) | 30.910 (+/- 1.9%) | 0.029 (+/- 1.9%) | 0.353 |

### End to end

| Workload | Iron requests/s | pico requests/s | Iron/pico |
|---|---:|---:|---:|
| Minimal persistent server | 126069.504 (+/- 3.0%) | 144644.225 (+/- 4.4%) | 0.872 |

Bytes include HTTP framing. The parse results include model allocation and
release for Iron and chunk decoding for both implementations. The end-to-end
result excludes process startup and connection setup and includes parsing,
the two-byte handler, serialization and one sequential persistent-connection
round trip. See [README.md](README.md) for the complete methodology.
