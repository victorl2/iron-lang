# HTTP benchmarks

This harness records the C HTTP/1.1 baseline that the planned pure Iron
implementation must match or beat. It uses the production Iron parser and
serializer through an in-memory transport for parse-only measurements, the
vendored picohttpparser reference, and a loopback load generator for complete
request and response exchanges.

Run a Release build:

```sh
cmake -S . -B build-http-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang
cmake --build build-http-release -j4 --target http_benchmark
bench/http/run.sh build-http-release --samples 15 --warmup 3 --out bench-http-raw.txt
```

The script prints markdown tables. `--quick` uses small iteration counts for
a smoke run. `--cpu N --server-cpu M` overrides automatic affinity selection.

## Workloads

| Workload | Contents |
|---|---|
| Small GET | 35-byte origin-form request with Host |
| Browser-like request | request line plus twelve realistic browser and application headers |
| 1 MiB chunked upload | sixteen 64 KiB chunks, extensions, zero chunk and a trailer |
| 128-request pipeline | one buffer containing 128 complete requests |
| Response serialization | 2-byte, 4 KiB and 1 MiB response bodies |
| Minimal persistent server | one persistent loopback connection and a two-byte text handler |

## Method

- Each parse-only sample starts after deterministic input generation. Iron
  runs the exact production parser and body decoder over an in-memory
  transport, including its model allocations and releases. pico parses the
  same head and uses its chunk decoder for the upload. Bytes include HTTP
  framing. Message rate counts complete messages.
- Serialization measures complete wire responses. Iron runs the production
  serializer. The C reference uses `snprintf` plus `memcpy`, which is the work
  a minimal pico-based server needs because pico is parser-only.
- End-to-end samples use an included load generator over TCP loopback. The
  load and server processes are pinned to separate allowed cores when at
  least two are available. The connection is persistent and requests are
  sequential, so the number measures parser, handler, serializer and one
  request-response turn without connection setup.
- The default discards three full warmup runs and records fifteen independent
  timed runs. Tables report the median and median absolute deviation as a
  percentage of the median. Timers use `CLOCK_MONOTONIC`; input construction,
  process startup and TCP connection setup are outside timed regions.
- The harness is intended to run on an otherwise idle machine with frequency
  scaling controlled by the operator. Raw result headers record date, kernel,
  compiler and CPU affinity. Compare builds on the same machine and settings.

The reference source is pinned under
`tests/http/vendor/picohttpparser` with its MIT license and source commit.
Current Linux x86 results are in [RESULTS.md](RESULTS.md).
