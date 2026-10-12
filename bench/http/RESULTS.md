# HTTP benchmark baseline

Measured 2026-10-12 at commit `0555f414b2412efca7c66882067276e868ce365d`.

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
| Small GET | 0.070 (+/- 2.0%) | 1.997 (+/- 2.0%) | 0.957 (+/- 9.9%) | 27.353 (+/- 9.9%) | 0.073 |
| Browser-like request | 0.124 (+/- 3.2%) | 0.199 (+/- 3.2%) | 1.438 (+/- 17.6%) | 2.305 (+/- 17.6%) | 0.086 |
| 1 MiB chunked upload | 1.819 (+/- 9.8%) | 0.002 (+/- 9.8%) | 20.489 (+/- 5.0%) | 0.020 (+/- 5.0%) | 0.089 |
| 128-request pipeline | 0.059 (+/- 2.8%) | 1.676 (+/- 2.8%) | 1.331 (+/- 9.7%) | 38.029 (+/- 9.7%) | 0.044 |

### Response serialization

| Workload | Iron GB/s | Iron Mmsg/s | C GB/s | C Mmsg/s | Iron/C |
|---|---:|---:|---:|---:|---:|
| 2-byte response | 0.243 (+/- 4.0%) | 2.334 (+/- 4.0%) | 1.592 (+/- 8.4%) | 15.312 (+/- 8.4%) | 0.152 |
| 4 KiB response | 7.966 (+/- 4.9%) | 1.896 (+/- 4.9%) | 32.347 (+/- 14.3%) | 7.700 (+/- 14.3%) | 0.246 |
| 1 MiB response | 9.424 (+/- 6.2%) | 0.009 (+/- 6.2%) | 29.299 (+/- 7.4%) | 0.028 (+/- 7.4%) | 0.322 |

### End to end

| Workload | Iron requests/s | pico requests/s | Iron/pico |
|---|---:|---:|---:|
| Minimal persistent server | 115255.371 (+/- 11.7%) | 139337.307 (+/- 7.4%) | 0.827 |

Bytes include HTTP framing. The parse results include model allocation and
release for Iron and chunk decoding for both implementations. The end-to-end
result excludes process startup and connection setup and includes parsing,
the two-byte handler, serialization and one sequential persistent-connection
round trip. See [README.md](README.md) for the complete methodology.
