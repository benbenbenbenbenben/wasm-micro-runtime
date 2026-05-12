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

struct WASMComponentInstance;
typedef struct WASMComponentRuntimeResourceState WASMComponentRuntimeResourceState;

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
    uint32 generation;
    uint32 borrow_count;
    uint32 borrowed_from_handle;
    uint32 borrowed_from_generation;
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
    const char *import_name;
    const WASMComponentTypes *declared_type;
    bool has_dtor;
    uint32 dtor_func_idx;
    bool has_callback;
    uint32 callback_func_idx;
    wasm_component_imported_resource_drop_callback_t imported_drop_callback;
    void *imported_drop_user_data;
    WASMComponentResourceHandleTable handle_table;
} WASMComponentRuntimeResourceType;

typedef struct WASMComponentOwnedResourceHandle {
    uint32 type_idx;
    uint32 handle;
} WASMComponentOwnedResourceHandle;

#define WASM_COMPONENT_PUBLIC_RESOURCE_VALUE_MAGIC UINT32_C(0x43524d56)
#define WASM_COMPONENT_PUBLIC_COMPOSITE_RESOURCE_VALUE_MAGIC UINT32_C(0x43524d43)

typedef enum WASMComponentPublicResourceValueKind {
    WASM_COMPONENT_PUBLIC_RESOURCE_VALUE_TRANSFERRED = 0,
    WASM_COMPONENT_PUBLIC_RESOURCE_VALUE_BORROWED,
    WASM_COMPONENT_PUBLIC_RESOURCE_VALUE_PENDING_IMPORTED_RESULT
} WASMComponentPublicResourceValueKind;

typedef struct WASMComponentPublicResourceValue {
    uint32 magic;
    WASMComponentPublicResourceValueKind kind;
    WASMComponentRuntimeResourceState *resource_state;
    uint32 resource_type_idx;
    uint32 canonical_type_idx;
    uint32 handle;
    void *data;
    WASMComponentResourceHandleFinalizer finalizer;
    void *finalizer_ctx;
} WASMComponentPublicResourceValue;

typedef struct WASMComponentPublicCompositeResourceValue {
    uint32 magic;
    uint32 byte_size;
    uint32 resource_count;
    uint8 *data;
    WASMComponentPublicResourceValue *resource_values;
} WASMComponentPublicCompositeResourceValue;

typedef struct WASMComponentRuntimeResourceState {
    const WASMComponent *component;
    struct WASMComponentInstance *owner_instance;
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

bool
wasm_component_resource_bind_imported_drop_callback(
    WASMComponentRuntimeResourceState *resource_state, uint32 type_idx,
    wasm_component_imported_resource_drop_callback_t callback, void *user_data,
    char *error_buf, uint32 error_buf_size);

bool
wasm_component_resource_create_imported_handle(
    WASMComponentRuntimeResourceState *resource_state, uint32 type_idx,
    void *data, bool owned, uint32 *out_handle, char *error_buf,
    uint32 error_buf_size);

bool
wasm_component_resource_take_owned_handle(
    WASMComponentRuntimeResourceState *resource_state, uint32 type_idx,
    uint32 handle, WASMComponentPublicResourceValue *resource_value_out,
    char *error_buf, uint32 error_buf_size);

bool
wasm_component_resource_restore_owned_handle(
    WASMComponentPublicResourceValue *resource_value, char *error_buf,
    uint32 error_buf_size);

bool
wasm_component_resource_borrow_handle(
    WASMComponentRuntimeResourceState *resource_state, uint32 type_idx,
    uint32 handle, WASMComponentPublicResourceValue *resource_value_out,
    char *error_buf, uint32 error_buf_size);

bool
wasm_component_resource_get_borrowed_owner(
    const WASMComponentPublicResourceValue *resource_value,
    WASMComponentRuntimeResourceState **owner_resource_state_out,
    uint32 *owner_resource_type_idx_out, uint32 *owner_handle_out,
    char *error_buf, uint32 error_buf_size);

bool
wasm_component_resource_clone_borrowed_value(
    const WASMComponentPublicResourceValue *source_value,
    WASMComponentPublicResourceValue *resource_value_out, char *error_buf,
    uint32 error_buf_size);

bool
wasm_component_resource_create_borrowed_handle(
    WASMComponentRuntimeResourceState *resource_state, uint32 type_idx,
    uint32 source_handle, uint32 *out_handle, char *error_buf,
    uint32 error_buf_size);

bool
wasm_component_resource_release_borrowed_handle(
    WASMComponentRuntimeResourceState *resource_state, uint32 type_idx,
    uint32 handle, char *error_buf, uint32 error_buf_size);

void
wasm_component_resource_release_public_value(
    WASMComponentPublicResourceValue *resource_value);

void
wasm_component_drop_transferred_public_resource_value(
    WASMComponentPublicResourceValue *resource_value);

#ifdef __cplusplus
}
#endif

#endif
