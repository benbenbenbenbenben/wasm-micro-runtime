/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef WASM_COMPONENT_RESOURCE_H
#define WASM_COMPONENT_RESOURCE_H

#include "wasm_component.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WASM_COMPONENT_RESOURCE_INVALID_TYPE_IDX UINT32_MAX

typedef void (*WASMComponentResourceHandleFinalizer)(void *data, void *ctx);

typedef enum WASMComponentRuntimeResourceTypeKind {
    WASM_COMP_RUNTIME_RESOURCE_TYPE_NONE = 0,
    WASM_COMP_RUNTIME_RESOURCE_TYPE_LOCAL,
    WASM_COMP_RUNTIME_RESOURCE_TYPE_IMPORTED,
    WASM_COMP_RUNTIME_RESOURCE_TYPE_ALIAS
} WASMComponentRuntimeResourceTypeKind;

typedef struct WASMComponentResourceHandleEntry {
    bool is_live;
    bool is_owned;
    uint32 handle;
    void *data;
    WASMComponentResourceHandleFinalizer finalizer;
    void *finalizer_ctx;
} WASMComponentResourceHandleEntry;

typedef struct WASMComponentResourceHandleTable {
    uint32 entry_count;
    uint32 live_handle_count;
    uint32 owned_handle_count;
    uint32 next_handle;
    WASMComponentResourceHandleEntry *entries;
} WASMComponentResourceHandleTable;

typedef struct WASMComponentRuntimeResourceType {
    WASMComponentRuntimeResourceTypeKind kind;
    uint32 type_idx;
    uint32 canonical_type_idx;
    uint32 source_type_idx;
    const WASMComponentTypes *declared_type;
    bool has_dtor;
    uint32 dtor_func_idx;
    bool has_callback;
    uint32 callback_func_idx;
    WASMComponentResourceHandleTable handle_table;
} WASMComponentRuntimeResourceType;

typedef struct WASMComponentOwnedResourceHandle {
    uint32 type_idx;
    uint32 handle;
} WASMComponentOwnedResourceHandle;

typedef struct WASMComponentRuntimeResourceState {
    const WASMComponent *component;
    uint32 type_count;
    uint32 type_capacity;
    WASMComponentRuntimeResourceType *types;
    uint32 owned_handle_count;
    uint32 owned_handle_capacity;
    WASMComponentOwnedResourceHandle *owned_handles;
} WASMComponentRuntimeResourceState;

WASMComponentRuntimeResourceState *
wasm_component_resource_state_create(const WASMComponent *component,
                                     char *error_buf,
                                     uint32 error_buf_size);

void
wasm_component_resource_state_destroy(
    WASMComponentRuntimeResourceState *resource_state);

WASMComponentRuntimeResourceType *
wasm_component_resource_lookup_runtime_type(
    WASMComponentRuntimeResourceState *resource_state, uint32 type_idx);

const WASMComponentRuntimeResourceType *
wasm_component_resource_lookup_runtime_type_const(
    const WASMComponentRuntimeResourceState *resource_state, uint32 type_idx);

bool
wasm_component_resource_create_owned_handle(
    WASMComponentRuntimeResourceState *resource_state, uint32 type_idx,
    void *data, WASMComponentResourceHandleFinalizer finalizer,
    void *finalizer_ctx, uint32 *out_handle, char *error_buf,
    uint32 error_buf_size);

bool
wasm_component_resource_drop_owned_handle(
    WASMComponentRuntimeResourceState *resource_state, uint32 type_idx,
    uint32 handle, char *error_buf, uint32 error_buf_size);

#ifdef __cplusplus
}
#endif

#endif
