/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wasm_component_resource_call.h"

#include <string.h>

static bool
set_component_call_error(WASMComponentInstance *inst, const char *message)
{
    wasm_runtime_set_exception((WASMModuleInstanceCommon *)inst, message);
    return false;
}

WASMComponentPublicResourceValue *
wasm_component_get_public_resource_value(const wasm_component_value_t *value)
{
    WASMComponentPublicResourceValue *resource_value;

    if (!value || value->storage_kind != WASM_COMPONENT_VALUE_STORAGE_RESOURCE
        || !value->storage.owned_data)
        return NULL;

    resource_value =
        (WASMComponentPublicResourceValue *)value->storage.owned_data;
    return resource_value->magic == WASM_COMPONENT_PUBLIC_RESOURCE_VALUE_MAGIC
               ? resource_value
               : NULL;
}

void
wasm_component_clear_public_resource_value(wasm_component_value_t *value)
{
    if (!value)
        return;
    memset(value, 0, sizeof(*value));
}

void
wasm_component_consume_public_resource_value(
    WASMComponentPublicResourceValue *resource_value)
{
    if (!resource_value)
        return;

    resource_value->data = NULL;
    resource_value->finalizer = NULL;
    resource_value->finalizer_ctx = NULL;
}

bool
wasm_component_init_public_resource_value(
    WASMComponentInstance *inst,
    const WASMComponentRuntimeResourceState *resource_state, uint32 resource_type_idx,
    uint32 handle, wasm_component_value_t *value)
{
    WASMComponentPublicResourceValue *resource_value;
    char error_buf[128] = { 0 };

    wasm_component_value_destroy(value);
    resource_value = wasm_runtime_malloc(sizeof(*resource_value));
    if (!resource_value)
        return set_component_call_error(
            inst, "host component function could not allocate resource value "
                  "storage");

    if (!wasm_component_resource_take_owned_handle(
            (WASMComponentRuntimeResourceState *)resource_state, resource_type_idx,
            handle, resource_value, error_buf, (uint32)sizeof(error_buf))) {
        wasm_runtime_free(resource_value);
        return set_component_call_error(inst, error_buf);
    }

    value->type.kind = WASM_COMPONENT_VALUE_TYPE_DEFINED;
    value->storage_kind = WASM_COMPONENT_VALUE_STORAGE_RESOURCE;
    value->storage.owned_data = resource_value;
    value->byte_size = 0;
    return true;
}

bool
wasm_component_init_public_borrowed_resource_value(
    WASMComponentInstance *inst,
    const WASMComponentRuntimeResourceState *resource_state, uint32 resource_type_idx,
    uint32 handle, wasm_component_value_t *value)
{
    WASMComponentPublicResourceValue *resource_value;
    uint32 borrowed_handle = 0;
    char error_buf[128] = { 0 };

    wasm_component_value_destroy(value);
    resource_value = wasm_runtime_malloc(sizeof(*resource_value));
    if (!resource_value)
        return set_component_call_error(
            inst, "host component function could not allocate resource value "
                  "storage");

    if (!wasm_component_resource_create_borrowed_handle(
            (WASMComponentRuntimeResourceState *)resource_state, resource_type_idx,
            handle, &borrowed_handle, error_buf, (uint32)sizeof(error_buf))
        || !wasm_component_resource_borrow_handle(
            (WASMComponentRuntimeResourceState *)resource_state, resource_type_idx,
            borrowed_handle, resource_value, error_buf,
            (uint32)sizeof(error_buf))) {
        if (borrowed_handle > 0)
            (void)wasm_component_resource_release_borrowed_handle(
                (WASMComponentRuntimeResourceState *)resource_state,
                resource_type_idx, borrowed_handle, error_buf,
                (uint32)sizeof(error_buf));
        wasm_runtime_free(resource_value);
        return set_component_call_error(inst, error_buf);
    }

    value->type.kind = WASM_COMPONENT_VALUE_TYPE_DEFINED;
    value->storage_kind = WASM_COMPONENT_VALUE_STORAGE_RESOURCE;
    value->storage.owned_data = resource_value;
    value->byte_size = 0;
    return true;
}

bool
wasm_component_promote_pending_imported_resource_result(
    WASMComponentInstance *inst,
    const WASMComponentRuntimeResourceState *resource_state, uint32 resource_type_idx,
    wasm_component_value_t *value, wasm_val_t *handle_value_out)
{
    WASMComponentPublicResourceValue *resource_value =
        wasm_component_get_public_resource_value(value);
    const WASMComponentRuntimeResourceType *runtime_type;
    const WASMComponentRuntimeResourceType *canonical_type;
    char error_buf[128] = { 0 };
    uint32 handle;

    if (!resource_value
        || resource_value->kind
               != WASM_COMPONENT_PUBLIC_RESOURCE_VALUE_PENDING_IMPORTED_RESULT)
        return set_component_call_error(
            inst,
            "host component function result 0 must return an owned resource "
            "argument from the current call or a fresh imported resource result");

    runtime_type = wasm_component_resource_lookup_runtime_type_const(
        resource_state, resource_type_idx);
    canonical_type = runtime_type
                         ? wasm_component_resource_lookup_runtime_type_const(
                               resource_state, runtime_type->canonical_type_idx)
                         : NULL;
    if (!canonical_type
        || canonical_type->kind != WASM_COMP_RUNTIME_RESOURCE_TYPE_IMPORTED)
        return set_component_call_error(
            inst,
            "host component function fresh resource results require an imported "
            "resource type");

    if (!canonical_type->imported_drop_callback)
        return set_component_call_error(
            inst,
            "host component function fresh imported resource result requires a "
            "bound imported resource drop callback");

    if (!wasm_component_resource_create_imported_handle(
            (WASMComponentRuntimeResourceState *)resource_state, resource_type_idx,
            resource_value->data, true, &handle, error_buf,
            (uint32)sizeof(error_buf)))
        return set_component_call_error(inst, error_buf);

    wasm_component_consume_public_resource_value(resource_value);
    wasm_component_value_destroy(value);
    handle_value_out->kind = WASM_I32;
    handle_value_out->of.i32 = (int32)handle;
    return true;
}

bool
wasm_component_restore_public_resource_value(WASMComponentInstance *inst,
                                             wasm_component_value_t *value)
{
    WASMComponentPublicResourceValue *resource_value =
        wasm_component_get_public_resource_value(value);
    char error_buf[128] = { 0 };

    if (!resource_value)
        return set_component_call_error(
            inst, "host component function resource value is invalid");

    if (!wasm_component_resource_restore_owned_handle(resource_value, error_buf,
                                                      (uint32)sizeof(error_buf)))
        return set_component_call_error(inst, error_buf);

    wasm_component_value_destroy(value);
    return true;
}

bool
wasm_component_validate_public_owned_resource_handle(
    WASMComponentInstance *inst,
    const WASMComponentRuntimeResourceState *resource_state, uint32 resource_type_idx,
    uint32 handle)
{
    WASMComponentPublicResourceValue resource_value;
    char error_buf[128] = { 0 };

    memset(&resource_value, 0, sizeof(resource_value));
    if (!wasm_component_resource_take_owned_handle(
            (WASMComponentRuntimeResourceState *)resource_state, resource_type_idx,
            handle, &resource_value, error_buf, (uint32)sizeof(error_buf))
        || !wasm_component_resource_restore_owned_handle(&resource_value, error_buf,
                                                         (uint32)sizeof(error_buf)))
        return set_component_call_error(inst, error_buf);

    return true;
}

bool
wasm_component_track_owned_resource_param(
    WASMComponentCanonOwnedResourceParamTracker *tracker,
    wasm_component_value_t *value)
{
    if (!tracker || !value || tracker->count >= tracker->capacity)
        return false;

    tracker->values[tracker->count++] = value;
    return true;
}

void
wasm_component_consume_owned_resource_params(
    WASMComponentCanonOwnedResourceParamTracker *tracker)
{
    uint32 i;

    if (!tracker || !tracker->values)
        return;

    for (i = 0; i < tracker->count; i++) {
        if (tracker->values[i])
            wasm_component_value_destroy(tracker->values[i]);
    }

    tracker->count = 0;
}

bool
wasm_component_track_borrowed_resource_param(
    WASMComponentCanonBorrowedResourceParamTracker *tracker,
    WASMComponentRuntimeResourceState *resource_state, uint32 resource_type_idx,
    uint32 handle, WASMComponentRuntimeResourceState *owner_resource_state,
    uint32 owner_resource_type_idx, uint32 owner_handle)
{
    if (!tracker || !tracker->params || tracker->count >= tracker->capacity
        || !resource_state || handle == 0 || !owner_resource_state
        || owner_handle == 0)
        return false;

    tracker->params[tracker->count].resource_state = resource_state;
    tracker->params[tracker->count].resource_type_idx = resource_type_idx;
    tracker->params[tracker->count].handle = handle;
    tracker->params[tracker->count].owner_resource_state = owner_resource_state;
    tracker->params[tracker->count].owner_resource_type_idx =
        owner_resource_type_idx;
    tracker->params[tracker->count].owner_handle = owner_handle;
    tracker->count++;
    return true;
}

WASMComponentCanonBorrowedResourceParam *
wasm_component_find_borrowed_resource_param_by_handle(
    WASMComponentCanonBorrowedResourceParamTracker *tracker, uint32 handle,
    uint32 owner_resource_type_idx)
{
    uint32 i;

    if (!tracker || !tracker->params || handle == 0)
        return NULL;

    for (i = 0; i < tracker->count; i++) {
        if (tracker->params[i].handle == handle
            && tracker->params[i].owner_resource_type_idx
                   == owner_resource_type_idx)
            return &tracker->params[i];
    }
    return NULL;
}

bool
wasm_component_public_resource_values_have_same_borrowed_owner(
    const WASMComponentPublicResourceValue *left,
    const WASMComponentPublicResourceValue *right)
{
    WASMComponentRuntimeResourceState *left_owner_state = NULL;
    WASMComponentRuntimeResourceState *right_owner_state = NULL;
    uint32 left_owner_type = WASM_COMPONENT_RESOURCE_INVALID_TYPE_IDX;
    uint32 right_owner_type = WASM_COMPONENT_RESOURCE_INVALID_TYPE_IDX;
    uint32 left_owner_handle = 0;
    uint32 right_owner_handle = 0;
    char error_buf[128] = { 0 };

    if (!left || !right)
        return false;

    if (!wasm_component_resource_get_borrowed_owner(
            left, &left_owner_state, &left_owner_type, &left_owner_handle,
            error_buf, (uint32)sizeof(error_buf))) {
        return false;
    }

    error_buf[0] = '\0';
    if (!wasm_component_resource_get_borrowed_owner(
            right, &right_owner_state, &right_owner_type, &right_owner_handle,
            error_buf, (uint32)sizeof(error_buf))) {
        return false;
    }

    return left_owner_state == right_owner_state
           && left_owner_type == right_owner_type
           && left_owner_handle == right_owner_handle;
}

WASMComponentCanonBorrowedResourceParam *
wasm_component_find_matching_borrowed_resource_param(
    WASMComponentCanonBorrowedResourceParamTracker *tracker,
    const WASMComponentPublicResourceValue *resource_value)
{
    WASMComponentRuntimeResourceState *owner_resource_state = NULL;
    uint32 owner_resource_type_idx = WASM_COMPONENT_RESOURCE_INVALID_TYPE_IDX;
    uint32 owner_handle = 0;
    char error_buf[128] = { 0 };
    uint32 i;

    if (!tracker || !tracker->params || !resource_value)
        return NULL;

    if (!wasm_component_resource_get_borrowed_owner(
            resource_value, &owner_resource_state, &owner_resource_type_idx,
            &owner_handle, error_buf, (uint32)sizeof(error_buf)))
        return NULL;

    for (i = 0; i < tracker->count; i++) {
        if (tracker->params[i].owner_resource_state == owner_resource_state
            && tracker->params[i].owner_resource_type_idx
                   == owner_resource_type_idx
            && tracker->params[i].owner_handle == owner_handle)
            return &tracker->params[i];
    }
    return NULL;
}

void
wasm_component_cleanup_borrowed_resource_params(
    WASMComponentInstance *inst,
    WASMComponentCanonBorrowedResourceParamTracker *tracker)
{
    char error_buf[128];
    uint32 i;

    if (!tracker || !tracker->params)
        return;

    for (i = tracker->count; i > 0; i--) {
        WASMComponentCanonBorrowedResourceParam *param = &tracker->params[i - 1];

        if (!param->resource_state || param->handle == 0)
            continue;

        error_buf[0] = '\0';
        if (!wasm_component_resource_release_borrowed_handle(
                param->resource_state, param->resource_type_idx, param->handle,
                error_buf, (uint32)sizeof(error_buf))
            && !wasm_runtime_get_exception((WASMModuleInstanceCommon *)inst)
            && error_buf[0]) {
            set_component_call_error(inst, error_buf);
        }
    }

    tracker->count = 0;
}

void
wasm_component_detach_matching_public_resource_value(
    wasm_component_value_t *values, uint32 count,
    WASMComponentPublicResourceValue *match)
{
    uint32 i;

    if (!values || !match)
        return;

    for (i = 0; i < count; i++) {
        if (wasm_component_get_public_resource_value(&values[i]) == match)
            wasm_component_clear_public_resource_value(&values[i]);
    }
}
