# Runtime Failure Contract

Phase 71 (HARDEN-01). Canonical classification of every thread-pool runtime
function's failure modes — which ones abort on failure, which ones return
typed errors, and how user code should handle each class. v1.4.0-alpha
Networking Standard Library cites this document when defining retry/backoff
behavior for HTTP/TLS/JSON/WebSocket stacks.

Before Phase 71, every runtime-alloc failure in `src/runtime/iron_threads.c`
routed through `iron_oom_abort` — including `pthread_create` returning
`EAGAIN` under scheduler pressure, which is recoverable operational pressure,
not genuine OOM. Phase 71 distinguishes four failure classes and routes each
to the appropriate caller-observable behavior via a parallel `*_or_error` API
that leaves the legacy abort-on-failure signatures byte-identical so
external callers can migrate at their own pace.

## Scope

This document covers the thread-pool creation and submission API in
`src/runtime/iron_threads.c`:

- `Iron_pool_create` / `Iron_pool_create_or_error`
- `Iron_elastic_pool_create` / `Iron_elastic_pool_create_or_error`
- `Iron_poolwait_create` / `Iron_poolwait_create_or_error`
- `Iron_handle_create` / `Iron_handle_create_or_error`
- `iron_handle_create_self_ref` / `iron_handle_create_self_ref_or_error`
- `Iron_channel_create` / `Iron_channel_create_or_error`
- `Iron_mutex_create` / `Iron_mutex_create_or_error`
- `Iron_pool_submit` / `Iron_pool_submit_or_error`
- `Iron_pool_submit_wait`
- `Iron_pool_destroy`

Out of scope (revisit in a future comprehensive runtime-error-channel
milestone per Phase 71 CONTEXT.md §deferred):

- `src/runtime/iron_string.c` — 9 `iron_oom_abort` guards for silent-
  truncation / empty-string / self-return fallbacks from Phase 67-06
- `src/runtime/iron_collections.c` — `IRON_LIST_IMPL` / `IRON_MAP_IMPL`
  `_push` / `_put` OOM paths (guarded by Phase 67-02 `test_alloc_*_oom.c`)
- `src/stdlib/iron_net.c` — NetError channel is already typed; migration of
  its legacy thread-pool callers to the new `*_or_error` variants is v1.4.0-
  alpha resumption work
- Windows thread-API path (`iron__win_thread_create`) — same conceptual
  treatment, separate milestone gated on Windows CI re-enablement

## 1. Failure Classification

Iron runtime failures fall into four classes:

| Class | Cause | Caller-observable | Example |
|-------|-------|-------------------|---------|
| **OOM** | `malloc` / `calloc` / `realloc` returned NULL; address space or kernel reserve genuinely exhausted | `iron_oom_abort` — hard abort with `"iron: out of memory at <where>"` on stderr, followed by `abort(3)` | Every `_or_error` body's malloc-NULL branch; every legacy creation function via its delegate wrapper |
| **Scheduler pressure** | `pthread_create` returned `EAGAIN` — process thread limit reached (`RLIMIT_NPROC`, container cgroup, elevated `ulimit`) | Typed error `IRON_ERR_THREAD_LIMIT` via `_or_error` variant; legacy wrapper still aborts | `Iron_pool_create_or_error` `IRON_THREAD_CREATE` failure; `Iron_handle_create_or_error` spawn failure; `Iron_pool_submit_or_error` when elastic spawn fails |
| **Kernel resource exhaustion** | File descriptor limit, mutex limit, semaphore limit, condition variable limit reached (not exercised in HARDEN-01 but reserved for future use) | Typed error `IRON_ERR_RESOURCE_EXHAUSTED` | Reserved for future expansion when kernel-level pressure gets explicit handling |
| **Degraded mode** | Recoverable internal failure during a path where aborting would be worse than proceeding at reduced capacity | Best-effort + diagnostic log; caller may observe side effects (leaked workers, rejected submit) | `Iron_pool_destroy` snapshot malloc NULL (logs `"iron: pool_destroy snapshot alloc failed — leaking N live workers (degraded-mode shutdown)"` to stderr, accumulates into `pool->leaked_count`, skips the join phase, continues teardown); `pool_queue_grow` propagates `IRON_ERR_POOL_FULL` to the submit caller |

The load-bearing fix of HARDEN-01 is the four **scheduler pressure** sites.
Phase 67-06's walkthrough misclassified `pthread_create` returning `EAGAIN`
as OOM, causing long-running services to hard-abort when they hit
`RLIMIT_NPROC` — a normal operational condition on networked workloads.
Phase 71 routes all four sites through typed errors so a service can retry
with exponential backoff or gracefully shed load instead of crashing.

## 2. Per-Function Contract

Every public creation / submission function has TWO entry points: a legacy
signature that aborts on any failure (preserving Phase 67-06 abort-on-
failure semantics for codegen-emitted user code and for callers that cannot
handle typed errors) and an `*_or_error` variant that returns the error to
the caller for retry / backoff / graceful degradation.

The legacy signatures and the `_or_error` variants share implementations:
each legacy function is a 3-line delegate that calls its `_or_error`
counterpart and routes any non-zero `Iron_Error` through `iron_oom_abort`
with a distinct location literal so the abort message still points at the
caller that refused to handle the typed error.

### Creation functions

| Legacy | `*_or_error` variant | Failure classes | Typed error returned |
|--------|----------------------|------------------|----------------------|
| `Iron_pool_create(name, thread_count)` aborts on any failure | `Iron_pool_create_or_error(name, thread_count) → Iron_Pool_OrError` | OOM (malloc `Iron_Pool` / queue / threads), scheduler pressure (`IRON_THREAD_CREATE` `EAGAIN`) | `IRON_ERR_THREAD_LIMIT` on `pthread_create` `EAGAIN` with partial teardown (already-spawned workers signalled via `shutdown=true` + `IRON_COND_BROADCAST(work_ready)` under `pool->lock`, then joined; all allocations freed; mutex + condvars destroyed) |
| `Iron_elastic_pool_create(name, max_threads, idle_timeout_ms)` aborts on any failure | `Iron_elastic_pool_create_or_error(...) → Iron_ElasticPool_OrError` | OOM only (no threads spawned eagerly — workers spawn lazily on first submit) | N/A in current implementation — only OOM, which stays as `iron_oom_abort` inside the `_or_error` body |
| `Iron_poolwait_create()` aborts on any failure | `Iron_poolwait_create_or_error() → Iron_PoolWait_OrError` | OOM only | N/A — only OOM |
| `Iron_handle_create(fn, arg)` aborts on any failure | `Iron_handle_create_or_error(fn, arg) → Iron_Handle_OrError` | OOM (`Iron_Handle` + `HandleWrapper`), scheduler pressure (`IRON_THREAD_CREATE` `EAGAIN`) | `IRON_ERR_THREAD_LIMIT` with clean-up: wrapper freed, handle's mutex + cond destroyed, handle freed |
| `iron_handle_create_self_ref(fn)` aborts on any failure | `iron_handle_create_self_ref_or_error(fn) → Iron_Handle_OrError` | Same as `Iron_handle_create_or_error`; `wrapper->arg = h` establishes the self-reference so the worker can observe its own handle | `IRON_ERR_THREAD_LIMIT` with the same clean-up ordering |
| `Iron_channel_create(capacity)` aborts on any failure | `Iron_channel_create_or_error(capacity) → Iron_Channel_OrError` | OOM only (channel struct + ring array) | N/A — only OOM |
| `Iron_mutex_create(initial_value, size)` aborts on any failure | `Iron_mutex_create_or_error(initial_value, size) → Iron_Mutex_OrError` | OOM only (`Iron_Mutex` struct + value copy) | N/A — only OOM |

### Submission functions

| Legacy | `*_or_error` variant | Failure classes | Typed error returned |
|--------|----------------------|------------------|----------------------|
| `Iron_pool_submit(pool, fn, arg)` aborts on any failure | `Iron_pool_submit_or_error(pool, fn, arg) → Iron_Error` | OOM (`pool_queue_grow` malloc NULL on ring-buffer doubling), scheduler pressure (elastic-spawn `EAGAIN`) | `IRON_ERR_POOL_FULL` on grow failure (degraded-mode propagation — the pool stays operational at its current capacity; caller decides retry or backpressure); `IRON_ERR_THREAD_LIMIT` on elastic-spawn failure with enqueue back-out under `pool->lock` so no parked worker observes the phantom item |
| `Iron_pool_submit_wait(pool, fn, arg, wait)` aborts on any failure | (no public `_or_error` variant in Phase 71; future milestone) | Same as `Iron_pool_submit` | Legacy only — delegates to the same shared `iron_pool_submit_impl` private helper and aborts on any typed error |

Both `Iron_pool_submit` and `Iron_pool_submit_wait` were reduced to delegate
wrappers in Phase 71 Plan 04 so they share a single submit-path
implementation with `Iron_pool_submit_or_error`. Adding a public
`Iron_pool_submit_wait_or_error` in a future milestone is a 3-line delegate
over the existing private helper.

### Destruction / shutdown functions

| Function | Failure behavior |
|----------|------------------|
| `Iron_pool_destroy(pool)` | On elastic pools, snapshots the live thread-slot array before joining. If the snapshot `malloc` fails, enters degraded mode: logs `"iron: pool_destroy snapshot alloc failed — leaking %d live workers (degraded-mode shutdown)"` to stderr, accumulates still-live workers into `pool->leaked_count`, skips the join loop, continues with pool struct teardown. Rationale: aborting during a server shutdown is the worst moment to abort; the still-running workers will hit their own idle-timeout retirement path or the OS will reclaim them at process exit. |
| `Iron_pool_barrier(pool)` | Never fails — just blocks on `pool->pending` reaching zero |
| `Iron_channel_close(ch)` / `Iron_channel_destroy(ch)` | Never fail |
| `Iron_mutex_destroy(m)` | Never fails |

## 3. Caller Decision Guide

Which variant should your code use?

| Caller profile | Recommended API | Rationale |
|----------------|------------------|-----------|
| **One-shot CLI tool** (compiler, test harness, fuzz target) | Legacy (`Iron_pool_create`, etc.) | Abort-on-failure is correct — a CLI tool that cannot allocate a 2-thread pool cannot usefully continue, and the hard abort gives a bisectable stderr message |
| **Long-running service / daemon** | `*_or_error` variants | Scheduler pressure is recoverable; a service that retries with backoff after `IRON_ERR_THREAD_LIMIT` is strictly better than one that crashes |
| **Networked server** (HTTP / TLS / WebSocket) | `*_or_error` variants — MANDATORY | Under sustained connection load, `RLIMIT_NPROC` pressure is a normal operational condition, not an error. Any networked code path that cannot handle `IRON_ERR_THREAD_LIMIT` is a crash-loop waiting to happen |
| **Codegen-emitted user code** (`src/lir/emit_c.c`) | Legacy | User code does not see the failure path directly; the runtime's abort-on-failure is the terminal error handler for unrecoverable resource exhaustion |
| **Internal runtime helpers** (inside `iron_threads.c`) | `*_or_error` where available | Internal call sites are migrated first so the runtime itself handles its own pressure before user code does |

## 4. Error-Code Reference

New codes introduced in Phase 71 (Plan 01), defined in
`src/runtime/iron_errors.h` in the 7000-7099 runtime-pressure band (opened
by narrowing the internal-invariant band from 7000..7999 to 7100..7999):

| Code | Value | Class | One-line description |
|------|-------|-------|----------------------|
| `IRON_ERR_THREAD_LIMIT` | 7000 | Scheduler pressure | `pthread_create` / equivalent returned `EAGAIN`. Process thread budget exhausted. Recoverable — retry with backoff. |
| `IRON_ERR_POOL_FULL` | 7001 | Degraded mode | Pool work queue could not grow — either OOM on the grow malloc or an explicit capacity bound hit. Pool remains operational at current capacity. |
| `IRON_ERR_RESOURCE_EXHAUSTED` | 7002 | Kernel exhaustion | File descriptor / mutex / semaphore / condition-variable limit reached. (Reserved; not currently returned by any Phase 71 path.) |

Pre-existing NET codes (from Phase 59) that caller code commonly sees
alongside the new HARDEN-01 codes: see `src/runtime/iron_errors.h` lines
24-48 for the `IRON_ERR_NET_*` range (1000-1023).

The canonical error-code partitioning is documented in the header comment
at the top of `src/runtime/iron_errors.h`: 1xxx net, 2xxx url, 3xxx tls,
4xxx json, 5xxx http, 6xxx ws, 7000-7099 runtime pressure, 7100-7999
internal invariants.

## 5. Retry Guidance

For each recoverable error class, the runtime does NOT retry on behalf of
the caller — per HARDEN-01 §Non-Goals, automatic retry with backoff is
explicitly out of scope. The caller owns the retry policy. Recommended
shapes:

| Error class | Retry? | Pattern |
|-------------|--------|---------|
| `IRON_ERR_THREAD_LIMIT` (scheduler pressure) | Yes, with backoff | Exponential backoff starting at 10 ms, doubling up to a cap of 10 s. Typical transient — scheduler pressure usually clears within 1-2 retries when other processes release threads. Give up after ~10 attempts and fall back to a smaller pool size or reject the request. |
| `IRON_ERR_POOL_FULL` (queue grow failure) | No — caller should reject | Backpressure: return the failure to your own caller and let them throttle. Retrying immediately will hit the same OOM since the pool's queue is bounded by process memory. For a networked server, respond with `503 Service Unavailable`. |
| `IRON_ERR_RESOURCE_EXHAUSTED` | Context-dependent | If caused by fd exhaustion on a server, close idle connections before retry. If caused by mutex exhaustion, the process is in a pathological state and abort-and-restart may be cleanest. Reserved for future use — no Phase 71 path currently returns this code. |
| `iron_oom_abort` (hard abort) | No — process is already dead | The only recovery is process-level restart. This is what Phase 67-06 intended: OOM is unrecoverable, abort is correct. The `_or_error` variants preserve this posture for malloc-NULL paths. |

The load-bearing distinction is `IRON_ERR_THREAD_LIMIT` vs OOM: the former
is a normal operational condition that retry+backoff handles correctly,
while the latter is a pathological state where retry accomplishes nothing.
Phase 67-06's mistake was collapsing both into `iron_oom_abort`; Phase 71
restores the distinction.

## 6. Migration Examples

Before (Phase 67-06 — every failure aborts):

```c
Iron_Pool *pool = Iron_pool_create("workers", 16);
/* Iron_pool_create aborts on any failure, so `pool` is guaranteed non-NULL
 * here. No check needed — but if the system is under RLIMIT_NPROC pressure,
 * the process has already died and the caller never gets here. */
for (int i = 0; i < work_count; i++) {
    Iron_pool_submit(pool, do_work, items[i]);
}
Iron_pool_barrier(pool);
Iron_pool_destroy(pool);
```

After (Phase 71 — scheduler pressure is recoverable):

```c
Iron_Pool *pool = NULL;
Iron_Pool_OrError r;
int attempt = 0;
int backoff_ms = 10;

for (;;) {
    r = Iron_pool_create_or_error("workers", 16);
    if (iron_error_is_ok(r.err)) {
        pool = r.pool;
        break;
    }
    if (r.err.code != IRON_ERR_THREAD_LIMIT) {
        /* Non-recoverable — the malloc-NULL paths inside
         * Iron_pool_create_or_error already called iron_oom_abort, so we
         * cannot reach here on OOM. Any other non-zero code is a contract
         * violation — fall through to abort. */
        fprintf(stderr, "unexpected pool-create error: code=%d\n", r.err.code);
        abort();
    }
    if (attempt++ >= 10) {
        /* Give up after 10 retries — scheduler pressure is persistent.
         * Fall back to a smaller pool size. */
        fprintf(stderr, "scheduler pressure persists after 10 retries; "
                         "reducing concurrency\n");
        r = Iron_pool_create_or_error("workers", 2);
        if (!iron_error_is_ok(r.err)) return -1;
        pool = r.pool;
        break;
    }
    usleep(backoff_ms * 1000);
    if (backoff_ms < 10000) backoff_ms *= 2;
}

/* Submit with error checking — IRON_ERR_POOL_FULL is backpressure,
 * don't retry; IRON_ERR_THREAD_LIMIT is the elastic-spawn pressure path
 * and can use the same retry-with-backoff shape as pool creation. */
for (int i = 0; i < work_count; i++) {
    Iron_Error e = Iron_pool_submit_or_error(pool, do_work, items[i]);
    if (!iron_error_is_ok(e)) {
        if (e.code == IRON_ERR_POOL_FULL) {
            /* Queue is saturated. Drop or queue on a higher-level buffer. */
            reject_item(items[i]);
        } else if (e.code == IRON_ERR_THREAD_LIMIT) {
            /* Elastic spawn failure — same retry-with-backoff shape as
             * pool creation; caller policy decides retry count. */
            retry_submit_with_backoff(pool, do_work, items[i]);
        } else {
            /* Contract violation */
            abort();
        }
    }
}
Iron_pool_barrier(pool);
Iron_pool_destroy(pool);
```

The retry loop is the caller's responsibility — the runtime returns the
typed error and lets the caller decide whether backoff, fallback, or reject
is the right response for their workload profile.

## 7. Cross-reference: v1.4.0-alpha Networking Standard Library

The paused v1.4.0-alpha milestone (see `.planning/REQUIREMENTS-v0.2.0.md`)
covers HTTP/TLS/JSON/WebSocket and will build on the Phase 59 NET-01..13
foundation. When that milestone resumes, it MUST consume the `_or_error`
variants from this document rather than the legacy abort-on-failure forms.
Specifically:

- **HTTP server / client**: every connection-accepting path spawns worker
  handles via `Iron_handle_create_or_error` and must handle
  `IRON_ERR_THREAD_LIMIT` via exponential backoff (recommended: 10 ms →
  10 s cap). A connection that cannot be accepted due to scheduler pressure
  returns `503 Service Unavailable` with a `Retry-After` header.
- **TLS handshake worker**: the TLS phase can use `Iron_pool_submit_or_error`
  against a dedicated TLS pool. `IRON_ERR_POOL_FULL` means the TLS pool is
  saturated and the connection should be rejected cleanly (close socket
  without TLS alert, since the handshake never started).
- **WebSocket long-poll worker**: same as HTTP — scheduler pressure is a
  normal operational condition, handle via backoff and surface `503` or
  `Retry-After` to the client.
- **JSON parser**: pure CPU work, not thread-pool bound; no direct
  dependency on this document.

When v1.4.0-alpha resumes, the first planning task is to migrate the
current legacy `Iron_poolwait_create()` caller inside the DNS lookup path
in `src/stdlib/iron_net.c` to `Iron_poolwait_create_or_error()`, then
re-audit every other legacy thread-pool call in `iron_net.c` /
`iron_net_init.c`. HARDEN-01 explicitly deferred those downstream caller
migrations to v1.4.0-alpha resumption so this phase could land as a
focused runtime-internals change.

---

*Phase 71 (HARDEN-01, Plans 01-06). Generated from the post-Plan-05 state of
`src/runtime/iron_threads.c`, `src/runtime/iron_runtime.h`, and
`src/runtime/iron_errors.h`.*
