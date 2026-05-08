/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef WASM_COMPONENT_RUNTIME_H
#define WASM_COMPONENT_RUNTIME_H

#include "wasm_component.h"

typedef enum WASMComponentCoreRuntimeRefType {
    WASM_COMP_CORE_RUNTIME_REF_FUNC = 0,
    WASM_COMP_CORE_RUNTIME_REF_TABLE,
    WASM_COMP_CORE_RUNTIME_REF_MEMORY,
    WASM_COMP_CORE_RUNTIME_REF_GLOBAL,
    WASM_COMP_CORE_RUNTIME_REF_MODULE,
    WASM_COMP_CORE_RUNTIME_REF_INSTANCE
} WASMComponentCoreRuntimeRefType;

struct WASMComponentCoreRuntimeInstance;

typedef struct WASMComponentCoreRuntimeRef {
    WASMComponentCoreRuntimeRefType type;
    union {
        wasm_function_inst_t function;
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
    uint32 export_count;
    WASMComponentCoreNamedExport *exports;
} WASMComponentCoreRuntimeInstance;

typedef struct WASMComponentResolvedAlias {
    const char *name;
    WASMComponentCoreRuntimeRef ref;
} WASMComponentResolvedAlias;

typedef enum WASMComponentRuntimeFuncKind {
    WASM_COMP_RUNTIME_FUNC_LIFT = 0,
    WASM_COMP_RUNTIME_FUNC_UNSUPPORTED_CANON
} WASMComponentRuntimeFuncKind;

typedef struct WASMComponentRuntimeFunc {
    WASMComponentRuntimeFuncKind kind;
    WASMComponentCanonType canon_tag;
    uint32 type_idx;
    WASMComponentCanonOpts *canon_opts;
    WASMComponentCoreRuntimeRef core_func_ref;
} WASMComponentRuntimeFunc;

struct WASMComponentRuntimeScope;

typedef struct WASMComponentRuntimeComponent {
    WASMComponent *component;
    struct WASMComponentRuntimeScope *scope;
    bool owns_scope;
} WASMComponentRuntimeComponent;

typedef enum WASMComponentRuntimeRefType {
    WASM_COMP_RUNTIME_REF_FUNC = 0,
    WASM_COMP_RUNTIME_REF_INSTANCE,
    WASM_COMP_RUNTIME_REF_COMPONENT
} WASMComponentRuntimeRefType;

struct WASMComponentRuntimeInstance;

typedef struct WASMComponentRuntimeRef {
    WASMComponentRuntimeRefType type;
    union {
        WASMComponentRuntimeFunc *function;
        struct WASMComponentRuntimeInstance *instance;
        WASMComponentRuntimeComponent *component;
    } of;
} WASMComponentRuntimeRef;

typedef struct WASMComponentNamedExport {
    const char *name;
    WASMComponentRuntimeRef ref;
} WASMComponentNamedExport;

typedef struct WASMComponentRuntimeInstance {
    bool owns_exports;
    uint32 export_count;
    WASMComponentNamedExport *exports;
    uint32 owned_instance_count;
    struct WASMComponentRuntimeInstance *owned_instances;
    uint32 owned_component_count;
    WASMComponentRuntimeComponent *owned_components;
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
    uint32 component_instance_count;
    WASMComponentRuntimeInstance *component_instances;
    uint32 component_export_count;
    WASMComponentNamedExport *component_exports;
};

WASMComponentModule *
wasm_component_module_load(uint8 *buf, uint32 size, const LoadArgs *args,
                           char *error_buf, uint32 error_buf_size);

void
wasm_component_module_unload(WASMComponentModule *module);

WASMComponentInstance *
wasm_component_module_instantiate(WASMComponentModule *module,
                                  char *error_buf, uint32 error_buf_size);

void
wasm_component_module_deinstantiate(WASMComponentInstance *inst);

#endif
