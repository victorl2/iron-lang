# C reference corpus

2000 small, self-contained, self-checking C programs. They cover nine topics
and give Iron a correctness baseline: each program's `.expected` file records
the exact stdout of a known-good C build. Later on, an Iron port of any
program must print the same bytes.

This directory holds only C. Iron ports come in a later step.

## Layout

```
tests/c_corpus/
  run.py                 build + run + diff harness
  MANIFEST.md            generated index of every program
  <topic>/<slug>.c       the program
  <topic>/<slug>.expected  exact expected stdout
```

Topics: `algorithms`, `data_structures`, `memory`, `concurrency`, `io_files`,
`networking`, `unix`, `statistics`, `other`.

## Running

```sh
python3 tests/c_corpus/run.py                  # everything
python3 tests/c_corpus/run.py networking       # one topic
python3 tests/c_corpus/run.py -k heap          # slug filter
python3 tests/c_corpus/run.py --sanitize       # ASan + UBSan build
python3 tests/c_corpus/run.py --lint           # headers and slugs only
python3 tests/c_corpus/run.py --manifest       # regenerate MANIFEST.md
```

Each program is built on its own with:

```
cc -std=gnu11 -O1 -Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter -pthread prog.c -lm
```

(`-lrt` is added on Linux.) `gnu11` only exposes POSIX and BSD declarations
on glibc; `-Wpedantic -Werror` rejects GNU language extensions, so the code
itself stays ISO C11.

A program passes when it exits 0, prints nothing to stderr, and its stdout
matches `<slug>.expected` byte for byte. It runs with no arguments, stdin
set to `/dev/null`, and the working directory and `TMPDIR` set to a fresh
scratch directory that is deleted afterwards.

## Program contract

Every program follows these rules. They keep the corpus deterministic across
macOS and Linux and make it a usable oracle for a second implementation.

* **One file, no inputs.** No argv, no stdin, no fixtures. Data is generated
  in the program, usually from a small seeded PRNG written inline.
* **Minimal dependencies.** The C standard library, POSIX, pthreads, and libm.
  Nothing else. The header lists what is used.
* **Self-checking.** Results are verified in-program (against a brute-force
  version, a known answer, or an invariant). On a failed check the program
  prints to stderr and exits nonzero, so a wrong port cannot pass just by
  printing the right text.
* **Deterministic stdout.** No addresses, pids, uids, timestamps, durations,
  hostnames, `strerror` text, thread scheduling order, or random ports in the
  output. Threads record results and the main thread prints after joining.
  Floating point output uses modest precision (`%.4f` / `%.6g`) on
  well-conditioned computations so libm differences do not show.
* **Portable POSIX.** Must behave the same on macOS and Linux. Not used:
  `pthread_barrier_*`, unnamed `sem_init`, `epoll`, `kqueue`, `eventfd`,
  `signalfd`, `timerfd`, `inotify`, `memfd_create`, `pipe2`, `accept4`,
  `sendfile`, `/proc`, `ucontext`, `qsort_r`, `strlcpy`, `arc4random`.
* **Local only.** Networking uses loopback (`127.0.0.1`, port 0, then
  `getsockname`) or `socketpair`. No DNS lookups, no Internet access.
* **Clean.** Every allocation freed, every fd closed, temp files removed.
  Runs in well under a second and under 64 MB, and passes `--sanitize`.

## Header

Each file starts with a comment the harness parses:

```c
/*
 * title: Dijkstra shortest paths with a binary heap
 * topic: algorithms
 * covers: priority queue, edge relaxation, adjacency lists
 * deps: libc
 */
```

`deps` is a comma-separated subset of `libc`, `libm`, `pthread`, `posix`,
`sockets`. Slugs are unique across the whole corpus.
