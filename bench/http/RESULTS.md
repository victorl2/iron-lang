# HTTP benchmark baseline

Measured 2026-10-13 at commit `a02d5a27e10b4d01a8225f96be52594b8d0a4f42`.

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
| Small GET | 0.087 (+/- 1.7%) | 2.496 (+/- 1.7%) | 1.229 (+/- 4.3%) | 35.128 (+/- 4.3%) | 0.071 |
| Browser-like request | 0.197 (+/- 3.0%) | 0.315 (+/- 3.0%) | 1.907 (+/- 4.4%) | 3.057 (+/- 4.4%) | 0.103 |
| 1 MiB chunked upload | 1.977 (+/- 8.7%) | 0.002 (+/- 8.7%) | 20.558 (+/- 4.0%) | 0.020 (+/- 4.0%) | 0.096 |
| 128-request pipeline | 0.074 (+/- 1.4%) | 2.117 (+/- 1.4%) | 1.260 (+/- 10.6%) | 36.007 (+/- 10.6%) | 0.059 |

### Response serialization

| Workload | Iron GB/s | Iron Mmsg/s | C GB/s | C Mmsg/s | Iron/C |
|---|---:|---:|---:|---:|---:|
| 2-byte response | 0.396 (+/- 1.7%) | 3.810 (+/- 1.7%) | 1.680 (+/- 4.7%) | 16.153 (+/- 4.7%) | 0.236 |
| 4 KiB response | 12.259 (+/- 4.5%) | 2.918 (+/- 4.5%) | 42.192 (+/- 5.1%) | 10.043 (+/- 5.1%) | 0.291 |
| 1 MiB response | 10.190 (+/- 15.9%) | 0.010 (+/- 15.9%) | 31.255 (+/- 1.5%) | 0.030 (+/- 1.5%) | 0.326 |

### End to end

| Workload | Iron requests/s | pico requests/s | Iron/pico |
|---|---:|---:|---:|
| Minimal persistent server | 127445.606 (+/- 5.2%) | 144366.837 (+/- 3.4%) | 0.883 |

Bytes include HTTP framing. The parse results include model allocation and
release for Iron and chunk decoding for both implementations. The end-to-end
result excludes process startup and connection setup and includes parsing,
the two-byte handler, serialization and one sequential persistent-connection
round trip. See [README.md](README.md) for the complete methodology.
