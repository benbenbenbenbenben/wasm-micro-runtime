# Component Model Support Gaps in This Fork

This document summarizes the **current** state of component-model support in this fork after the recent component-runtime execution work.

The short version is:

- **This fork is no longer parser-only.**
- **It now has a real component runtime surface, including public component APIs.**
- **It still does not implement the full Component Model execution story.**

The main remaining gaps are now centered on:

- Canonical ABI beyond the current scalar / UTF-8 string / `list<u8>` / scalar-leaf tuple-record slices
- canon-lower / imported component-function lowering paths
- broader composite component values and memory-backed leaves inside composites
- operational resource semantics
- remaining public host API limitations
- nested core-module / core-instance / core-type runtime support

## 1. What is implemented today

The implementation is now best described as:

> **A partial but executable component runtime: top-level component loading/instantiation, public import/export APIs, scalar / UTF-8 string / `list<u8>` / limited tuple-record canon-lift calls, host-provided component-function imports for the currently supported subset, runtime values, value imports/exports, start execution slices, and resource bookkeeping foundations are all present.**

### 1.1 Top-level component loading, instantiation, and teardown

Top-level component binaries now flow through the normal runtime entry points:

- `wasm_runtime_load_ex()` / `wasm_component_module_load(...)`
- `wasm_runtime_instantiate_internal()` / `wasm_component_module_instantiate(...)`
- `wasm_runtime_deinstantiate_internal()` / `wasm_component_module_deinstantiate(...)`
- `wasm_runtime_unload()` / `wasm_component_module_unload(...)`

So component binaries are genuine runtime modules/instances, not detached parser artifacts.

### 1.2 Real runtime graph construction

The runtime builds and retains concrete runtime state for:

- embedded core modules
- top-level core instances
- core aliases
- canon-derived component functions
- component values
- component instances
- component definitions
- component exports
- lexical-scope-backed `alias outer` component references

This runtime graph lives primarily in:

- `core/iwasm/common/component-model/wasm_component_runtime.c`
- `core/iwasm/common/component-model/wasm_component_runtime.h`

### 1.3 Public component host APIs now exist

The document used to say this area was missing. That is no longer true.

Public component-facing APIs now include:

- top-level export enumeration:
  - `wasm_runtime_get_component_export_count(...)`
  - `wasm_runtime_get_component_export_type(...)`
  - `wasm_runtime_get_component_export_value(...)`
- top-level lookup:
  - `wasm_runtime_lookup_component_function(...)`
  - `wasm_runtime_lookup_component_value(...)`
  - `wasm_runtime_lookup_component_instance(...)`
  - `wasm_runtime_lookup_component_component(...)`
  - `wasm_runtime_lookup_component_core_module(...)`
- nested-instance lookup:
  - `wasm_component_instance_get_export_count(...)`
  - `wasm_component_instance_get_export_type(...)`
  - `wasm_component_instance_lookup_function(...)`
  - `wasm_component_instance_lookup_instance(...)`
  - `wasm_component_instance_lookup_component(...)`
  - `wasm_component_instance_lookup_core_module(...)`
- invocation:
  - `wasm_runtime_call_component(...)`
  - `wasm_runtime_call_component_values(...)`
- public value helpers:
  - `wasm_component_value_get_data(...)`
  - `wasm_component_value_destroy(...)`

Important nuance: the old generic core-Wasm entry points are still component-unaware in places. For example, `wasm_runtime_lookup_function(...)` and `wasm_runtime_create_exec_env(...)` still reject component instances. The supported surface is the newer **component-specific** API above.

### 1.4 Top-level component imports now have a host-facing API

This also used to be called out as missing; that is no longer accurate.

Embedders can now provide top-level component imports through:

- `InstantiationArgs2`
- `wasm_runtime_instantiation_args_set_component_imports(...)`
- `wasm_component_import_binding_t`

Supported import binding kinds at the public boundary are:

- component functions
- component values
- component instances
- component definitions
- core modules

That is enough to support real top-level import wiring for already-existing runtime handles and public component values.

### 1.5 Canonical ABI execution is partially implemented

Canonical ABI is no longer metadata-only.

What works today:

- `canon lift` runtime metadata resolution
- direct calling of top-level exported canon-lift functions
- scalar parameter/result lifting via `wasm_runtime_call_component(...)`
- UTF-8 string parameter/result handling via `wasm_runtime_call_component_values(...)`
- top-level `list<u8>` parameter/result handling via `wasm_runtime_call_component_values(...)`
- top-level tuple/record parameter/result handling when every leaf is scalar
- memory / realloc / post-return validation and use for supported UTF-8 string lifts
- host-provided top-level component-function imports for scalar / UTF-8 string / `list<u8>` signatures
- host-provided top-level component-function imports for tuple/record parameters with scalar leaves

Current supported execution envelope is intentionally narrow:

- only `canon lift` is executable
- calls must target top-level exported canon-lift functions or the currently supported top-level host-import callback path
- scalar signatures work through the `wasm_val_t` API
- UTF-8 string / `list<u8>` / tuple-record signatures work through the component-value API
- raw `wasm_runtime_call_component(...)` stays scalar-only

### 1.6 Runtime values and value sections are implemented

The runtime now has explicit value machinery:

- `WASMComponentRuntimeValue`
- `WASMComponentRuntimeValueType`
- inline / borrowed / owned storage
- public/runtime value copying helpers

Implemented slices include:

- top-level value section instantiation
- nested value section instantiation
- top-level value exports
- top-level public value imports
- aliasing/re-export of value references through the runtime graph

This is real runtime value plumbing, even though full composite value semantics are still missing.

### 1.7 Start sections now execute in limited form

Start sections are no longer blanket-rejected.

Implemented slices include:

- top-level scalar start execution
- nested scalar start execution
- result materialization back into component values

This support is still intentionally narrow:

- scalar-only start arguments/results
- at most one result
- execution routes through supported canon-lift function shapes

### 1.8 Resource bookkeeping foundations exist

Resources are no longer only parser/validator structure.

The runtime now includes:

- `WASMComponentRuntimeResourceState`
- resource type scanning for local/imported/alias resource types
- canonical resource-type bookkeeping
- owned handle tables
- owned-handle creation/drop helpers
- finalizer cleanup during deinstantiation

This is a real runtime substrate, but not yet full resource semantics.

### 1.9 Test coverage reflects the runtime work

`tests/unit/component/test_binary_parser.cc` now covers more than parsing. It exercises:

- public component export discovery/lookup
- public scalar component calls
- public UTF-8 string component calls
- public `list<u8>` component calls
- public tuple/record component calls
- top-level component import binding
- public value import/export flows
- top-level and nested value sections
- top-level and nested scalar start execution
- top-level host function imports across scalar / UTF-8 string / `list<u8>` / scalar-leaf tuple-record slices
- resource-state and owned-handle cleanup foundations

## 2. What is still missing for full component-model support

This fork is still **not** a complete Component Model runtime.

## 3. Canonical ABI support is still narrow

The executable Canonical ABI surface is currently limited to:

- scalar values
- UTF-8 strings
- top-level `list<u8>`
- top-level tuple/record values with scalar leaves
- top-level exported `canon lift`
- top-level host-defined component-function imports for the same supported subset

Major Canonical ABI gaps remain:

- no executable `canon lower`
- no general adapter/lowering path for imported component functions beyond the supported host-callback subset
- no list marshalling beyond UTF-8 strings and top-level `list<u8>`
- no variant / flags / enum / option / result marshalling
- no nested memory-backed leaves inside tuple/record Canonical ABI values yet
- no broader composite flattening/lifting rules beyond scalar-leaf tuple/record support
- no non-UTF-8 string encodings (`utf16`, `latin1+utf16`)
- no `memory64` memory-backed Canonical ABI support
- no `error-context` value support
- no async/callback canon options

So "Canonical ABI execution" is now **partially true**, but only for a small supported subset.

## 4. Public host APIs are present, but still incomplete

The public host story is much better than before, but still not complete.

Current limitations include:

- generic `wasm_runtime_lookup_function(...)` still rejects component instances
- generic `wasm_runtime_create_exec_env(...)` still rejects component instances
- `wasm_runtime_call_component(...)` / `wasm_runtime_call_component_values(...)` only accept top-level exported canon-lift handles
- nested function handles can be discovered, but not invoked through the public top-level call API
- top-level import binding is limited to existing runtime handles / public values and the current supported host callback subset, not arbitrary host-native lowered adapters
- host-import composite results are still limited beyond the current scalar/string/`list<u8>` surface
- there is still no public resource import/export contract comparable to the current function/value/instance/component/core-module surface

## 5. Broader component values are still missing

The runtime now has value objects, value imports, value exports, and value sections, but the value model is still much narrower than the full spec.

What works today:

- primitive scalar values
- raw borrowed/owned value payload storage
- opaque defined-value payloads for UTF-8 strings and top-level `list<u8>` calls
- opaque defined-value payloads for tuple/record composites in the currently supported scalar-leaf cases

What is still missing:

- first-class runtime semantics for general lists
- first-class typed runtime semantics for records
- first-class typed runtime semantics for tuples
- variants
- flags
- enums
- options
- results
- richer defined-type introspection/manipulation

Today, composite value support is still mostly "opaque bytes plus limited special cases", not full typed component-value semantics.

## 6. Start semantics are only partially implemented

Start sections are no longer absent, but the implementation is still limited.

Supported today:

- top-level scalar starts
- nested scalar starts
- materializing a single scalar result as a component value

Still missing:

- string/composite start arguments and results
- multi-result start handling
- broader Canonical ABI-backed start execution
- the more complete execution space needed for start-heavy real-world components

## 7. Resources are structural/bookkeeping-heavy, not operational

Resource support has meaningful runtime foundations now, but the operational model is still incomplete.

What exists:

- resource type bookkeeping
- alias/import tracking
- owned handle allocation/drop helpers
- deinstantiate-time cleanup/finalization

What is still missing:

- full ownership/borrow/lend semantics
- live Canonical ABI resource lowering/lifting
- resource imports/exports as a complete public host feature
- runtime enforcement of richer resource lifecycle rules
- integration with callable component APIs
- full trap/failure-path operational cleanup semantics

So resources have a foundation, not a finished runtime.

## 8. Nested core runtime support is still incomplete

This remains one of the clearest hard gaps.

Nested components still reject:

- nested `core module` sections
- nested `core instance` sections
- nested `core type` sections

The runtime can still thread some existing core handles through component graphs, but it does **not** yet construct a full nested core runtime.

## 9. Remaining spec limitations still apply

Several validator/runtime limitations remain explicit:

- unsupported core GC forms such as `rectype` / `subtype`
- async canon options rejected
- callback canon options rejected

These are still real spec-coverage gaps, not just missing convenience APIs.

## 10. What this fork is good for today

This fork is already useful for:

- loading and instantiating component binaries
- exploring non-trivial component runtime graphs
- enumerating and looking up component exports through public APIs
- calling supported top-level canon-lift exports
- exercising scalar and UTF-8 string lift execution
- using value imports/exports and value sections
- experimenting with limited top-level and nested start execution
- testing resource bookkeeping foundations

## 11. What it is still not good for yet

It is still not a complete basis for:

- full Canonical ABI execution
- imported component-function lowering/adapters
- general composite component values
- complete resource-heavy component APIs
- full nested core-runtime execution
- broad WASI Preview 2 style application execution

## 12. Overall assessment

If this feature is described as:

- **"Component Model binary parser support"** - accurate
- **"Partial Component Model runtime support"** - definitely accurate
- **"Partial executable Component Model host/runtime API"** - now also accurate
- **"Full Component Model runtime support"** - still inaccurate

The right maturity label today is:

> **A real but still partial component runtime: public host APIs, scalar / UTF-8 string / `list<u8>` / limited tuple-record canon-lift calls, supported host-provided component-function imports, runtime values, value imports/exports, start execution slices, and resource bookkeeping foundations are implemented; full support is still blocked on canon-lower/imported-function lowering, broader composite Canonical ABI and value semantics, operational resources, remaining host API gaps, and nested core-runtime support.**
