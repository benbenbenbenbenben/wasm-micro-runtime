# Component Model Runtime — Developer Handover Guide

## Overview

This fork of `wasm-micro-runtime` has been extended with comprehensive
Component Model runtime support. All major spec features are implemented:
async Canonical ABI, GC core forms, memory64, nested core runtime, resource
lifecycle, OS thread spawning, and public host APIs. The remaining work
is documented in `CM-QOL-IMPROVEMENTS-STAGE-1.md` and focuses on quality-
of-life improvements, edge case hardening, and broader test coverage.

## Quick Start

```bash
# Build the runtime library
cd /mnt/faststorage/repos/wasm-micro-runtime
cmake -S . -B build
cmake --build build

# Build and run the component model test suite
cmake -S tests/unit -B build/test-unit-component
cmake --build build/test-unit-component
cd build/test-unit-component/component && ./component
```

All 596 tests should pass.

## Working on the Codebase

### 1. Choose a task

Open `CM-QOL-IMPROVEMENTS-STAGE-1.md`. Each item has:

- **Area** — e.g., "Async stubs", "Test coverage expansion"
- **Priority** — Medium, Low, Medium-High
- **Estimated lines** — approximate scope
- **Status** — the current implementation state
- **Location** — file:line for the relevant code
- **Description** — what needs to change

Pick an item that interests you and fits your available time.

### 2. Understand the codebase

Key files:

| File | Purpose |
|------|---------|
| `core/iwasm/common/component-model/wasm_component_runtime.c` | Main runtime (~24700 lines). Canon lift/lower trampolines, import binding, ABI flattening, resource builtins, async builtin dispatch, public API implementations, instantiation, exports, value resolution |
| `core/iwasm/common/component-model/wasm_component_runtime.h` | Runtime structs (`WASMComponentInstance`, `WASMComponentRuntimeFunc`, etc.) |
| `core/iwasm/common/component-model/wasm_component_async.c` | Async engine: task lifecycle, stream/future read/write, waitable sets, error-context, thread management (~1300 lines) |
| `core/iwasm/common/component-model/wasm_component_async.h` | Async engine structs and public API declarations |
| `core/iwasm/common/component-model/wasm_component_resource.c` | Resource handle table, borrow/drop/take/restore, cross-instance transfer, repurpose (~1400 lines) |
| `core/iwasm/common/component-model/wasm_component_resource.h` | Resource structs (handle entry, resource type, public resource value) |
| `core/iwasm/common/component-model/wasm_component_resource_call.c` | Borrowed resource parameter tracking during calls |
| `core/iwasm/common/component-model/wasm_component_value.c` | Public value helpers, resource value lifecycle (~750 lines) |
| `core/iwasm/common/component-model/wasm_component_validate.c` | Component binary validation (~3100 lines) |
| `core/iwasm/common/component-model/wasm_component_core_type_section.c` | Core type section parser (~1270 lines, GC types included) |
| `core/iwasm/common/component-model/wasm_component_canons_section.c` | Canon section parser |
| `core/iwasm/include/wasm_export.h` | Public API declarations (~2900 lines) |
| `tests/unit/component/test_binary_parser.cc` | All component model tests (~66400 lines) |
| `tests/unit/component/wasm-apps/` | Pre-compiled `.wasm` modules used by tests |
| `ASYNC-PLAN.md` | Original async implementation plan (historical reference) |

### 3. Write your code

**Conventions:**

- **C89/C99 style** in the runtime (the upstream style). No C++ features.
- **C++ with GTest** in the test file. Test names use `CamelCase`.
- **Memory allocation**: Use `wasm_runtime_malloc`/`wasm_runtime_free`/
  `wasm_runtime_realloc` (not `malloc`/`free`).
- **Error handling**: Return `false`/`NULL` and set `error_buf`. Use
  `set_component_runtime_error_fmt()` for formatted errors.
- **String handling**: `clone_core_name()` duplicates names; test helpers
  manage their own memory.
- **No comments** — the code should be self-documenting. Use descriptive
  function and variable names.

**Patterns to follow:**

- For canon builtin dispatch: add a case in
  `component_async_builtin_trampoline()` or
  `component_resource_builtin_trampoline()` in `wasm_component_runtime.c`
- For new public API functions: add declaration in `wasm_export.h` with
  `WASM_RUNTIME_API_EXTERN`, implement in `wasm_component_runtime.c`
- For tests: follow the pattern in `test_binary_parser.cc` using GTest's
  `TEST_F(BinaryParserTest, TestName)` with the `ComponentHelper` fixture
- For programmatic component binary construction: use the static helpers
  at the top of `test_binary_parser.cc` (e.g.,
  `append_top_level_lowered_core_caller_sections_for_func`,
  `append_top_level_core_export_lift_sections`)

### 4. Run tests

```bash
cd /mnt/faststorage/repos/wasm-micro-runtime

# Build the main library (always do this first to catch compilation errors)
cmake --build build

# Build the test binary
cmake --build build/test-unit-component

# Run the full suite (596 tests, ~20 seconds)
cd build/test-unit-component/component && ./component

# Run a single test by name
./component --gtest_filter="BinaryParserTest.TestName"
```

**Important:** Run the full suite before committing. All 596 tests must pass.

If a test fails, figure out whether your change caused it. If you changed
behavior that makes a "Rejects" test outdated (e.g., a function that used
to return an error now succeeds), update the test's expected error message
or convert it from a rejection test to an acceptance test.

### 5. Update documentation

After completing an item from `CM-QOL-IMPROVEMENTS-STAGE-1.md`, update that
file to mark the item as done. Change the "Status" line and optionally add
a note about what was implemented.

If you close a gap in `CM-SUPPORT-GAPS.md`, update that file too.

### 6. Commit

```bash
# Stage your changes
git add <files>

# Commit with a descriptive message
git commit -m "Brief description of what changed

Details about why and how, references to CM-QOL-IMPROVEMENTS items.

596 tests passing."
```

**Commit style:**
- First line: summary (~50 chars)
- Second line: blank
- Third line+: details, motivation, tradeoffs
- Last line: test count (e.g., "596 tests passing.")

### 7. Repeat

Pick the next item from `CM-QOL-IMPROVEMENTS-STAGE-1.md` and go to step 1.

## Debugging Tips

### Finding where an error message comes from

```bash
grep -rn "the error message text" core/iwasm/common/component-model/
```

### Understanding a test failure

```bash
cd build/test-unit-component/component
./component --gtest_filter="TestName" 2>&1
```

### Getting a backtrace from a crash

```bash
cd build/test-unit-component/component
gdb -batch -ex "run" -ex "bt" --args ./component --gtest_filter="TestName"
```

### Finding what a canon tag maps to

```bash
grep -n "WASM_COMP_CANON_STREAM_NEW\|canon_tag.*=" core/iwasm/common/component-model/wasm_component.h
```

## Key Architectural Decisions

1. **Synchronous poll model**: Async tasks execute synchronously when
   `wasm_component_async_poll_task()` is called. There is no preemptive
   scheduling. `yield` checks cancellation but doesn't switch tasks.

2. **Lowered imports vs canon builtins**: Lowered imports (from `canon
   lower`) route through `component_lowered_import_trampoline`. Async
   canon builtins (stream.new, future.read, etc.) route through
   `component_async_builtin_trampoline`. Resource builtins (resource.new,
   resource.drop) route through `component_resource_builtin_trampoline`.
   The dispatch decision is in `component_lowered_import_trampoline` at
   line ~5190 — check `WASM_COMP_RUNTIME_FUNC_RESOURCE_BUILTIN` kind,
   then dispatch based on canon tag.

3. **Component instance vs core module instance**: `WASMComponentInstance`
   is the component-level runtime struct. Core module instances within a
   component are represented as `WASMComponentCoreRuntimeInstance`. The
   async engine lives on the component instance (`inst->async_engine`).
   When the trampoline needs the component instance, use
   `lowered_function->owner_instance`, NOT `caller_module_inst` (which
   is the core module instance that called the import).

4. **Thread safety**: The async engine has a `korp_mutex lock`. Lock before
   accessing shared state (task table, stream/future arrays). Release the
   lock during blocking calls (wasm function calls). `current_task_id` is
   thread-local via `tls_current_task_id`.

5. **Resource type identity**: Resource types are identified by
   `type_idx` (component-level) and `canonical_type_idx` (canonical).
   Cross-instance matching uses `remap_resource_type_idx_for_state` which
   compares import names or canonical type indices.

## Test Infrastructure

Tests are in `tests/unit/component/test_binary_parser.cc` (~66400 lines).
They use the `ComponentHelper` fixture which sets up the WASM runtime,
loads `.wasm` files, and provides helper methods.

**Patterns:**

- **Component binary test**: Load `.wasm` via `helper->read_wasm_file()`,
  then `wasm_runtime_load_ex()` → modify component sections →
  `wasm_runtime_instantiate()` → call functions → verify.

- **Direct API test**: Call runtime functions directly without a component
  binary (e.g., `wasm_component_async_engine_create()` → create stream →
  verify).

- **Static helpers**: Can modify the component binary's parsed sections
  before instantiation (e.g., `append_top_level_canon_lower_relift_sections`,
  `ensure_canon_lift_memory_opt`).

**Test modules:** Pre-compiled `.wasm` files live in
`tests/unit/component/wasm-apps/`. The CMakeLists copies them to the
binary directory. To create a new one, write a `.wat` file and compile
with `wasm-tools parse`.
