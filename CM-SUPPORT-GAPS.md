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
- broader operational resource semantics
- remaining public host API limitations
- nested core-instance / core-type runtime support

## 1. What is implemented today

The implementation is now best described as:

> **A partial but executable component runtime: top-level component loading/instantiation, public import/export APIs, scalar / UTF-8 string / `list<scalar>` / limited tuple-record canon-lift calls, a narrow direct-core-call `canon lower` seam for scalar signatures plus top-level UTF-8 string / `list<scalar>` params/results on the specifically tested memory-backed path, host-provided component-function imports for the currently supported subset, runtime values, value imports/exports, start execution slices, and a narrow executable resource-builtin seam are all present.**

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
  - `wasm_runtime_drop_component_owned_result(...)`
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
  UTF-8 string / variable-length `list<scalar>` / `list<string>` /
  tuple-record leaf subset, with at most one result
- exported `core module` members
- exported scalar `value` members
- exported variable-length `list<scalar>` / `list<string>` `value` members
- exported tuple/record `value` members in the current scalar / UTF-8 string /
  nested `list<scalar>` / `list<string>` leaf subset
- exported nested `instance` members, including recursive validation

Typed matching of exported component `func` / `value` / `component` members is
still incomplete: typed function matching remains limited to the current scalar /
UTF-8 string / variable-length `list<scalar>` / `list<string>` /
tuple-record subset, typed value matching beyond the current scalar / UTF-8 string /
variable-length `list<scalar>` / `list<string>` tuple-record subset is still
incomplete, and typed exported
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

Those value-section/value-export paths are now exercised not just for scalars,
but also for opaque tuple/record payloads with nested `list<string>` leaves.

This is real runtime value plumbing, even though full composite value semantics are still missing.

### 1.7 Start sections now execute in limited form

Start sections are no longer blanket-rejected.

Implemented slices include:

- top-level start execution for the current public-value subset
- nested start execution for the current public-value subset
- result materialization back into component values

This support is still intentionally narrow:

- start arguments/results are limited to the currently supported public-value
  subset: UTF-8 strings, top-level variable-length `list<scalar>` /
  `list<string>`, and tuple/record values with scalar / nested UTF-8 string /
  nested `list<scalar>` / `list<string>` leaves
- at most one result
- execution routes through supported canon-lift function shapes

### 1.8 Narrow executable resource builtin support exists

Resources are no longer only parser/validator structure.

The runtime now includes:

- `WASMComponentRuntimeResourceState`
- resource type scanning for local/imported/alias resource types
- canonical resource-type bookkeeping
- owned handle tables
- owned-handle creation/drop helpers
- executable `canon resource.new` / `canon resource.drop` / `canon resource.rep`
  when a child core module imports those builtins
- the first proven operational subset for those builtins:
  locally-defined resource types and aliases thereof, `rep i32` only, sync
  destructors on explicit `resource.drop`, no async drop, and imported
  non-alias resource types only on the narrower public/internal
  `resource.drop` path described below
- imported-resource builtin behavior is now split more honestly:
  `canon resource.new` / `canon resource.rep` on imported resource types stay
  explicitly rejected, while imported `canon resource.drop` now has a narrow
  executable contract for owned and borrowed imported handles when the host
  binds a top-level imported resource type with a destructor callback
- host component callbacks can now synchronously accept and return
  `own<resource>` values through the public component value API for the
  already-existing handle subset, with runtime transfer/restore semantics for
  handles coming from the current call
- deinstantiate-time destructor execution for still-live handles in that same
  currently supported local subset
- finalizer cleanup during deinstantiation

This is now a real but narrow operational slice, not yet full resource semantics.

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
- direct child-core execution of the narrow `canon resource.new` /
  `canon resource.rep` / `canon resource.drop` subset, including sync
  destructor execution on explicit drops
- deinstantiate-time sync destructor execution for still-live handles in that
  same tested subset

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
  `list<scalar>` / `list<string>` leaves on the mixed composite param/result
  paths
  - explicit rejection of lower-side canon options and outer synthetic-lift
    canon options on this synthetic path
- direct core-wasm invocation of lowered component functions for the currently
  tested direct subset:
  - scalar parameters/results
  - top-level tuple/record parameters over the tested scalar / UTF-8 string /
    nested variable-length `list<scalar>` / `list<string>` leaf subset with
    scalar results
  - top-level tuple/record results over that same supported leaf subset through
    the tested `(param i32) -> ()` return-area path, including pure-scalar
    tuple/record results
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
    and mixed composite-result witnesses with nested `list<scalar>` /
    `list<string>` leaves, plus pure-scalar tuple/record results through the
    return-area path
  - nested child-core lowered calls into host-imported component functions whose
    multi-result vectors stay within the tested scalar + UTF-8 string, scalar +
    `list<scalar>`, or
    `record{s32, string, list<scalar>} + s32` subset on the retptr-backed
    result-area path
  - tested cross-component scalar, UTF-8 string, `list<string>`-parameter /
    `list<string>`-result, `list<u8>`-parameter / `list<u8>`-result, and
    `list<s32>`-parameter / `list<s32>`-result seams plus tested
    cross-component mixed composite-param seams and mixed composite-result seams
    (including nested `list<scalar>` / `list<string>` leaves on both the param
    and result paths), where a nested child-core lowered import targets a
    function exported from another component instance
  - no lower-side canon options beyond tested `(string-utf8)` / `(memory ...)`
    for the direct string, direct `list<string>`, and direct tuple/record
    `list<scalar>` / `list<string>`-leaf parameter/result paths, and
    `(memory ...)` for the `list<scalar>` parameter/result path
  - positive top-level and nested direct child-core witnesses plus explicit
    failures for invalid UTF-8 input, omitted lower-side memory, and malformed
    result areas on the tested memory-backed paths
- top-level host-defined component-function imports for the same supported subset,
  including the tested lowered non-scalar multi-result host-import seams above

Major Canonical ABI gaps remain:

- no general executable `canon lower`; the only supported executable lowering
  paths are:
  - the narrow direct core-call subset above
  - the synthetic scalar / UTF-8 string / `list<scalar>`-parameter /
    `list<scalar>`-result / tuple/record-parameter / record-result /
    tuple/mixed-composite-result `lift(lower(f))` subset above, including
    nested `list<scalar>` / `list<string>` leaves on the mixed composite
    param/result paths
- no general adapter/lowering path for imported component functions beyond the
  supported host-callback subset, the tested cross-component scalar / UTF-8
  string / `list<string>`-parameter / `list<string>`-result /
  `list<u8>`-parameter / `list<u8>`-result / `list<s32>`-parameter /
  `list<s32>`-result / mixed composite-param / mixed composite-result seams
  above (including nested `list<scalar>` / `list<string>` leaves on the mixed
  composite param and result paths), and the narrow direct core-call subset
  above
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
  top-level and nested return-area path over the current scalar / UTF-8 string /
  nested variable-length `list<scalar>` / `list<string>` leaf subset, including
  the newer pure-scalar tuple/record result path
- no executable lower path yet for broader nested lowered-consumer coverage beyond
  the tested nested child-core UTF-8 string / `list<string>`-parameter /
  `list<string>`-result / `list<u8>`- and `list<s32>`-parameter /
  `list<scalar>`-result / mixed composite-param / mixed composite-result paths
  (including nested `list<scalar>` / `list<string>` leaves on the mixed
  composite-param path) and the tested scalar / UTF-8 string / nested
  `list<scalar>` tuple-record subset
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
  members for the current scalar / UTF-8 string / variable-length `list<scalar>` /
  `list<string>` / tuple-record leaf subset; typed `instance` import matching is otherwise
  limited to exported `core module`, exported `resource type` members with the
  current abstract-`type` / runtime-eq-bound subset, scalar /
  variable-length `list<scalar>` /
  `list<string>` / current tuple-record-subset `value`, nested `instance`
  members, and the first typed `component` subset: top-level component imports
  plus typed `instance` component members whose expected component types have no
  imports and only recurse through typed `component` exports with explicit
  component type metadata plus the current metadata-only eq-bound resource
  subset; broader pure componenttype identity/rebinding and runtime resource
  rebinding are still unsupported
- host-import tuple/record values are still limited to the current scalar /
  UTF-8 string / nested `list<scalar>` / `list<string>` subset
- public resource-type exports are now enumerable through
  `wasm_runtime_get_component_export_type(...)` /
  `wasm_component_instance_get_export_type(...)`, but the broader public
  resource import/export contract is still incomplete compared to the current
  function/value/instance/component/core-module surface

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
- materializing multiple start results when the start function is a
  canon-lifted scalar-only multi-result function using direct core multi-value
  returns
- materializing multiple start results when the start function is a
  canon-lifted multi-result function using the retptr-backed Canonical ABI
  result-area shape for the current supported scalar / UTF-8 string /
  `list<scalar>` / `list<string>` / tuple-record-leaf result-vector subset,
  plus the tested direct local-resource `own<resource> + s32` /
  `borrow<resource> + s32` subset
- materializing multiple start results when the start function is a
  host-imported multi-result component function whose results stay within the
  tested scalar-only or non-scalar `string + s32` / `list<scalar> + s32` /
  `record{s32, string, list<scalar>} + s32` subset
- parsing and validating multi-result component functypes, including start
  sections whose declared result count matches a multi-result functype

Still missing:

- broader start-section execution for non-scalar multi-result host-imported
  component functions beyond the current tested `string + s32` /
  `list<scalar> + s32` / `record{s32, string, list<scalar>} + s32` subset
- host-import multi-result execution for broader resource-result seams beyond
  the current tested direct callback and direct lowered
  `own<resource> + s32` / `borrow<resource> + s32` subset plus the tested
  imported `own<resource> + string` lowered seam
- start-section execution for retptr/canon-lifted resource multi-result
  functions beyond the current tested local/imported
  `own<resource> + s32` / `borrow<resource> + s32` subset plus the tested
  imported `own<resource> + string` start seam
- retptr-backed Canonical ABI multi-results beyond the current scalar /
  UTF-8 string / `list<scalar>` / `list<string>` / tuple-record-leaf
  result-vector subset plus the current direct top-level local/imported
  `own<resource> + s32` / `borrow<resource> + s32` public-call subset and the
  tested direct local `own<resource> + string` witness
- Canonical ABI beyond the current supported public-value subset
- the more complete execution space needed for start-heavy real-world components

## 7. Resources now have a narrow operational slice, not full semantics

Resource support is no longer purely structural, but the operational model is still incomplete.

What exists:

- resource type bookkeeping
- alias/import tracking
- owned handle allocation/drop helpers
- executable child-core `canon resource.new` / `canon resource.rep` /
  `canon resource.drop` for the tested locally-defined `rep i32` subset and
  aliases thereof
- lifted top-level and nested-exported `own<resource>` results through
  `wasm_runtime_call_component_values(...)` for that same local-resource subset
- host-side destruction of those lifted owned results through
  `wasm_runtime_drop_component_owned_result(...)`, including the proven
  top-level sync-destructor path and nested exported-result drops for the
  current local-resource subset
- sync destructor execution when that tested subset is dropped explicitly from
  child core code
- deinstantiate-time sync destructor execution for still-live handles in that
  same currently supported subset
- deinstantiate-time cleanup/finalization
- explicit rejection of imported `resource.new` / `resource.rep`
- an executable imported `resource.drop` seam for imported resource handles,
  including:
  - trap-on-own-drop when no host destructor is bound
  - destructor callback execution for owned imported handles
  - borrowed imported-handle drop without destructor execution
  - deinstantiate-time imported-handle destructor cleanup when a callback is bound
- top-level public host binding of imported resource types through
  `wasm_component_import_binding_t`, for the same imported `resource.drop`
  subset
- top-level and nested public resource-type export lookup through
  `wasm_runtime_get_component_export_type(...)` and
  `wasm_component_instance_get_export_type(...)`
- typed top-level `instance` and `component` import matching for exported
  resource-type members with abstract `type` bounds
- recursive component-type matching for nested component exports whose own
  component types export resource-type members with abstract `type` bounds
- public `wasm_runtime_call_component_values(...)` lowering of existing
  `own<resource>` arguments into exported canon-lifted component functions for
  the currently supported local-resource subset
- synchronous host callback round-tripping of existing `own<resource>` handles
  through `wasm_runtime_call_component_values(...)` for host function imports
- synchronous host callback multi-result transport for the current tested
  direct host-import subset:
  - `own<resource> + s32` result vectors that round-trip an existing current-call
    owned handle for the current local-resource subset
  - fresh imported `own<resource> + s32` result vectors that mint a new owned
    imported handle through
    `wasm_component_value_init_owned_imported_resource_result(...)`
  - `borrow<resource> + s32` result vectors that return a borrowed alias of the
    current borrowed argument for the current local-resource subset
  - direct lowered child-core execution of the tested imported
    `own<resource> + s32` host-import seam on the retptr/result-area path
  - direct lowered child-core execution of the tested imported
    `own<resource> + string` host-import seam on that same retptr/result-area
    path, including rollback when the string lane is invalid
  - rollback/restoration when a later result lane fails after an earlier local
    owned-resource round-trip lane succeeded
- retptr-backed canon-lift multi-result transport for the current tested direct
  top-level exported-function/start subset:
  - direct canon-lift `own<resource> + s32` result vectors that round-trip an
    existing live owned handle for the current local/imported-resource subset
  - direct canon-lift `borrow<resource> + s32` result vectors that return a
    borrowed alias of the current borrowed argument for the current
    local/imported-resource subset
  - direct canon-lift `own<resource> + string` result vectors for the current
    local/imported-resource subset
  - top-level start-section materialization of direct canon-lift local/imported
    `own<resource> + s32` / `borrow<resource> + s32` result vectors plus the
    tested imported `own<resource> + string` subset
- synchronous host callback transport of borrowed resource parameters for host
  function imports, including both local owned handles and imported handles
  without consuming ownership
- scalar borrowed resource results that alias current borrowed arguments through
  both direct top-level host callbacks and canon-lowered/relifted callback
  paths, for both local and imported handles, including:
  - public construction of those borrowed callback results through
    `wasm_component_value_init_borrowed_resource_result(...)`
  - identity-based matching of those returned borrows against the current call's
    tracked borrowed arguments instead of requiring raw public-value pointer
    reuse
  - balanced borrowed-handle cleanup for those direct/relifted scalar callback
    result paths
- fresh host-created **imported** `own<resource>` results from host callbacks,
  including:
  - `wasm_component_value_init_owned_imported_resource_result(...)` for pending
    imported-resource result tokens
  - promotion of those tokens into owned imported handles when the callback's
    expected result type is `own<imported-resource>`
  - dropping those promoted results through the existing
    `wasm_runtime_drop_component_owned_result(...)` helper
  - finalizer-driven cleanup when result validation fails before the pending
    token is promoted
  - the current direct host-import multi-result `own<resource> + s32` subset

What is still missing:

- full ownership/borrow/lend semantics
- live Canonical ABI resource lowering/lifting
- resource imports/exports as a complete **public** host feature beyond the
   current top-level imported-resource-type binding, public resource-type export
   lookup, the current typed resource-member matching subset (top-level
   `instance`/`component` imports plus nested component-type matching with the
   current abstract-`type` / metadata-eq-bound subset), and host-callback
   imported own-result subset
- resource-type identity/rebinding beyond the current name/sort plus the
  current abstract-`type` / metadata-eq-bound / runtime-eq-bound matching
  subset is still unsupported
- borrowed resource values and the rest of general own/borrow public value
  transport beyond the current supported subset (host-callback borrowed
  parameters, host-callback round-tripping of existing owned handles, public
  call lowering of existing live owned handles into exported canon-lifted
  `own<resource>` and `borrow<resource>` parameters, guest-side local
  `resource.rep` / borrowed `resource.drop` on those temporary borrowed handles,
  exported borrow results that alias current borrowed parameters, direct
  top-level and canon-lowered callback borrow results that alias current
  borrowed arguments for the current scalar local/imported-resource subset,
  including public construction through
  `wasm_component_value_init_borrowed_resource_result(...)`,
  plus fresh imported own results)
- runtime enforcement of richer resource lifecycle rules beyond the current
  outstanding-borrow subset for local and imported owned handles
- no full lend-count or borrow-scope enforcement yet: the current supported
  local/imported resource subset now rejects owned drops/transfers while tracked
  borrowed aliases are still live, but broader outstanding-borrow tracking and
  lifetime enforcement still remain incomplete
- public resource-aware callable component APIs beyond the new
  owned-result drop helper, borrowed-result helper, top-level
  imported-resource binding, and the current owned-resource /
  borrowed-parameter / scalar borrowed-result call subset plus the current
  direct host-import/lowered `own<resource> + s32` /
  `borrow<resource> + s32` multi-result subset plus the tested imported
  lowered `own<resource> + string` seam, the current direct top-level
  retptr/canon-lift local/imported `own<resource> + s32` /
  `borrow<resource> + s32` subset, the tested local/imported
  `tuple<own<resource>> + s32` composite retptr witness, the tested local/imported
  `own<resource> + string` retptr witness, and top-level start materialization
  of the local/imported retptr `own<resource> + s32` /
  `borrow<resource> + s32` subset plus the tested imported
  `own<resource> + string` start seam; broader borrow/lend behavior, broader
  mixed/composite resource retptr vectors beyond the tested
  `tuple<own<resource>>` local/imported witness pair, composite/non-scalar borrow-result
  flows, and stricter caller-side ownership/consumption semantics still remain
  open
- full trap/failure-path operational cleanup semantics

So resources now have a narrow executable seam, not a finished runtime.

## 8. Nested core runtime support is still incomplete

This remains one of the clearest hard gaps.

Nested components now support:

- nested local `core module` sections
- export/re-export of those nested core modules through nested component instances
- nested local `core instance` sections in instantiate/`with_args` form
- nested inline/`without_args` `core instance` expressions for core-module
  re-exports
- nested `alias core export` for `func`
- nested `alias core export` for `memory`, including re-export through a nested
  inline core instance
- nested `core type` sections as tolerated structural metadata around those
  flows
- typed nested `core module` imports that use those `core type` entries for
  module import/export matching

Nested components still reject:

- nested `alias core export` for broader still-unwired core sorts such as
  `table` / `global`
- nested `alias outer` for core runtime refs
- broader operational use of nested `core type` entries beyond the current
  typed core-module import matching subset

The runtime can now thread nested local core-module handles and construct nested
local/synthetic core instances, including the current nested core-function and
core-memory alias/re-export subset, but it still does **not** construct a full
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

> **A real but still partial component runtime: public host APIs, scalar / UTF-8 string / `list<scalar>` / limited tuple-record canon-lift calls, a narrow executable direct-core-call `canon lower` subset, supported host-provided component-function imports, runtime values, value imports/exports, start execution slices, a narrow scalar local/imported resource-call subset (including borrowed callback results), and a limited nested core-runtime subset for local core modules/instances plus nested core `func`/`memory` aliases are implemented; full support is still blocked on broader canon-lower/imported-function lowering, broader composite Canonical ABI and value semantics, operational resources, remaining host API gaps, and the rest of nested core-runtime support.**
