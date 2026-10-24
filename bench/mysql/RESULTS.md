# MySQL protocol baseline

Current C codec baseline, recorded 2026-10-12 on Linux x86-64 (13th Gen Intel Core i7-13700K, 24 logical CPUs), pinned to CPU 0. The CMake Release build used Ubuntu clang 18.1.3. Each cell used five warmups and 25 timed samples; MAD is the median absolute deviation as a percentage of the median.

| Operation | Fixture | MB/s | rows or params/s | MAD | MariaDB Connector/C |
|---|---|---:|---:|---:|---|
| decode | narrow_integer | 619.6 | 22,570,179 | 2.60% | N/A (no memory codec API) |
| decode | wide_mixed | 1094.8 | 5,210,403 | 1.67% | N/A (no memory codec API) |
| decode | large_text_blob | 5485.1 | 41,844 | 2.46% | N/A (no memory codec API) |
| decode | binary_integer | 1640.5 | 43,161,214 | 6.14% | N/A (no memory codec API) |
| encode | prepared_params | 1786.1 | 138,681,604 | 3.55% | N/A (no memory codec API) |

Raw records:

```text
# 2026-10-12T13:06:38Z Linux 5.14.0-570.58.1.el9_6.x86_64 x86_64 cpu=0 warmup=5 samples=25
RESULT decode narrow_integer 549054 20000 886125 22995
RESULT decode wide_mixed 840464 4000 767695 12816
RESULT decode large_text_blob 8389474 64 1529492 37635
RESULT decode binary_integer 760164 20000 463379 28445
RESULT encode prepared_params 659400 51200 369191 13110
```

Connector/C is intentionally not assigned a synthetic number. MariaDB Connector/C 3.x does not expose its packet or statement codecs as a supported memory API, so driving it requires a server and measures networking, scheduling and server execution in addition to codec work. See [README.md](README.md) for the fixture and timing details; the optional live differential test still uses the command line client as an independent behavior oracle.
