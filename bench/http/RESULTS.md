# HTTP benchmark baseline

Measured 2026-10-13 at commit `f1f1d2c0c64ea7a23c17f21a3743c4734f88cd4e`.

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
| Small GET | 0.096 (+/- 2.4%) | 2.744 (+/- 2.4%) | 1.401 (+/- 5.5%) | 40.026 (+/- 5.5%) | 0.069 |
| Browser-like request | 0.229 (+/- 3.7%) | 0.367 (+/- 3.7%) | 2.177 (+/- 7.2%) | 3.488 (+/- 7.2%) | 0.105 |
| 1 MiB chunked upload | 2.253 (+/- 11.9%) | 0.002 (+/- 11.9%) | 21.362 (+/- 7.2%) | 0.020 (+/- 7.2%) | 0.105 |
| 128-request pipeline | 0.090 (+/- 1.5%) | 2.567 (+/- 1.5%) | 1.428 (+/- 5.8%) | 40.801 (+/- 5.8%) | 0.063 |

### Response serialization

| Workload | Iron GB/s | Iron Mmsg/s | C GB/s | C Mmsg/s | Iron/C |
|---|---:|---:|---:|---:|---:|
| 2-byte response | 0.451 (+/- 3.5%) | 4.338 (+/- 3.5%) | 1.781 (+/- 4.7%) | 17.129 (+/- 4.7%) | 0.253 |
| 4 KiB response | 13.114 (+/- 5.3%) | 3.122 (+/- 5.3%) | 43.459 (+/- 11.7%) | 10.345 (+/- 11.7%) | 0.302 |
| 1 MiB response | 10.306 (+/- 4.9%) | 0.010 (+/- 4.9%) | 29.215 (+/- 5.6%) | 0.028 (+/- 5.6%) | 0.353 |

### End to end

| Workload | Iron requests/s | pico requests/s | Iron/pico |
|---|---:|---:|---:|
| Minimal persistent server | 119318.236 (+/- 5.0%) | 128437.213 (+/- 15.2%) | 0.929 |

Bytes include HTTP framing. The parse results include model allocation and
release for Iron and chunk decoding for both implementations. The end-to-end
result excludes process startup and connection setup and includes parsing,
the two-byte handler, serialization and one sequential persistent-connection
round trip. See [README.md](README.md) for the complete methodology.
