# Local CI on a shared machine

`scripts/ci/local_ci.sh` reproduces the five local CI legs:

- `mac-asan` and `mac-release` run on the invoking Mac.
- `linux-asan`, `linux-release`, and `linux-leaks` run in podman on
  `CI_HOST`.

## Commands

```sh
scripts/ci/local_ci.sh all       # Linux and Mac
scripts/ci/local_ci.sh linux     # start the three detached Linux legs and wait
scripts/ci/local_ci.sh mac       # run the two Mac legs
scripts/ci/local_ci.sh status    # inspect the recorded detached Linux run
scripts/ci/local_ci.sh wait      # reconnect, wait, copy results, and clean up
```

The Linux containers are detached before testing starts. A dropped SSH
connection therefore does not terminate a build or discard its output. The
run ID is stored in `.local-ci/current-run`; `status` and `wait` use that ID.
`wait` copies remote artifacts to `.local-ci/runs/<run-id>/` and removes only
the containers and network whose names contain that run ID. Ctrl-C also
requests cleanup for that run.

For a direct local reproduction with no SSH or podman, run a leg script:

```sh
CI_JOBS=2 IRON_TEST_TIMEOUT_SCALE=1 scripts/ci/run_linux_leg.sh release
CI_JOBS=2 IRON_TEST_TIMEOUT_SCALE=1 scripts/ci/run_linux_leg.sh asan
CI_JOBS=2 IRON_TEST_TIMEOUT_SCALE=1 scripts/ci/run_linux_leg.sh leaks
```

These are the same commands the remote containers execute.

## Capacity and timeouts

At startup, the Linux driver reads `/proc/loadavg` and `nproc` on the server.
It reserves one sixth of the machine, with a two-CPU floor on machines larger
than two CPUs, and shares the remaining idle capacity among the three legs.
Each leg is capped at eight workers by default. All three configure/build
phases use a shared run-scoped lock, so three compiler builds never start at
once; test phases may overlap within the computed total budget.

The same load reading selects `IRON_TEST_TIMEOUT_SCALE`. CMake applies this
positive integer to every CTest `TIMEOUT` through one central policy. The
default is 1. It changes only watchdog headroom, not scheduling or assertions.
The optimizer parity runner also applies it to its per-fixture runtime limit.

Useful overrides:

| Variable | Meaning |
| --- | --- |
| `CI_JOBS` | Worker count for every Linux leg |
| `CI_JOBS_ASAN` | Worker count for only `linux-asan` |
| `CI_JOBS_RELEASE` | Worker count for only `linux-release` |
| `CI_JOBS_LEAKS` | Worker count for only `linux-leaks` |
| `JOBS_LOCAL` | Worker count for the Mac legs |
| `CI_RETRY_JOBS` | CTest second-pass workers, default 2 |
| `IRON_TEST_TIMEOUT_SCALE` | Positive integer timeout multiplier, default 1 |
| `PARITY_JOBS` | Optimizer parity compiler budget |
| `PARITY_TIMEOUT` | Default parity runtime limit before scaling |

## Results and retries

The first CTest pass runs at the leg's normal worker count and writes
`<leg>-first.xml`. If it fails, CTest's own `LastTestsFailed.log` is copied to
`<leg>-retried.txt` and exactly those tests run again with `--rerun-failed` at
low parallelism. The retry writes `<leg>-retry.xml`.

A leg passes when each test passed in either pass. A deterministic failure
still fails twice and fails the leg. The console summary lists every test
that needed the second pass, even when the leg ultimately passes. The
per-leg `.result` files are key/value records suitable for scripts.

Each Linux leg has its own 3 GB MySQL server. Readiness requires a TCP query
that creates, writes, reads, and drops a table. A ping alone is not considered
ready. Keeping servers separate prevents one leg's statements from delaying
another leg.

## Long corpus tests

CTest exposes acceptance and leak coverage as eight shards apiece:
`test_v4_acceptance_1` through `_8` and `test_leak_check_1` through `_8`.
Each shard is internally serial, so CTest owns the parallelism and reports
which slice is running or failed. A contract test verifies that acceptance
shards form an exact, non-overlapping partition of the v4 corpus.

`test_opt_parity` uses `ironc` directly and runs the optimized and
non-optimized compilation for a fixture concurrently. The outer pair pool
never exceeds `PARITY_JOBS`. Its report ends with the ten slowest fixtures.
A fixture may request a longer runtime watchdog with an Iron line comment:

```iron
-- @parity-timeout: 120
```

The value is still multiplied by `IRON_TEST_TIMEOUT_SCALE`.

## Remote configuration

| Variable | Default |
| --- | --- |
| `CI_HOST` | `silvaserver.local` |
| `CI_REMOTE_DIR` | `/home/victor/code/iron-local-ci` |
| `CI_IMAGE` | `localhost/iron-dev:latest` |
| `CI_MEMORY` | `24g` per test leg |
| `CI_MYSQL_IMAGE` | `docker.io/library/mysql:8.4` |
| `CI_MYSQL_MEMORY` | `3g` per database server |
| `CI_LOG_DIR` | `.local-ci` in the checkout |
| `CI_POLL_SECONDS` | `15` |

If `wait` reports that SSH is unavailable, restore connectivity and run
`scripts/ci/local_ci.sh wait` again. Do not start another run while
`.local-ci/current-run` exists.
