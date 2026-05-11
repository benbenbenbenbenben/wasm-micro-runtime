/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef WASM_COMPONENT_RESOURCE_CALL_H
#define WASM_COMPONENT_RESOURCE_CALL_H

#include "wasm_component_runtime.h"

typedef struct WASMComponentCanonOwnedResourceParamTracker {
    wasm_component_value_t **values;
    uint32 count;
    uint32 capacity;
} WASMComponentCanonOwnedResourceParamTracker;

typedef struct WASMComponentCanonBorrowedResourceParam {
    WASMComponentRuntimeResourceState *resource_state;
    uint32 resource_type_idx;
    uint32 handle;
    WASMComponentRuntimeResourceState *owner_resource_state;
    uint32 owner_resource_type_idx;
    uint32 owner_handle;
} WASMComponentCanonBorrowedResourceParam;

typedef struct WASMComponentCanonBorrowedResourceParamTracker {
    WASMComponentCanonBorrowedResourceParam *params;
    uint32 count;
    uint32 capacity;
} WASMComponentCanonBorrowedResourceParamTracker;

WASMComponentPublicResourceValue *
wasm_component_get_public_resource_value(const wasm_component_value_t *value);

void
wasm_component_clear_public_resource_value(wasm_component_value_t *value);

void
wasm_component_consume_public_resource_value(
    WASMComponentPublicResourceValue *resource_value);

bool
wasm_component_init_public_resource_value(
    WASMComponentInstance *inst,
    const WASMComponentRuntimeResourceState *resource_state, uint32 resource_type_idx,
    uint32 handle, wasm_component_value_t *value);

bool
wasm_component_init_public_borrowed_resource_value(
    WASMComponentInstance *inst,
    const WASMComponentRuntimeResourceState *resource_state, uint32 resource_type_idx,
    uint32 handle, wasm_component_value_t *value);

bool
wasm_component_promote_pending_imported_resource_result(
    WASMComponentInstance *inst,
    const WASMComponentRuntimeResourceState *resource_state, uint32 resource_type_idx,
    wasm_component_value_t *value, wasm_val_t *handle_value_out);

bool
wasm_component_restore_public_resource_value(WASMComponentInstance *inst,
                                             wasm_component_value_t *value);

bool
wasm_component_validate_public_owned_resource_handle(
    WASMComponentInstance *inst,
    const WASMComponentRuntimeResourceState *resource_state, uint32 resource_type_idx,
    uint32 handle);

bool
wasm_component_track_owned_resource_param(
    WASMComponentCanonOwnedResourceParamTracker *tracker,
    wasm_component_value_t *value);

void
wasm_component_consume_owned_resource_params(
    WASMComponentCanonOwnedResourceParamTracker *tracker);

bool
wasm_component_track_borrowed_resource_param(
    WASMComponentCanonBorrowedResourceParamTracker *tracker,
    WASMComponentRuntimeResourceState *resource_state, uint32 resource_type_idx,
    uint32 handle, WASMComponentRuntimeResourceState *owner_resource_state,
    uint32 owner_resource_type_idx, uint32 owner_handle);

WASMComponentCanonBorrowedResourceParam *
wasm_component_find_borrowed_resource_param_by_handle(
    WASMComponentCanonBorrowedResourceParamTracker *tracker, uint32 handle,
    uint32 owner_resource_type_idx);

WASMComponentCanonBorrowedResourceParam *
wasm_component_find_matching_borrowed_resource_param(
    WASMComponentCanonBorrowedResourceParamTracker *tracker,
    const WASMComponentPublicResourceValue *resource_value);

void
wasm_component_cleanup_borrowed_resource_params(
    WASMComponentInstance *inst,
    WASMComponentCanonBorrowedResourceParamTracker *tracker);

void
wasm_component_detach_matching_public_resource_value(
    wasm_component_value_t *values, uint32 count,
    WASMComponentPublicResourceValue *match);

bool
wasm_component_public_resource_values_have_same_borrowed_owner(
    const WASMComponentPublicResourceValue *left,
    const WASMComponentPublicResourceValue *right);

#endif
