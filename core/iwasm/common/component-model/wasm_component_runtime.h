/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef WASM_COMPONENT_RUNTIME_H
#define WASM_COMPONENT_RUNTIME_H

#include "wasm_runtime_common.h"
#include "wasm_component.h"
#include "wasm_component_resource.h"
#include "wasm_component_value.h"

struct InstantiationArgs2;
struct WASMModule;
struct WASMImport;

typedef enum WASMComponentCoreRuntimeRefType {
    WASM_COMP_CORE_RUNTIME_REF_FUNC = 0,
    WASM_COMP_CORE_RUNTIME_REF_LOWERED_FUNC,
    WASM_COMP_CORE_RUNTIME_REF_TABLE,
    WASM_COMP_CORE_RUNTIME_REF_MEMORY,
    WASM_COMP_CORE_RUNTIME_REF_GLOBAL,
    WASM_COMP_CORE_RUNTIME_REF_MODULE,
    WASM_COMP_CORE_RUNTIME_REF_INSTANCE
} WASMComponentCoreRuntimeRefType;

struct WASMComponentCoreRuntimeInstance;
struct WASMComponentRuntimeFunc;

typedef struct WASMComponentCoreRuntimeRef {
    WASMComponentCoreRuntimeRefType type;
    struct WASMComponentCoreRuntimeInstance *owner_instance;
    uint32 func_type_idx;
    union {
        wasm_function_inst_t function;
        struct WASMComponentRuntimeFunc *lowered_function;
        wasm_table_inst_t table;
        wasm_memory_inst_t memory;
        wasm_global_inst_t global;
        wasm_module_t module;
        struct WASMComponentCoreRuntimeInstance *instance;
    } of;
} WASMComponentCoreRuntimeRef;

typedef struct WASMComponentCoreNamedExport {
    const char *name;
    WASMComponentCoreRuntimeRef ref;
} WASMComponentCoreNamedExport;

typedef struct WASMComponentCoreRuntimeInstance {
    wasm_module_inst_t module_inst;
    void *patched_import_attachments;
    uint32 patched_import_count;
    uint32 export_count;
    WASMComponentCoreNamedExport *exports;
} WASMComponentCoreRuntimeInstance;

typedef struct WASMComponentResolvedAlias {
    const char *name;
    WASMComponentCoreRuntimeRef ref;
} WASMComponentResolvedAlias;

typedef enum WASMComponentRuntimeFuncKind {
    WASM_COMP_RUNTIME_FUNC_LIFT = 0,
    WASM_COMP_RUNTIME_FUNC_HOST_IMPORT,
    WASM_COMP_RUNTIME_FUNC_LOWER,
    WASM_COMP_RUNTIME_FUNC_RESOURCE_BUILTIN,
    WASM_COMP_RUNTIME_FUNC_UNSUPPORTED_CANON
} WASMComponentRuntimeFuncKind;

typedef enum WASMComponentRuntimeStringEncoding {
    WASM_COMP_RUNTIME_STRING_ENCODING_NONE = 0,
    WASM_COMP_RUNTIME_STRING_ENCODING_UTF8,
    WASM_COMP_RUNTIME_STRING_ENCODING_UTF16,
    WASM_COMP_RUNTIME_STRING_ENCODING_LATIN1_UTF16
} WASMComponentRuntimeStringEncoding;

typedef enum WASMComponentRuntimeCanonLiftMemoryResultKind {
    WASM_COMP_RUNTIME_CANON_LIFT_MEMORY_RESULT_NONE = 0,
    WASM_COMP_RUNTIME_CANON_LIFT_MEMORY_RESULT_STRING,
    WASM_COMP_RUNTIME_CANON_LIFT_MEMORY_RESULT_LIST_SCALAR,
    WASM_COMP_RUNTIME_CANON_LIFT_MEMORY_RESULT_COMPOSITE,
    WASM_COMP_RUNTIME_CANON_LIFT_MEMORY_RESULT_RETPTR_VECTOR
} WASMComponentRuntimeCanonLiftMemoryResultKind;

typedef struct WASMComponentRuntimeFunc {
    WASMComponentRuntimeFuncKind kind;
    WASMComponentCanonType canon_tag;
    uint32 type_idx;
    uint32 resource_type_idx;
    struct WASMComponentInstance *owner_instance;
    WASMComponentRuntimeResourceState *resource_state;
    const WASMComponent *type_owner_component;
    struct WASMComponentRuntimeFunc *lowered_target;
    WASMComponentCanonOpts *canon_opts;
    WASMComponentCoreRuntimeRef core_func_ref;
    WASMComponentCoreRuntimeRef canon_memory_ref;
    WASMComponentCoreRuntimeRef canon_realloc_ref;
    WASMComponentCoreRuntimeRef canon_post_return_ref;
    WASMComponentRuntimeStringEncoding string_encoding;
    WASMComponentRuntimeCanonLiftMemoryResultKind memory_result_kind;
    bool has_string_params;
    bool has_list_scalar_params;
    bool has_composite_params;
    bool has_owned_resource_params;
    bool has_borrowed_resource_params;
    bool has_string_result;
    bool has_list_scalar_result;
    bool has_composite_result;
    bool has_owned_resource_result;
    bool has_borrowed_resource_result;
    bool is_top_level_export;
    bool is_async;
    uint32 callback_func_idx;
    wasm_component_host_func_callback_t host_callback;
    void *host_user_data;
} WASMComponentRuntimeFunc;

struct WASMComponentRuntimeScope;

typedef struct WASMComponentRuntimeComponent {
    WASMComponent *component;
    struct WASMComponentRuntimeScope *scope;
    bool owns_scope;
} WASMComponentRuntimeComponent;

typedef enum WASMComponentRuntimeRefType {
    WASM_COMP_RUNTIME_REF_FUNC = 0,
    WASM_COMP_RUNTIME_REF_VALUE,
    WASM_COMP_RUNTIME_REF_INSTANCE,
    WASM_COMP_RUNTIME_REF_COMPONENT,
    WASM_COMP_RUNTIME_REF_CORE_MODULE,
    WASM_COMP_RUNTIME_REF_RESOURCE_TYPE
} WASMComponentRuntimeRefType;

struct WASMComponentRuntimeInstance;

typedef struct WASMComponentRuntimeRef {
    WASMComponentRuntimeRefType type;
    union {
        WASMComponentRuntimeFunc *function;
        WASMComponentRuntimeValue *value;
        struct WASMComponentRuntimeInstance *instance;
        WASMComponentRuntimeComponent *component;
        wasm_module_t core_module;
        const struct WASMComponentRuntimeResourceType *resource_type;
    } of;
} WASMComponentRuntimeRef;

typedef struct WASMComponentNamedExport {
    const char *name;
    WASMComponentRuntimeRef ref;
} WASMComponentNamedExport;

typedef struct WASMComponentRuntimeInstance {
    bool owns_exports;
    bool owns_resource_state;
    uint32 export_count;
    WASMComponentNamedExport *exports;
    WASMComponentRuntimeResourceState *resource_state;
    uint32 owned_func_count;
    struct WASMComponentRuntimeFunc *owned_funcs;
    uint32 owned_value_count;
    WASMComponentRuntimeValue *owned_values;
    uint32 owned_core_instance_count;
    WASMComponentCoreRuntimeInstance *owned_core_instances;
    uint32 owned_lowered_func_count;
    struct WASMComponentRuntimeFunc *owned_lowered_funcs;
    uint32 owned_instance_count;
    struct WASMComponentRuntimeInstance *owned_instances;
    uint32 owned_component_count;
    WASMComponentRuntimeComponent *owned_components;
    uint32 core_type_count;
    WASMComponentCoreType *core_types;
} WASMComponentRuntimeInstance;

typedef struct WASMComponentModule {
    uint32 module_type;
    WASMComponent component;
    uint8 *binary;
    uint32 binary_size;
    bool is_binary_freeable;
} WASMComponentModule;

struct WASMComponentInstance {
    uint32 module_type;
    WASMComponentModule *module;
    char cur_exception[128];
    uint32 core_module_count;
    wasm_module_t *core_modules;
    uint32 core_instance_count;
    WASMComponentCoreRuntimeInstance *core_instances;
    uint32 core_func_count;
    WASMComponentCoreRuntimeRef *core_funcs;
    uint32 lowered_func_count;
    WASMComponentRuntimeFunc *lowered_funcs;
    uint32 core_table_count;
    WASMComponentCoreRuntimeRef *core_tables;
    uint32 core_memory_count;
    WASMComponentCoreRuntimeRef *core_memories;
    uint32 core_global_count;
    WASMComponentCoreRuntimeRef *core_globals;
    uint32 resolved_alias_count;
    WASMComponentResolvedAlias *resolved_aliases;
    uint32 component_count;
    WASMComponentRuntimeComponent *components;
    uint32 component_func_count;
    WASMComponentRuntimeFunc *component_funcs;
    uint32 component_value_count;
    WASMComponentRuntimeValue *component_values;
    uint32 component_instance_count;
    WASMComponentRuntimeInstance *component_instances;
    uint32 component_export_count;
    WASMComponentNamedExport *component_exports;
    WASMComponentRuntimeResourceState *resource_state;
    struct WASMComponentAsyncEngine *async_engine;
    uint32 core_type_count;
    WASMComponentCoreType *core_types;
};

WASMComponentModule *
wasm_component_module_load(uint8 *buf, uint32 size, const LoadArgs *args,
                           char *error_buf, uint32 error_buf_size);

void
wasm_component_module_unload(WASMComponentModule *module);

WASMComponentInstance *
wasm_component_module_instantiate(WASMComponentModule *module,
                                  const struct InstantiationArgs2 *args,
                                  char *error_buf, uint32 error_buf_size);

int32
wasm_component_get_export_count(const WASMComponentInstance *inst);

bool
wasm_component_get_export_type(const WASMComponentInstance *inst,
                               int32 export_index,
                               wasm_component_export_t *export_type);

bool
wasm_component_get_export_value(const WASMComponentInstance *inst,
                                int32 export_index,
                                wasm_component_value_t *value,
                                char *error_buf, uint32 error_buf_size);

WASMComponentRuntimeFunc *
wasm_component_lookup_function(const WASMComponentInstance *inst,
                               const char *name);

bool
wasm_component_func_get_generic_signature(
    const WASMComponentInstance *inst,
    const WASMComponentRuntimeFunc *function, uint32 *param_count,
    wasm_valkind_t *param_types, uint32 param_types_capacity,
    uint32 *result_count, wasm_valkind_t *result_types,
    uint32 result_types_capacity, char *error_buf, uint32 error_buf_size);

bool
wasm_component_lookup_value(const WASMComponentInstance *inst, const char *name,
                            wasm_component_value_t *value, char *error_buf,
                            uint32 error_buf_size);

WASMComponentRuntimeInstance *
wasm_component_lookup_instance(const WASMComponentInstance *inst,
                               const char *name);

WASMComponentRuntimeComponent *
wasm_component_lookup_component(const WASMComponentInstance *inst,
                                const char *name);

wasm_module_t
wasm_component_lookup_core_module(const WASMComponentInstance *inst,
                                  const char *name);

bool
wasm_component_call(WASMComponentInstance *inst,
                    const WASMComponentRuntimeFunc *function,
                    uint32 num_results, wasm_val_t *results,
                    uint32 num_args, wasm_val_t *args);

bool
wasm_component_call_values(WASMComponentInstance *inst,
                           const WASMComponentRuntimeFunc *function,
                           uint32 num_results,
                           wasm_component_value_t *results,
                           uint32 num_args,
                           const wasm_component_value_t *args);

bool
wasm_component_drop_owned_result(WASMComponentInstance *inst,
                                 const WASMComponentRuntimeFunc *function,
                                 uint32 result_index,
                                 wasm_component_value_t *value);

void
wasm_component_module_deinstantiate(WASMComponentInstance *inst);

#endif
