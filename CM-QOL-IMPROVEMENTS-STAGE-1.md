# Component Model Quality-of-Life Improvements — Stage 1

All major Component Model spec features are now implemented. This document
tracks the remaining quality-of-life improvements, edge case hardening, and
execution breadth extensions that would take the implementation from
"comprehensive" to "production-ready."

---

## 1. Deferred async stub implementations

### 1a. `task.return` with result encoding

**Status:** Stub (no-op)
**Location:** `wasm_component_runtime.c:component_async_builtin_trampoline`
**Effort:** ~80-100 lines

The `task.return` canon builtin allows a core wasm function to return
results early (before reaching its natural `end` instruction). Currently
a no-op because in synchronous execution the function's natural return
serves the same purpose. A full implementation would:

1. Add `WASMComponentResultList *result_list` and
   `WASMComponentCanonOpts *task_return_opts` to `WASMComponentRuntimeFunc`
2. Populate during `append_component_canon_function` for
   `WASM_COMP_CANON_TASK_RETURN`
3. In the trampoline: encode results per `result_list`/`canon_opts`, mark
   task as COMPLETED, dispatch callback

### 1b. `yield` with cooperative scheduling

**Status:** Cancellation check only (no cooperative scheduling)
**Location:** `wasm_component_runtime.c:component_async_builtin_trampoline`
**Effort:** ~40-60 lines

Currently `yield` checks if the task has been externally cancelled. A full
implementation would save task execution state and yield control to the
async engine's poll loop, allowing other pending tasks to execute. This
requires a stackful coroutine mechanism or split-stack approach.

### 1c. `context.get/set` with context inheritance

**Status:** Implemented as per-task array
**Location:** `wasm_component_async.c`
**Effort:** ~30 lines

Context values are stored per-task but not inherited by child tasks
(thread.spawn). Add context array copying from parent task to child
task when spawning threads.

### 1d. Backpressure enforcement

**Status:** Implemented (flag stored, gate in task creation)
**Location:** `wasm_component_async.c`, `wasm_component_runtime.c`
**Effort:** ~10 lines

The backpressure flag is stored and gates `create_task`, but the
thread.spawn path should also check it before spawning new threads.

---

## 2. Broader Canonical ABI execution

### 2a. Canon lower for imported functions with composite return-area shapes

**Status:** Implemented for tested subset
**Effort:** ~200-400 lines of additional tests

The `component_lowered_import_trampoline` handles memory-backed shapes
(strings, lists, composites) for scalar parameters and results. Broader
coverage for complex nested composites (record<list<record<string>>>,
multiple nested memory-backed results) needs additional tests.

### 2b. Canon lower canon opts beyond string-utf8 + memory

**Status:** Mostly implemented
**Effort:** ~100 lines

The lower trampoline validates `string-utf8` and `memory` opts. For
completeness, validate that `utf16` and `latin1+utf16` string encodings
also work through the lower path (not just lift). The trampoline already
resolves `lowered_function->string_encoding` via
`resolve_lowered_import_string_encoding` — test that non-utf8 paths work.

### 2c. Full type compatibility check at import binding boundary

**Status:** Flat-representation size check only
**Location:** `validate_lowered_import_signature`
**Effort:** ~200-400 lines

Currently, lowered import signature validation checks that the flat i32
count matches and that core parameter types match expected i32/i64.
For full spec compliance, validate actual component type compatibility
rather than just projected flat sizes. This would compare the lowered
function's component function type against the core module's expected
function type structurally.

---

## 3. Resource lifecycle hardening

### 3a. Outstanding borrow tracking across nested component boundaries

**Status:** Implemented within a single resource state
**Effort:** ~100-200 lines

When resources are passed between component instances (e.g., an `own<T>`
result from one component is consumed by another), the borrow-count
tracking needs to work across resource state boundaries. The
`remap_resource_type_idx_for_state` function maps types between states,
but borrow count consistency across states needs verification.

### 3b. Trap/failure-path cleanup semantics

**Status:** Partial
**Effort:** ~200-300 lines

When a component function call fails mid-way through result materialization,
owned resource handles that were created during the call should be dropped.
The current rollback mechanism (`rollback_type_idxs`, `rollback_handles`)
handles owned resources in the canon lower trampoline. Broader coverage
for all paths (canon lift, host imports, composite results) is needed.

### 3c. `resource.rep` for imported resources during async drop

**Status:** Implemented but untested through async path
**Effort:** ~50 lines

The `resource.rep` builtin now allows `is_dropping` handles (so the
destructor can read the rep). Test the async drop path end-to-end.

---

## 4. WASI Preview 2/3 integration

### 4a. WASI CLI/P2 host implementation on top of component runtime

**Status:** Not started
**Effort:** ~2000-5000 lines (separate project)

The component model runtime provides the infrastructure for loading
and executing WASI Preview 2/3 components. A WASI host implementation
would provide the `wasi:cli/run`, `wasi:io/streams`, `wasi:filesystem/*`,
etc. imports that WASI components expect. This is a separate effort
from the component model runtime itself.

---

## 5. Public API polish

### 5a. Resource value creation from raw handles

**Status:** Missing
**Effort:** ~50 lines

Add `wasm_component_value_init_own(value, resource_state, type_idx, handle)`
and `wasm_component_value_init_borrow(value, resource_state, type_idx, handle)`
to allow embedders to create resource component values from raw handles
without going through the pending-result pattern.

### 5b. Import discovery API for non-resource imports

**Status:** Missing
**Effort:** ~80 lines

Add `wasm_runtime_get_component_import_count` and
`wasm_runtime_get_component_import_type` (parallel to the export discovery
APIs) so embedders can enumerate a component's imports programmatically.

### 5c. `wasm_component_resource_type_has_destructor` / `has_callback`

**Status:** Missing
**Effort:** ~16 lines

Expose the `has_dtor` and `has_callback` fields of resource types through
the public API.

---

## 6. Test coverage expansion

### 6a. Async stub end-to-end tests

**Status:** Missing
**Effort:** ~300-500 lines

Add end-to-end component binary tests for:
- `task.return` with early return
- `backpressure.set` through component binary
- `context.get/set` through component binary  
- `yield` cancellation through component binary
- `resource.drop.async` through component binary

### 6b. Thread spawn end-to-end test

**Status:** Missing
**Effort:** ~200-300 lines + CMake changes

Add a test that creates a component binary with a lowered function,
spawns a real OS thread via `thread.spawn-ref`, joins it, and verifies
the thread executed. Requires `WAMR_BUILD_THREAD_MGR` in test CMake.

### 6c. GC type end-to-end test

**Status:** Missing
**Effort:** ~200 lines

Add a test that creates a component with GC core types (struct, array)
in the core type section, instantiates a core module that uses those
types, and verifies the types are correctly propagated at runtime.

---

## Summary

| Area | Items | Est. Lines | Priority |
|------|-------|------------|----------|
| Async stubs | 4 (1a-1d) | ~160 | Medium |
| Canonical ABI breadth | 3 (2a-2c) | ~700 | Medium |
| Resource hardening | 3 (3a-3c) | ~450 | Medium |
| WASI integration | 1 (4a) | 2000-5000 | Low (separate project) |
| Public API polish | 3 (5a-5c) | ~146 | Low |
| Test coverage | 3 (6a-6c) | ~900 | Medium-High |
| **Total** | **17** | **~2356+** | |

The highest-impact items are:
1. **6a/6b** — Test coverage for async stubs and threads (proves what exists works)
2. **3b** — Trap/failure-path cleanup (hardens production use)
3. **2c** — Full type compatibility check (catches mismatches at binding time)
