# MySQL protocol benchmarks

This harness is the baseline for replacing the C wire codec with pure Iron. It drives the production codec through the same in-process transport as the conformance suite, so network and server scheduling do not hide codec costs.

```sh
bench/mysql/run.sh build --cpu 2 --warmup 5 --samples 25 --out mysql-results.txt
```

The script prints raw `RESULT` records followed by a Markdown table. Each fixture is generated once and held in memory. Every timed sample decodes and frees a complete result or encodes a 256-value prepared parameter set 200 times. The default is three untimed warmups and 15 samples. Reported time is the median; variability is median absolute deviation divided by the median. `--cpu` uses `taskset` on Linux to pin the process to one core.

Fixtures:

| Fixture | Workload |
|---|---|
| `narrow_integer` | 20,000 text-protocol rows with four integer columns |
| `wide_mixed` | 4,000 text rows with integers, decimal, double, temporal, BIT, JSON, ENUM, SET and text |
| `large_text_blob` | 64 rows containing a 64 KiB TEXT and 64 KiB BLOB |
| `binary_integer` | 20,000 binary-protocol rows with four 64-bit columns |
| `prepared_params` | NULL, signed integer, floating point and string parameters |

MariaDB Connector/C is an external LGPL reference and is not vendored. Its public API exposes result and statement codecs only behind a live server connection; it has no supported API that decodes recorded packets or encodes a parameter block from memory. The table therefore marks these memory-only cases N/A rather than comparing codec work with network work. Live client behavior is compared separately by `tests/mysql/differential.sh`.
