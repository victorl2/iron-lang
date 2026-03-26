---
phase: 03-runtime-stdlib-and-cli
plan: 02
subsystem: runtime
tags: [threading, thread-pool, channel, mutex, pthreads, concurrent, iron_pool, iron_channel, iron_mutex]

requires:
  - phase: 03-runtime-stdlib-and-cli
    plan: 01
    provides: "iron_runtime.h base header, iron_runtime_init/shutdown lifecycle, iron_string.c with iron_threads_init forward call"

provides:
  - "Iron_Pool fixed-size thread pool with FIFO circular buffer work queue and barrier synchronization"
  - "Iron_Handle future for spawn/await pattern with panic propagation"
  - "Iron_Channel bounded ring buffer with blocking send/recv and non-blocking try_recv"
  - "Iron_Mutex value-wrapping mutex exposing typed pointer via lock/unlock"
  - "Iron_Lock/Iron_CondVar thin pthread wrapper primitives"
  - "Iron_global_pool initialized with (cpu_count - 1) worker threads via iron_threads_init()"
  - "13 Unity tests covering all threading primitives"

affects:
  - "03-03 through 03-08: Concurrent code paths use Iron_global_pool"
  - "codegen: Iron_pool_submit/Iron_pool_barrier/Iron_handle_wait already emitted by gen_stmts.c and gen_exprs.c"

tech-stack:
  added: ["pthreads (pthread_create, pthread_mutex_t, pthread_cond_t)"]
  patterns:
    - "Pool worker: lock-then-wait loop; dequeue-unlock-execute-lock-decrement-signal pattern"
    - "Channel: dual condvar (not_full, not_empty) with closed flag for graceful shutdown"
    - "Handle: wrapper struct forwards to fn(arg), sets done=true, signals cond on completion"
    - "iron_threads_init/shutdown called from iron_runtime_init/shutdown via forward declaration"
    - "Pool queue growth: copy circular buffer to linear buffer then double capacity"
    - "sysconf(_SC_NPROCESSORS_ONLN) for cpu count with fallback to 1"

key-files:
  created:
    - src/runtime/iron_threads.c
    - tests/test_runtime_threads.c
  modified:
    - src/runtime/iron_runtime.h
    - src/runtime/iron_string.c
    - CMakeLists.txt

key-decisions:
  - "Pool queue is a circular buffer (head/tail/count) that grows by doubling when full; no work-stealing"
  - "Iron_Handle uses a wrapper struct (HandleWrapper) allocated on heap so the thread can access both handle and fn pointers after pthread_create returns"
  - "Channel capacity < 1 clamped to 1 (unbuffered semantics implemented as capacity-1 ring buffer)"
  - "iron_threads_init declared as void (extern) forward declaration in iron_string.c — not static — since it lives in a different translation unit"
  - "Test setUp/tearDown calls iron_runtime_init/shutdown which initializes/destroys the global pool each test"

requirements-completed: [RT-04, RT-05, RT-06]

duration: 10min
completed: 2026-03-26
---

# Phase 03 Plan 02: Threading Primitives Summary

**pthread-based Iron_Pool (FIFO thread pool), Iron_Channel (ring buffer), Iron_Mutex (value wrapper), and Iron_Handle (spawn/await future) — codegen stubs now resolve to real implementations**

## Performance

- **Duration:** ~10 min (verification and test fixes; implementation was pre-committed in Plan 01 session)
- **Completed:** 2026-03-26
- **Tasks:** 2
- **Files modified:** 5

## Accomplishments

- Iron_Pool with circular-buffer work queue, worker thread pool, Iron_pool_barrier blocking until pending == 0
- Iron_Handle future pattern: wrapper thread sets done=true + signals condvar; Iron_handle_wait joins and re-raises panic
- Iron_Channel bounded ring buffer: blocking send/recv, non-blocking try_recv, close/destroy, dual condvar design
- Iron_Mutex value-wrapping mutex: Iron_mutex_lock returns typed void* pointer; Iron_mutex_unlock releases
- Iron_global_pool initialized via iron_threads_init() called from iron_runtime_init()
- 13 Unity tests all passing: pool create/destroy, submit single/many, barrier, thread count, channel send/recv/try_recv, mutex concurrent, handle wait, global pool exists

## Task Commits

1. **Task 1: Iron_Pool, Iron_Channel, Iron_Mutex, Iron_Handle implementation** - `bcf91ab` (feat — committed as part of Plan 03-01)
2. **Task 2: Threading tests and CMakeLists update** - `5a4e1c7` (feat — committed as part of Plan 03-01)

Note: Both tasks were committed in the same Plan 03-01 session. Plan 03-02 execution verified the implementation and confirmed all 16 tests pass.

## Files Created/Modified

- `src/runtime/iron_threads.c` - Pool worker loop, submit/barrier/destroy, Handle wrapper thread, Channel ring buffer, Mutex
- `tests/test_runtime_threads.c` - 13 Unity tests for all threading primitives including concurrent mutex stress test
- `src/runtime/iron_runtime.h` - Threading type declarations: Iron_Pool, Iron_Handle, Iron_Channel, Iron_Mutex, Lock/CondVar, Iron_global_pool
- `src/runtime/iron_string.c` - Forward declarations for iron_threads_init/shutdown; calls in iron_runtime_init/shutdown
- `CMakeLists.txt` - iron_threads.c added to iron_runtime; test_runtime_threads executable added

## Decisions Made

- Pool circular buffer grows by doubling when full: copy head-to-tail items to linear buffer, reset head=0/tail=count
- Iron_Handle uses a HeapWrapper struct (`HandleWrapper`) so the spawned thread can access both the handle pointer and the function pointer even after the caller's stack frame is gone
- Channel unbuffered semantics use capacity=1 (ring buffer with one slot); send blocks until slot is empty
- Global pool uses `sysconf(_SC_NPROCESSORS_ONLN) - 1` threads with minimum of 1
- Pool barrier uses while-loop over `pending > 0` with cond_wait to avoid spurious wakeup issues

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] test_global_pool_exists failed — iron_runtime_init() didn't call iron_threads_init()**
- **Found during:** Task 2 verification (test_runtime_threads)
- **Issue:** iron_runtime_init() in iron_string.c had no call to iron_threads_init(), so Iron_global_pool remained NULL
- **Fix:** Added forward declarations `void iron_threads_init(void); void iron_threads_shutdown(void);` and calls in iron_runtime_init/shutdown
- **Files modified:** `src/runtime/iron_string.c`
- **Committed in:** 5a4e1c7 (Task 2 commit)

---

**Total deviations:** 1 auto-fixed (Rule 1 bug: missing function call in lifecycle)
**Impact on plan:** Fix was essential for global pool initialization. No scope creep.

## Issues Encountered

None beyond the auto-fixed global pool initialization issue.

## Self-Check

### Files Verified

- `src/runtime/iron_threads.c` — FOUND
- `tests/test_runtime_threads.c` — FOUND
- `src/runtime/iron_runtime.h` contains `Iron_global_pool` — VERIFIED
- `src/runtime/iron_runtime.h` contains `Iron_Pool` — VERIFIED
- `src/runtime/iron_runtime.h` contains `Iron_Channel` — VERIFIED
- `src/runtime/iron_runtime.h` contains `Iron_Mutex` — VERIFIED

### Commits Verified

- `bcf91ab` — FOUND (feat: implement Iron_String SSO+interning, Iron_Rc, builtins, and thread pool)
- `5a4e1c7` — FOUND (feat: update codegen to emit Iron_println/Iron_print builtins)

### Test Results

All 16 tests pass including test_runtime_threads (13/13 pass).

## Self-Check: PASSED

## Next Phase Readiness

- All codegen stubs for Iron_pool_submit, Iron_pool_barrier, Iron_handle_wait are now backed by real implementations
- Iron_global_pool available immediately after iron_runtime_init()
- Plan 03-03 (collections) can use the runtime without threading concerns
- Plan 03-04 (codegen integration) will add #include "runtime/iron_runtime.h" to generated C

---
*Phase: 03-runtime-stdlib-and-cli*
*Completed: 2026-03-26*
