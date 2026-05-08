# Component Model Support Gaps in This Fork

This document summarizes the current gaps in the integrated Component Model implementation from upstream PR #4889. The short version is:

- **The implementation is materially complete as a _component binary parser_.**
- **It is not materially complete as a _component runtime_.**
- **It can parse, validate, and retain component structure, but it cannot yet instantiate and execute components as first-class runtime objects.**

The sections below break that down in detail and point to the relevant code.

## 1. What is actually implemented today

The integrated code adds a substantial parser and validator under `core/iwasm/common/component-model/` and documents that scope explicitly.

The clearest statement is in `doc/component_model.md`:

- `doc/component_model.md:11` says WAMR implements **binary parsing** for the Component Model.
- `doc/component_model.md:47-60` describes a parser pipeline that decodes headers, iterates sections, delegates to per-section parsers, and returns a `WASMComponent` structure.
- `doc/component_model.md:82-85` lists only parser-oriented limitations: unsupported core GC types and unsupported async/callback canon options.

The public parser-facing entry points match that description:

- `core/iwasm/common/component-model/wasm_component.h:1690-1807`
  - `is_wasm_component()`
  - `wasm_component_parse_sections()`
  - per-section parse helpers
  - `wasm_component_free()`

That is a real and non-trivial implementation. It covers the binary format, section decoding, validation, nested components, and embedded core-module parsing. The gap is that this functionality stops at **decoded component data structures**.

## 2. The biggest missing piece: no top-level component runtime

The main functional gap is that WAMR still does **not** expose or implement a complete runtime object model for components.

### Evidence

- `core/iwasm/include/wasm_export.h:128-130` only forward-declares:

  ```c
  struct WASMComponentInstance;
  typedef struct WASMComponentInstance WASMComponentInstance;
  ```

  There is no public instantiate API, no component-export lookup API, no component invocation API, and no component teardown API analogous to the core-module runtime APIs.

- `core/iwasm/interpreter/wasm_runtime.h:479-481` adds placeholders inside module instances:

  - `WASMComponentInstance *comp_instance;`
  - `uint32 core_instance_idx;`

  These fields indicate planned runtime wiring, but by themselves they do not constitute a working component-instance system.

- `core/iwasm/common/component-model/wasm_component_export.c:9-20` contains only a single global boolean flag:

  - `is_component_runtime()`
  - `set_component_runtime(bool)`

  That file is scaffolding, not a runtime implementation.

### Practical consequence

Today, the implementation can produce a parsed `WASMComponent` tree, but it does not produce a first-class runtime object that can:

- instantiate component imports,
- create component instances,
- resolve component exports,
- invoke exported component functions,
- manage component lifetimes as executable entities.

## 3. No top-level component loader path in the public runtime

Another major gap is that the generic runtime load path still loads **core wasm bytecode** and **AoT**, not top-level component packages.

### Evidence

- `core/iwasm/common/wasm_runtime_common.c:1517-1584` (`wasm_runtime_load_ex`) checks `get_package_type(buf, size)` and only accepts:
  - `Wasm_Module_Bytecode`
  - `Wasm_Module_AoT`

  There is no branch that recognizes or returns a top-level component object.

- If the package type is not one of those two, `wasm_runtime_load_ex()` fails with:

  - `WASM module load failed: magic header not detected`

### Practical consequence

Even though the component parser exists, the main WAMR load API is still **not a component loader**. A caller cannot hand a component binary to the normal runtime loading API and expect a component runtime object back.

That is an architectural gap, not a missing convenience wrapper.

## 4. Embedded core modules are parsed, but component instantiation is not implemented

The implementation does parse and load embedded core modules from component sections, but that should not be confused with full component execution.

### Evidence

- `doc/component_model.md:58` states that Core Module sections delegate to `wasm_runtime_load_ex()`.
- `core/iwasm/common/component-model/wasm_component_core_module_section.c` uses `wasm_runtime_load_ex()` to parse embedded core wasm modules.

### Why this is still incomplete

Loading an embedded core module is only one piece of the Component Model. A working component runtime also needs to:

- instantiate the component graph,
- bind component imports and core imports,
- build component instances and core instances in dependency order,
- interpret aliases and exports as live runtime references,
- execute canonical ABI adapters,
- expose component exports in a callable form.

The integrated code does not complete that second half.

## 5. Canonical ABI is parsed and validated, but not executed

The implementation parses Canon sections and validates their option structure, but there is no evidence of a runtime layer that actually performs canonical lifting/lowering.

### Evidence

- `doc/component_model.md:76` describes the Canon section as canonical lift/lower/resource operations.
- `core/iwasm/common/component-model/wasm_component.h:1748-1752` exposes only the parser entry point for the canon section:
  - `wasm_component_parse_canons_section(...)`
- `core/iwasm/common/component-model/wasm_component_validate.c:2043-2124` validates canon options.

### What is missing in practice

There is no visible implementation here for:

- lowering host values into core wasm memory according to the Canonical ABI,
- lifting core wasm results back into component values,
- string encoding/decoding execution,
- list/record/variant flattening and reconstruction,
- adapter trampoline execution,
- post-return execution semantics,
- bridging component-level signatures to callable host/runtime functions.

In other words, the implementation knows how to **parse** canonical declarations, but not how to **run** them.

## 6. Resource handling is structural, not operational

The code parses resource-related constructs, but there is no visible runtime support for actual resource lifecycles.

### Why this matters

Resources are central to practical component execution. A complete runtime needs machinery for:

- resource handle allocation,
- ownership tracking,
- borrow semantics,
- drop semantics,
- representation conversion,
- host integration for resource-backed values.

### Current state

The parser can represent these constructs in parsed form, but there is no exposed runtime subsystem that performs those operations during execution.

That means resource types currently look more like **validated metadata** than executable runtime semantics.

## 7. Import resolution for component mode is only partially scaffolded

There is one runtime hook related to component mode, but it is not enough to qualify as completed import handling.

### Evidence

- `core/iwasm/interpreter/wasm_runtime.c:180-183`:

  ```c
  if (is_component_runtime()) {
      return true;
  }
  ```

This appears in `wasm_resolve_import_func()`, after native symbol resolution fails.

### Why this is a gap

This means that in component mode, unresolved imports may be treated as acceptable by this function, but the code shown here does **not** also provide:

- a complete alternative component import resolver,
- host/component binding tables,
- canonical ABI-backed import adapters,
- clear runtime semantics for how such imports are actually satisfied later.

So the code contains a component-specific relaxation in import resolution, but not the full execution path that would make that relaxation safe and complete.

## 8. The runtime object model is incomplete for component graphs

Component Model execution is not just "load a component, then run it." It needs a runtime object graph for:

- components,
- instances,
- core instances,
- aliases,
- imports,
- exports,
- values,
- adapter functions,
- start sections,
- resources.

The current implementation parses those sections into data structures, but there is no corresponding end-to-end runtime graph construction layer exposed in the runtime APIs.

## 9. Start section semantics are not wired through to execution

The parser supports the Start section structurally:

- `doc/component_model.md:77` lists section `0x09` as Start.
- `core/iwasm/common/component-model/wasm_component.h:1754-1758` exposes `wasm_component_parse_start_section(...)`.

But there is no evidence in the integrated public/runtime surface of:

- component start function execution,
- start-time argument wiring,
- start-time side-effect ordering,
- integration with instance construction or export readiness.

So Start is currently a parsed section, not a completed runtime behavior.

## 10. Alias, import, export, and value sections are available as parsed metadata, not executable interfaces

The parser handles:

- aliases,
- imports,
- exports,
- values,
- component instances,
- core instances.

That is useful, but there is still a significant gap between "I can inspect this section" and "I can use this component at runtime."

### Evidence from tests

`tests/unit/component/test_binary_parser.cc:114-144` validates alias parsing by checking that a parsed alias name exists in the section data.

That confirms:

- alias sections are decoded,
- alias entries are stored and inspectable.

It does **not** demonstrate:

- alias resolution during component execution,
- live exported handle lookup,
- functional invocation through aliases,
- runtime linkage across component boundaries.

The same pattern likely applies across the other component-level sections: the parser retains structure, but the runtime does not yet consume that structure end-to-end.

## 11. Tests cover parser behavior, not runtime behavior

The current tests are valuable, but they also show the present maturity boundary very clearly.

### Evidence

`tests/unit/component/test_binary_parser.cc:42-144` covers:

- component file load/unload,
- corrupted-header rejection,
- header decoding,
- alias section inspection.

### What is not covered

There are no tests here for:

- top-level component instantiation,
- import binding,
- export invocation,
- canonical lifting/lowering correctness,
- resource semantics,
- start function execution,
- WASI Preview 2 behavior,
- host/component interoperability at runtime,
- nested component execution.

That is a large completeness gap, because parser success alone does not prove executable support.

## 12. Validation and CI coverage are still conservative

The current automated validation story still treats component support as something that should be disabled in many mainstream flows.

### Evidence

- Windows CI disables it:
  - `.github/workflows/compilation_on_windows.yml:98`
  - `.github/workflows/compilation_on_windows.yml:133`
- Android/Ubuntu CI disables it:
  - `.github/workflows/compilation_on_android_ubuntu.yml:126`
- The general test harness defaults it off:
  - `tests/wamr-test-suites/test_wamr.sh:1090`
- The same harness turns it on specifically for unit tests:
  - `tests/wamr-test-suites/test_wamr.sh:1157`

### Practical consequence

This tells us two things:

1. The feature is not yet trusted enough to stay enabled across normal CI/build/test coverage.
2. Most regression coverage is still not exercising the component-enabled path by default.

So even where code compiles, the routine validation surface is still narrower than it should be for a mature runtime feature.

## 13. Explicitly unsupported portions of the spec remain

Some gaps are explicit design limitations rather than missing wiring.

### 13.1 Core GC types are rejected

- `doc/component_model.md:84`
- `core/iwasm/common/component-model/wasm_component_core_type_section.c:604-626`

The implementation explicitly rejects:

- `rectype`
- `subtype`

This means it does **not** fully cover the broader future-facing core type space referenced by the component format.

### 13.2 Async and callback canon options are rejected

- `doc/component_model.md:85`
- `core/iwasm/common/component-model/wasm_component_validate.c:2120-2124`

The validator explicitly rejects:

- `async`
- `callback`

This is a real functional gap for any components that rely on those canonical options.

## 14. The public API surface is still parser-centric

A mature component implementation would usually expose some combination of:

- load/compile component,
- instantiate component,
- link imports,
- enumerate component exports,
- invoke component exports,
- destroy component instance,
- inspect component/type metadata through stable APIs.

The integrated public surface does not yet expose that kind of component API family. The observable API is still centered on:

- header detection,
- section parsing,
- freeing parsed data.

That is a major usability and integration gap even if the internal parser is solid.

## 15. The `LoadArgs.is_component` flag is not enough to close the runtime gap

`core/iwasm/include/wasm_export.h:276-294` adds:

- `bool is_component;`

inside `LoadArgs`.

This is helpful scaffolding, but by itself it does not provide:

- component package recognition in `wasm_runtime_load_ex()`,
- a component instance constructor,
- canonical ABI execution,
- a component export invocation path.

Right now the flag mainly looks like a way to influence loader/runtime behavior for embedded core modules in a component context, not a full top-level component execution interface.

## 16. What this implementation is good for today

The current code is already useful for:

- recognizing component binaries,
- parsing and validating component structure,
- building tools or diagnostics that inspect component sections,
- testing and iterating on the binary-format side of the Component Model,
- loading embedded core wasm modules during parsing.

That is meaningful progress and not just a stub.

## 17. What it is not good for yet

It is not yet a complete basis for:

- running general WebAssembly components as first-class programs,
- executing WASI Preview 2/componentized applications end-to-end,
- reliable host-component interop via canonical ABI,
- async/callback-driven component flows,
- resource-heavy component APIs,
- broad CI-backed production support across platforms.

## 18. Summary of the main gaps

The main outstanding gaps are:

1. **No top-level component loader path** in `wasm_runtime_load_ex()`.
2. **No complete `WASMComponentInstance` implementation** exposed through runtime APIs.
3. **No component instantiation pipeline** that builds executable component graphs.
4. **No component export invocation API**.
5. **No canonical ABI execution engine** for lift/lower/resource semantics.
6. **No operational resource management subsystem** for component resources.
7. **No completed runtime handling for component imports/aliases/exports/values** beyond parsed metadata.
8. **No demonstrated start-section execution semantics**.
9. **Explicit rejection of async/callback canon options**.
10. **Explicit rejection of core GC `rectype`/`subtype` forms**.
11. **Parser-focused tests only**, with no runtime execution coverage.
12. **CI and general test flows still disable component mode in many places**.

## 19. Overall assessment

If this feature is described as:

- **"Component Model binary parser support"** — that is broadly accurate.
- **"Component Model runtime support"** — that would overstate its completeness.

The right maturity label today is something like:

> **A strong parser/validator foundation with partial runtime scaffolding, but without full component instantiation and execution support.**

