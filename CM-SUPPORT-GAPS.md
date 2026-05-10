# Component Model Support Gaps in This Fork

This document summarizes the **current** state of component-model support in this fork after the recent component-runtime execution work.

The short version is:

- **This fork is no longer parser-only.**
- **It now has a real component runtime surface, including public component APIs.**
- **It still does not implement the full Component Model execution story.**

The main remaining gaps are now centered on:

- Canonical ABI beyond the current scalar / UTF-8 string / `list<scalar>` /
  supported tuple-record slices
- broader canon-lower / imported component-function lowering paths
- broader composite component values and memory-backed leaves inside composites
- operational resource semantics
- remaining public host API limitations
- nested core-instance / core-type runtime support

## 1. What is implemented today

The implementation is now best described as:

> **A partial but executable component runtime: top-level component loading/instantiation, public import/export APIs, scalar / UTF-8 string / `list<scalar>` / limited tuple-record canon-lift calls, a narrow direct-core-call `canon lower` seam for scalar signatures plus top-level UTF-8 string / `list<scalar>` params/results on the specifically tested memory-backed path, host-provided component-function imports for the currently supported subset, runtime values, value imports/exports, start execution slices, and resource bookkeeping foundations are all present.**

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

Important nuance: the old generic core-Wasm entry points are still only
**partially** component-aware. `wasm_runtime_lookup_function(...)` now accepts
component instances for top-level exported component functions whose signatures
fit the generic scalar `wasm_val_t` shape, and `wasm_runtime_create_exec_env(...)`
now also accepts component instances. Actual component invocation still routes
through the newer **component-specific** APIs above.

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

When a component declares `core module` types, the runtime now also validates
bound core-module handles against the currently supported module-type subset for
import/export matching instead of treating those type indices as inert metadata.

When a component declares `instance` types, the runtime now also validates
bound instance handles against the current supported `instancetype` subset.
That support works for both top-level public imports and nested `with_args`
instance bindings, and currently covers:

- exported scalar `func` members
- exported top-level public `func` members, plus cross-component nested
  `with_args` `func` members, whose signatures stay within the current scalar /
  UTF-8 string / variable-length `list<u8>` / tuple-record leaf subset, with
  at most one result
- exported `core module` members
- exported scalar `value` members
- exported variable-length `list<u8>` `value` members
- exported tuple/record `value` members in the current scalar / UTF-8 string /
  nested `list<u8>` / `list<string>` leaf subset
- exported nested `instance` members, including recursive validation

Typed matching of exported component `func` / `value` / `component` members is
still incomplete: typed function matching remains limited to the current scalar /
UTF-8 string / variable-length `list<u8>` / tuple-record subset, typed value
matching beyond the current scalar / UTF-8 string / variable-length `list<u8>` /
`list<string>` tuple-record subset is still incomplete, and typed exported
`component` matching currently only covers zero-import component types plus the
recursive typed-`component`-export subset when the actual exports carry explicit
component type metadata.

### 1.5 Canonical ABI execution is partially implemented

Canonical ABI is no longer metadata-only.

What works today:

- `canon lift` runtime metadata resolution
- direct calling of supported top-level and nested canon-lift function handles
- direct core-wasm calls into lowered component functions for the narrow tested
  subset:
  - scalar-only parameters/results
  - top-level tuple/record parameters with scalar, nested UTF-8 string, and
    nested `list<scalar>` leaves
  - top-level tuple/record results with scalar, nested UTF-8 string, and nested
    `list<scalar>` leaves through the tested explicit i32 return-area pointer path
  - UTF-8 string parameters/results through i32 pointer/length pairs plus an
    explicit i32 return-area pointer
  - `list<scalar>` parameters with scalar results
  - top-level `list<scalar>` results through an explicit i32 return-area pointer
  - nested component-owned child core modules consuming that same direct lowered
    subset on the tested UTF-8 string / `list<u8>`- and `list<s32>`-parameter /
    `list<scalar>`-result / mixed composite-param / mixed composite-result paths
  - lower-side `(string-utf8)` plus `(memory ...)` for the tested direct string
    path, and `(memory ...)` for the tested direct `list<scalar>` parameter/result
    path
  - top-level and nested downstream core modules instantiated with explicit
    `with_args` lowered-function bindings
- scalar parameter/result lifting via `wasm_runtime_call_component(...)`
- UTF-8 string parameter/result handling via `wasm_runtime_call_component_values(...)`
- top-level `list<scalar>` parameter/result handling via `wasm_runtime_call_component_values(...)`
- top-level tuple/record parameters with scalar, nested UTF-8 string, and nested `list<scalar>` leaves
- top-level exported `canon lift` tuple/record results with scalar, UTF-8 string, and nested `list<scalar>` leaves
- memory / realloc / post-return validation and use for supported UTF-8 string lifts
- host-provided component-function imports for scalar / UTF-8 string / `list<scalar>` signatures
- host-provided component-function imports for tuple/record parameters with scalar, UTF-8 string, and nested `list<scalar>` leaves
- host-provided component-function imports for tuple/record results with scalar,
  UTF-8 string, and nested `list<scalar>` leaves

Current supported execution envelope is intentionally narrow:

- `canon lift` is executable for the currently supported scalar / UTF-8 string /
  `list<scalar>` / tuple-record subset
- `canon lower` is executable only for the currently tested direct core-call
  subset above
- calls may target supported top-level or nested canon-lift / host-import function
  handles discovered through the runtime graph, plus direct core-wasm calls through
  the narrow lowered-import path above
- scalar signatures work through the `wasm_val_t` API
- UTF-8 string / `list<scalar>` / tuple-record signatures work through the component-value API
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
- typed top-level and nested value-instance import validation for the current
  scalar / UTF-8 string / variable-length `list<u8>` / `list<string>` /
  tuple-record leaf subset
- aliasing/re-export of value references through the runtime graph

This is real runtime value plumbing, even though full composite value semantics are still missing.

### 1.7 Start sections now execute in limited form

Start sections are no longer blanket-rejected.

Implemented slices include:

- top-level start execution for the current public-value subset
- nested start execution for the current public-value subset
- result materialization back into component values

This support is still intentionally narrow:

- start arguments/results are limited to the currently supported public-value
  subset: UTF-8 strings, top-level `list<u8>`, and tuple/record values with
  scalar / nested UTF-8 string / nested `list<u8>` leaves
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
- public `list<scalar>` component calls
- public tuple/record component calls
- top-level component import binding
- typed top-level and nested component-instance import binding for the current
  exported scalar-func / core-module / scalar-value / nested-instance subset
- public value import/export flows
- top-level and nested value sections
- top-level and nested start execution for the current supported public-value
  subset, still capped at a single result
- top-level host function imports across scalar / UTF-8 string / `list<scalar>` /
  supported tuple-record parameter and result slices with scalar, UTF-8 string,
  and nested `list<scalar>` leaves
- resource-state and owned-handle cleanup foundations

## 2. What is still missing for full component-model support

This fork is still **not** a complete Component Model runtime.

## 3. Canonical ABI support is still narrow

The executable Canonical ABI surface is currently limited to:

- scalar values
- UTF-8 strings
- top-level `list<scalar>`
- top-level tuple/record parameters with scalar, nested UTF-8 string, and nested `list<scalar>` leaves
- top-level exported tuple/record results with scalar, UTF-8 string, and nested `list<scalar>` leaves
- top-level exported `canon lift`
- top-level and nested synthetic `lift(lower(f))` round-trips when `lower`
  targets an existing runtime `canon lift` handle for the currently tested
  subset:
  - scalar signatures
  - UTF-8 string signatures through the component value API
  - `list<scalar>` parameter signatures, including empty-list input
  - `list<scalar>` results
  - tuple/record parameter signatures over the current scalar / UTF-8 string /
    nested `list<scalar>` / `list<string>` leaf subset
  - record-result signatures
  - tuple-result and mixed tuple/record-result signatures through the component
    value API over that same supported leaf subset, including nested
    `list<string>` leaves on the mixed composite param/result paths
  - explicit rejection of lower-side canon options and outer synthetic-lift
    canon options on this synthetic path
- direct core-wasm invocation of lowered component functions for the currently
  tested direct subset:
  - scalar parameters/results
  - top-level tuple/record parameters over the tested scalar / UTF-8 string /
    nested variable-length `list<scalar>` / `list<string>` leaf subset with
    scalar results
  - top-level tuple/record results over that same supported leaf subset through
    the tested `(param i32) -> ()` return-area path
  - UTF-8 string parameters/results through the tested `(param i32 i32 i32) -> ()`
    direct return-area path
  - top-level `list<string>` parameters with scalar results through the tested
    `(param i32 i32) -> i32` direct path
  - top-level `list<string>` results through the tested `(param i32) -> ()`
    return-area path
  - `list<scalar>` parameters with scalar results
  - top-level `list<scalar>` results through the tested `(param i32) -> ()`
    return-area path
  - nested component-owned child-core consumers on the tested direct UTF-8 string,
    `list<string>`-parameter / `list<string>`-result, `list<u8>`- and
    `list<s32>`-parameter, `list<scalar>`-result, mixed composite-param, and
    mixed composite-result paths, including tuple/record mixed composite-param
    witnesses with a nested `list<string>` leaf
  - tested cross-component scalar, UTF-8 string, `list<string>`-parameter /
    `list<string>`-result, `list<u8>`-parameter / `list<u8>`-result, and
    `list<s32>`-parameter / `list<s32>`-result seams plus tested
    cross-component mixed composite-param seams and mixed composite-result seams
    (including a nested `list<string>` leaf on both the param and result
    paths), where a nested child-core lowered import targets a function exported
    from another component instance
  - no lower-side canon options beyond tested `(string-utf8)` / `(memory ...)`
    for the direct string, direct `list<string>`, and direct tuple/record
    `list<string>`-leaf parameter/result paths, and `(memory ...)` for the
    `list<scalar>` parameter/result path
  - positive top-level and nested direct child-core witnesses plus explicit
    failures for invalid UTF-8 input, omitted lower-side memory, and malformed
    result areas on the tested memory-backed paths
- top-level host-defined component-function imports for the same supported subset

Major Canonical ABI gaps remain:

- no general executable `canon lower`; the only supported executable lowering
  paths are:
  - the narrow direct core-call subset above
  - the synthetic scalar / UTF-8 string / `list<scalar>`-parameter /
    `list<scalar>`-result / tuple/record-parameter / record-result /
    tuple/mixed-composite-result `lift(lower(f))` subset above, including
    nested `list<string>` leaves on the mixed composite param/result paths
- no general adapter/lowering path for imported component functions beyond the
  supported host-callback subset, the tested cross-component scalar / UTF-8
  string / `list<string>`-parameter / `list<string>`-result /
  `list<u8>`-parameter / `list<u8>`-result / `list<s32>`-parameter /
  `list<s32>`-result / mixed composite-param / mixed composite-result seams
  above (including a nested `list<string>` leaf on the mixed composite param
  and result paths), and the narrow direct core-call subset above
- no executable lower path yet for memory-backed Canonical ABI shapes
  beyond the tested direct UTF-8-string parameter/result path, the tested
  direct top-level `list<string>` parameter and result paths, the tested nested
  child-core `list<string>` parameter/result paths, cross-component UTF-8
  string / `list<string>`-parameter / `list<string>`-result /
  `list<u8>`-parameter / `list<u8>`-result / `list<s32>`-parameter /
  `list<s32>`-result / mixed composite-param / mixed composite-result seams,
  the tested direct
  `list<scalar>`-parameter-with-scalar-result path, the tested top-level direct
  `list<scalar>`-result return-area path, the tested nested child-core UTF-8
  string / `list<u8>`- and `list<s32>`-parameter / `list<scalar>`-result /
  mixed composite-result paths, and the synthetic `list<scalar>`-parameter /
  `list<scalar>`-result / tuple/record-parameter / record-result /
  tuple/mixed-composite-result synthetic re-lifts above
- no executable lower path yet for direct tuple/record results beyond the tested
  top-level memory-backed return-area path over the current scalar / UTF-8
  string / nested variable-length `list<scalar>` / `list<string>` leaf subset
- no executable lower path yet for broader nested lowered-consumer coverage beyond
  the tested nested child-core UTF-8 string / `list<string>`-parameter /
  `list<string>`-result / `list<u8>`- and `list<s32>`-parameter /
  `list<scalar>`-result / mixed composite-param / mixed composite-result paths
  (including a nested `list<string>` leaf on the mixed composite-param path)
  and the tested scalar / UTF-8 string / nested `list<scalar>` tuple-record
  subset
- no list marshalling beyond UTF-8 strings, the tested direct top-level and
  nested child-core `list<string>` parameter/result paths, the tested
  cross-component `list<string>` parameter/result seams, top-level
  `list<scalar>`, and the tested top-level direct tuple/record
  `list<string>`-leaf plus nested `list<scalar>` leaves in exported
  tuple/record values and host-import tuple/record parameter/result values
- no variant / flags / enum / option / result marshalling
- no non-string memory-backed leaves inside tuple/record Canonical ABI values beyond nested `list<scalar>` leaves for exported canon-lift calls and host-import tuple/record parameters
- no broader composite flattening/lifting rules beyond the current
  string / variable-length `list<scalar>` / variable-length `list<string>`
  tuple-record subset
- no non-UTF-8 string encodings (`utf16`, `latin1+utf16`)
- no `memory64` memory-backed Canonical ABI support
- no `error-context` value support
- no async/callback canon options

So "Canonical ABI execution" is now **partially true**, but only for a small supported subset.

## 4. Public host APIs are present, but still incomplete

The public host story is much better than before, but still not complete.

Current limitations include:

- generic `wasm_runtime_lookup_function(...)` only exposes top-level exported
  component functions whose signatures stay within the generic scalar
  `wasm_val_t` shape; string / `list<scalar>` / tuple-record component functions
  still require the component-specific lookup/call APIs
- `wasm_runtime_call_component(...)` remains scalar-only even for nested handles
- `wasm_runtime_call_component_values(...)` still only supports the current
  string / `list<scalar>` / `list<string>` / limited tuple-record subset
- lowered core-function execution is still only exposed indirectly through
  synthetic re-lifted component exports; there is no general public API for
  invoking or binding lowered core functions
- top-level import binding is limited to existing runtime handles / public values and the current supported host callback subset, not arbitrary host-native lowered adapters
- typed function import matching now covers direct top-level bindings plus
  top-level, explicit cross-component `canon lift` runtime handles, and
  same-module plus cross-component nested typed `instance` import `func`
  members for the current scalar / UTF-8 string / variable-length `list<u8>` /
  tuple-record leaf subset; typed `instance` import matching is otherwise
  limited to exported `core module`, scalar / variable-length `list<u8>` /
  current tuple-record-subset `value`, nested `instance` members, and the first
  typed `component` subset: top-level component imports plus typed `instance`
  component members whose expected component types have no imports and only
  recurse through typed `component` exports with explicit component type
  metadata; broader componenttype matching is still unsupported
- host-import tuple/record values are still limited to the current scalar /
  UTF-8 string / nested `list<scalar>` / `list<string>` subset
- there is still no public resource import/export contract comparable to the current function/value/instance/component/core-module surface

## 5. Broader component values are still missing

The runtime now has value objects, value imports, value exports, and value sections, but the value model is still much narrower than the full spec.

What works today:

- primitive scalar values
- raw borrowed/owned value payload storage
- opaque defined-value payloads for UTF-8 strings and top-level `list<scalar>` calls
- opaque defined-value payloads for tuple/record composites in the currently
  supported scalar / UTF-8 string / nested `list<scalar>` / `list<string>` cases

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

- top-level start execution for the current supported public-value subset
- nested start execution for the current supported public-value subset
- materializing at most one start result as a component value

Still missing:

- multi-result start handling
- Canonical ABI beyond the current supported public-value subset
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

Nested components now support:

- nested local `core module` sections
- export/re-export of those nested core modules through nested component instances
- nested local `core instance` sections in instantiate/`with_args` form
- nested inline/`without_args` `core instance` expressions for core-module
  re-exports
- nested `core type` sections as tolerated structural metadata around those
  flows
- typed nested `core module` imports that use those `core type` entries for
  module import/export matching

Nested components still reject:

- broader operational use of nested `core type` entries beyond the current
  typed core-module import matching subset

The runtime can now thread nested local core-module handles and construct nested
local/synthetic core instances, but it still does **not** construct a full
nested core runtime.

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
- calling supported top-level and nested component function handles
- exercising scalar and UTF-8 string lift execution
- using value imports/exports and value sections
- experimenting with limited top-level and nested start execution
- testing resource bookkeeping foundations

## 11. What it is still not good for yet

It is still not a complete basis for:

- full Canonical ABI execution
- broad imported component-function lowering/adapters
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

> **A real but still partial component runtime: public host APIs, scalar / UTF-8 string / `list<scalar>` / limited tuple-record canon-lift calls, a narrow executable direct-core-call `canon lower` subset, supported host-provided component-function imports, runtime values, value imports/exports, start execution slices, and resource bookkeeping foundations are implemented; full support is still blocked on broader canon-lower/imported-function lowering, broader composite Canonical ABI and value semantics, operational resources, remaining host API gaps, and nested core-runtime support.**
