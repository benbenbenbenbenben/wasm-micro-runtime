# Async Component Model Canonical ABI Implementation Plan

## Overview

The Component Model async proposal defines a complete asynchronous execution model with tasks, streams, futures, backpressure, callbacks, and thread spawning. This document provides a phased plan for implementing the full async Canonical ABI in the wasm-micro-runtime fork.

The async proposal touches every layer of the runtime:
- **Binary format**: 30+ canon types already parsed (0x05-0x42)
- **Validation**: Currently rejects all async canon types and opts
- **Instantiation**: Creates `UNSUPPORTED_CANON` placeholders
- **Execution**: No async execution engine exists

---

## Phase 1: Canon Opt Acceptance (3 files, ~50 lines) ✅ COMPLETE

### Goal
Accept `async` (0x06) and `callback` (0x07) canon options through validation so lift/lower can be marked async.

### Changes (committed)

**`wasm_component_validate.c`** — `validate_canon_opts` ✅
- Replaced ASYNC/CALLBACK rejection with acceptance: duplicate check for async, func_idx bounds check for callback

**`wasm_component_runtime.h`** ✅
- Added `bool is_async` and `uint32 callback_func_idx` to `WASMComponentRuntimeFunc`

**`wasm_component_runtime.c`** ✅
- `resolve_component_canon_lift_abi`: handle ASYNC (set is_async) and CALLBACK (resolve func_idx) canon opts
- `validate_lowered_import_signature`: accept ASYNC/CALLBACK opts in lower validation

**Test** ✅
- `TestRuntimeSupportsAsyncCanonOpt`: verifies async opt acceptance and is_async flag

### Remaining for full async support
See Phases 2-8 below.

---

## Phase 2: Async Canon Type Acceptance (2 files, ~50 lines)

### Goal
Accept the core async canon types (`task.return`, `task.cancel`, `backpressure.set`, `context.get`, `context.set`, `yield`) through validation and instantiation.

### Changes

**`wasm_component_validate.c`** — `validate_canons_section` (~30 lines)
- Add explicit `case` entries for `WASM_COMP_CANON_BACKPRESSURE_SET` (0x08)
- Add explicit `case` entries for `WASM_COMP_CANON_TASK_RETURN` (0x09)
- Add explicit `case` entries for `WASM_COMP_CANON_TASK_CANCEL` (0x05)
- Add explicit `case` entries for `WASM_COMP_CANON_CONTEXT_GET` (0x0a)
- Add explicit `case` entries for `WASM_COMP_CANON_CONTEXT_SET` (0x0b)
- Add explicit `case` entries for `WASM_COMP_CANON_YIELD` (0x0c)
- Add explicit `case` entries for `WASM_COMP_CANON_SUBTASK_CANCEL` (0x06)
- Each validates bounds where applicable (func indices, context indices)
- Fall through to: mark as accepted (increment core_func_count or func_count as appropriate)

**`wasm_component_runtime.c`** — `append_component_canon_function` (~20 lines)
- Add explicit `else if` branches for each async canon type before the fallthrough `else` block
- For `WASM_COMP_CANON_TASK_RETURN`: create a `WASM_COMP_RUNTIME_FUNC_ASYNC` function with the declared result type
- For other types: create `WASM_COMP_RUNTIME_FUNC_ASYNC` placeholder functions
- All async functions register in `lowered_funcs` (like resource builtins)

### Verification
- Components containing these canon types instantiate without error
- Functions are registered with the correct kind

---

## Phase 3: Core Async Execution Engine (4 files, ~2000 lines)

### Goal
Implement the async task runtime: task lifecycle, callback scheduling, and the execution environment.

### New Structures

**`wasm_component_async.h`** — new file (~200 lines)
```c
// Task state
typedef enum WASMComponentAsyncTaskState {
    WASM_COMP_ASYNC_TASK_PENDING,
    WASM_COMP_ASYNC_TASK_RUNNING,
    WASM_COMP_ASYNC_TASK_COMPLETED,
    WASM_COMP_ASYNC_TASK_CANCELLED,
    WASM_COMP_ASYNC_TASK_FAILED
} WASMComponentAsyncTaskState;

// Task control block
typedef struct WASMComponentAsyncTask {
    uint32 task_id;
    WASMComponentAsyncTaskState state;
    WASMComponentRuntimeFunc *function;
    WASMComponentRuntimeFunc *callback;
    wasm_component_value_t *args;
    uint32 num_args;
    wasm_component_value_t *results;
    uint32 num_results;
    WASMModuleInstanceCommon *caller_inst;
    bool owns_args;
    bool owns_results;
    uint32 context_idx;
    uint64 context_value;
} WASMComponentAsyncTask;

// Async execution engine
typedef struct WASMComponentAsyncEngine {
    WASMComponentAsyncTask *tasks;
    uint32 task_capacity;
    uint32 task_count;
    uint32 next_task_id;
    uint32 *pending_queue;    // FIFO queue of pending task IDs
    uint32 queue_head;
    uint32 queue_tail;
    uint32 queue_capacity;
    void *(*allocator)(void *ctx, size_t size);
    void (*deallocator)(void *ctx, void *ptr);
    void *allocator_ctx;
} WASMComponentAsyncEngine;
```

### Execution Model

```
component call lift(f) async
  │
  ├─► create async task T
  │     store args, callback
  │     enqueue T
  │     return task_id (i32) to caller
  │
  └─► caller polls or waits using task_id

engine poll()
  │
  ├─► dequeue pending task
  ├─► execute core function synchronously
  ├─► store results in task T
  ├─► mark T as COMPLETED
  └─► if T has callback:
        call callback(T.results)

task.cancel(task_id)
  │
  ├─► mark T as CANCELLED (if still PENDING)
  └─► clean up T's args/results
```

### Changes

**`wasm_component_async.h/.c`** — new files (~600 lines)
- Task creation/destruction lifecycle
- FIFO pending queue management
- Engine poll/schedule functions
- Callback dispatch

**`wasm_component_runtime.c`** — async function execution (~400 lines)
- Add `WASM_COMP_RUNTIME_FUNC_ASYNC` handling in `wasm_component_call_values_internal`
- When an async function is called:
  1. Create a task from the args
  2. Store the callback function reference
  3. Enqueue the task
  4. Return immediately (don't block)
- Add `wasm_component_async_poll(inst)` public function
- Add `wasm_component_async_wait(inst, task_id)` public function

**`wasm_export.h`** — public API (~30 lines)
- `wasm_runtime_call_component_async(inst, func, num_results, results, num_args, args)` — returns task_id
- `wasm_runtime_async_poll(inst)` — execute one pending task
- `wasm_runtime_async_wait(inst, task_id)` — block until task completes
- `wasm_runtime_async_cancel(inst, task_id)` — cancel a pending task
- `wasm_runtime_async_get_result(inst, task_id, results)` — retrieve task results

**`wasm_component_resource.c`** — async resource handling (~200 lines)
- `wasm_component_resource_drop_owned_handle_async` — async version of handle drop
- Resource handle transfer in async calls (args/results need handle lifecycle across async boundaries)

### Verification
- Basic async call: call async function, poll, get result
- Async callback: async function with callback, verify callback is invoked on completion
- Async cancel: cancel pending task, verify cleanup
- Async resource lifecycle: pass `own<T>` through async call

---

## Phase 4: Async Canon Builtins (3 files, ~400 lines)

### Goal
Implement `task.return`, `task.cancel`, `backpressure.set`, `context.get/set`, `yield` as executable canon builtins.

### Changes

**`wasm_component_runtime.c`** — async builtin trampolines (~300 lines)

- `component_async_task_return_trampoline`: When a core function calls `canon task.return`:
  1. Read results from the core call
  2. Store them in the current task's result slot
  3. Mark the task as COMPLETED
  4. If a callback is registered, dispatch it
  5. Return control to the async engine

- `component_async_task_cancel_trampoline`: When a core function calls `canon task.cancel`:
  1. Look up the task by task_id
  2. If PENDING, mark as CANCELLED and clean up
  3. Return success/failure

- `component_async_backpressure_set_trampoline`:
  1. Store the backpressure flag on the current task
  2. The async engine checks backpressure before enqueueing new tasks

- `component_async_context_get_trampoline`:
  1. Return the context value stored on the current task

- `component_async_context_set_trampoline`:
  1. Set the context value on the current task

- `component_async_yield_trampoline`:
  1. Save the current task's state
  2. Re-enqueue the task as pending
  3. Return control to the async engine

**`wasm_component_runtime.c`** — builtin registration (~100 lines)
- Register each async builtin trampoline in the canon initialization path (similar to `prepare_resource_builtin_function`)
- Each builtin gets a `WASM_COMP_RUNTIME_FUNC_RESOURCE_BUILTIN`-style function entry

### Verification
- `task.return`: core function calls `task.return` with results, verify task completes
- `task.cancel`: call `task.cancel` from core code, verify cancellation
- `context.get/set`: set context in one function, read it in another
- `yield`: yield returns control, engine resumes later

---

## Phase 5: Streams and Futures (4 files, ~800 lines)

### Goal
Implement `stream.new/read/write/cancel-read/cancel-write/drop-readable/drop-writable` and `future.new/read/write/cancel-read/cancel-write/drop-readable/drop-writable` as executable canon builtins.

### Changes

**`wasm_component_async.h`** — stream/future types (~100 lines)
```c
typedef struct WASMComponentAsyncStream {
    uint32 stream_id;
    bool readable_closed;
    bool writable_closed;
    uint8 *buffer;
    uint32 buffer_capacity;
    uint32 buffer_size;
    uint32 read_offset;
    uint32 pending_readers;  // number of tasks waiting to read
    uint32 pending_writers;  // number of tasks waiting to write
} WASMComponentAsyncStream;

typedef struct WASMComponentAsyncFuture {
    uint32 future_id;
    bool readable_closed;
    bool writable_closed;
    uint8 *value;
    uint32 value_size;
    bool value_present;
    uint32 pending_readers;
} WASMComponentAsyncFuture;
```

**`wasm_component_runtime.c`** — stream/future trampolines (~500 lines)

- `component_async_stream_new_trampoline`: allocates a new stream, returns stream_id
- `component_async_stream_read_trampoline`: reads from stream (blocks if empty)
- `component_async_stream_write_trampoline`: writes to stream (blocks if full)
- `component_async_stream_cancel_read_trampoline`: cancels pending reads
- `component_async_stream_cancel_write_trampoline`: cancels pending writes
- `component_async_stream_drop_readable_trampoline`: closes read end
- `component_async_stream_drop_writable_trampoline`: closes write end
- `component_async_future_new_trampoline`: allocates future, returns future_id
- `component_async_future_read_trampoline`: reads future value (blocks if not ready)
- `component_async_future_write_trampoline`: writes future value (wakes readers)
- `component_async_future_cancel_read_trampoline`: cancels pending reads
- `component_async_future_cancel_write_trampoline`: cancels pending writes
- `component_async_future_drop_readable_trampoline`: closes read end
- `component_async_future_drop_writable_trampoline`: closes write end

**`wasm_component_async.c`** — stream/future management (~200 lines)
- Stream allocation table
- Future allocation table
- Lifecycle cleanup on deinstantiation

### Verification
- Stream: write data, read it back
- Future: write value, read it back
- Stream blocking: read from empty stream, write from another task, verify read completes
- Cancel: cancel pending read, verify cancellation
- Cleanup: drop stream ends, verify proper cleanup

---

## Phase 6: Waitable Sets and Thread Spawning (3 files, ~500 lines)

### Goal
Implement waitable sets (`waitable-set.new/wait/poll/drop`, `waitable.join`) and thread spawning (`thread.spawn_ref`, `thread.spawn_indirect`, `thread.available_parallelism`).

### Changes

**`wasm_component_async.h`** — waitable/thread types (~100 lines)
```c
typedef struct WASMComponentAsyncWaitableSet {
    uint32 ws_id;
    uint32 *task_ids;
    uint32 task_count;
    uint32 task_capacity;
} WASMComponentAsyncWaitableSet;

typedef uint32 (*WASMComponentThreadSpawnFunc)(
    void *stack, size_t stack_size, void (*entry)(void*), void *arg);
```

**`wasm_component_runtime.c`** — waitable/thread trampolines (~300 lines)

- `component_async_waitable_set_new_trampoline`: creates a new waitable set
- `component_async_waitable_set_wait_trampoline`: blocks until any task in set completes
- `component_async_waitable_set_poll_trampoline`: non-blocking check
- `component_async_waitable_set_drop_trampoline`: drops the set
- `component_async_waitable_join_trampoline`: joins a task (blocks until completion)
- `component_async_thread_spawn_ref_trampoline`: spawns a task from a core function reference
- `component_async_thread_spawn_indirect_trampoline`: spawns a task from a table reference
- `component_async_thread_available_parallelism_trampoline`: returns available parallelism

**`wasm_component_async.c`** — waitable set management (~100 lines)

### Verification
- Waitable set: create set, add tasks, poll for completion
- Join: join a task, verify it blocks until completion
- Thread spawn: spawn a task, wait for it to complete, verify result

---

## Phase 7: Subtask Cancellation and Deinstantiation (2 files, ~200 lines)

### Goal
Implement `subtask.cancel` and `subtask.drop` canon operations. Add async cleanup to component deinstantiation.

### Changes

**`wasm_component_runtime.c`** — subtask canons (~100 lines)
- `component_async_subtask_cancel_trampoline`: cancels a subtask of the current task
- `component_async_subtask_drop_trampoline`: drops a subtask reference

**`wasm_component_runtime.c`** — deinstantiation cleanup (~100 lines)
- `destroy_component_async_state(inst)`: cancels all pending tasks, frees all streams/futures
- Called from `destroy_component_instance_graph`
- Ensures no tasks leak on component teardown

### Verification
- Subtask cancel: parent task creates subtask, cancels it, verify cleanup
- Deinstantiation: component with pending tasks is deinstantiated, verify cleanup

---

## Phase 8: Comprehensive Tests (~1500 lines)

### Test Categories

1. **Canon opt acceptance tests** (Phase 1)
   - Verify async/callback opts pass validation and instantiation

2. **Basic async execution tests** (Phase 3)
   - Simple async call → poll → result
   - Multiple async calls in flight
   - Async call with callback

3. **Async builtin tests** (Phase 4)
   - `task.return` from core function
   - `task.cancel` on pending task
   - `context.get/set` round-trip
   - `yield` and resume

4. **Stream/future tests** (Phase 5)
   - Stream write then read
   - Future write then read
   - Multiple readers/writers on stream
   - Cancel and drop operations

5. **Waitable/thread tests** (Phase 6)
   - Waitable set with multiple tasks
   - Thread spawn and join
   - Available parallelism

6. **Resource lifecycle tests** (Phase 3-4)
   - `own<T>` through async call
   - `borrow<T>` through async call
   - Cleanup on cancellation

7. **Deinstantiation tests** (Phase 7)
   - Pending tasks on teardown
   - Open streams/futures on teardown

---

## Summary

| Phase | Scope | Est. Lines | Key Files |
|-------|-------|-----------|-----------|
| 1 | Canon opt acceptance | 50 | validate.c, runtime.c, runtime.h |
| 2 | Async canon type acceptance | 50 | validate.c, runtime.c |
| 3 | Core async execution engine | 2000 | async.h, async.c, runtime.c, export.h |
| 4 | Async canon builtins | 400 | runtime.c |
| 5 | Streams and futures | 800 | async.h, async.c, runtime.c |
| 6 | Waitable sets and threads | 500 | async.h, async.c, runtime.c |
| 7 | Subtask cancel and deinstantiation | 200 | runtime.c |
| 8 | Tests | 1500 | test_binary_parser.cc |

**Total estimated: ~5500 lines across 8+ files**

### Key Risks
1. **Thread safety**: The async engine may need locks if multiple host threads can call `async_poll` concurrently
2. **Resource lifecycle**: Owned handles passed through async calls must not be dropped until the async task completes, requiring deferred handle consumption
3. **Callback cycles**: Async callbacks that themselves trigger async calls could create deep or infinite recursion
4. **Backpressure integration**: WASI components may depend on backpressure for flow control; incorrect implementation could cause deadlocks

### Dependencies
- Phase 1-2 can proceed independently
- Phase 3 depends on Phase 1-2
- Phase 4 depends on Phase 3
- Phase 5-7 depend on Phase 3
- Phase 8 depends on all preceding phases

**Phase 1 (canon opt acceptance alone) would be ~50 lines and could be completed in a single session.**
