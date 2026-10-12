# Byte loop microbenchmark

This benchmark exercises the scalar foundations from issue #658:

- a `while i < xs.len()` byte-list reduction;
- a range-based byte-list scan;
- a `String.byte_at()` scan;
- lookup through a global constant classification table;
- a JSON-like scalar tokenizer using string byte access and that table.

The program builds its input before timing and reports the best elapsed time
from each kernel's repeated runs. Every row includes a checksum so builds can
be compared without accepting dead-code elimination or a semantic difference.

On Linux, build the compiler in Release mode and run:

```sh
tests/benchmarks/byte_loops/run.sh build-release/ironc 32 11 2
```

The last argument is the CPU used by `taskset`. The script prints both the
throughput results and disassembly of the five timed functions. To compare two
compiler revisions, run the script once with each revision's `ironc` using the
same input size, repeat count, CPU, toolchain, and machine load.
