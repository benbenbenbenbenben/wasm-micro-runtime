/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wasm_component_value.h"
#include <string.h>
#include "wasm_component_resource.h"
#include "wasm_runtime_common.h"

static bool
set_component_value_error(char *error_buf, uint32 error_buf_size,
                          const char *message)
{
    set_error_buf_ex(error_buf, error_buf_size,
                     "WASM component value failed: %s", message);
    return false;
}

const WASMComponentTypes *
wasm_component_lookup_type(const WASMComponent *component, uint32 type_idx)
{
    uint32 remaining = type_idx;
    uint32 i;

    if (!component)
        return NULL;

    for (i = 0; i < component->section_count; i++) {
        const WASMComponentSection *section = &component->sections[i];

        if (section->id != WASM_COMP_SECTION_TYPE || !section->parsed.type_section)
            continue;

        if (remaining < section->parsed.type_section->count)
            return &section->parsed.type_section->types[remaining];

        remaining -= section->parsed.type_section->count;
    }

    return NULL;
}

bool
wasm_component_runtime_value_resolve_type(
    const WASMComponent *component, const WASMComponentValueType *value_type,
    WASMComponentRuntimeValueType *out_type, char *error_buf,
    uint32 error_buf_size)
{
    const WASMComponentTypes *type_entry;

    if (!value_type)
        return set_component_value_error(error_buf, error_buf_size,
                                         "component runtime value is missing a "
                                         "declared type");

    memset(out_type, 0, sizeof(*out_type));
    out_type->declared_type = value_type;

    if (value_type->type == WASM_COMP_VAL_TYPE_PRIMVAL) {
        out_type->kind = WASM_COMP_RUNTIME_VALUE_TYPE_PRIMITIVE;
        out_type->type.primitive_type =
            (WASMComponentPrimValType)value_type->type_specific.primval_type;
        return true;
    }

    type_entry = wasm_component_lookup_type(component,
                                            value_type->type_specific.type_idx);
    if (!type_entry)
        return set_component_value_error(
            error_buf, error_buf_size,
            "component runtime value references an unresolved type index");

    if (type_entry->tag != WASM_COMP_DEF_TYPE || !type_entry->type.def_val_type)
        return set_component_value_error(
            error_buf, error_buf_size,
            "component runtime value references a non-value type");

    out_type->kind = WASM_COMP_RUNTIME_VALUE_TYPE_DEFINED;
    out_type->type.defined_type = type_entry->type.def_val_type;
    return true;
}

const void *
wasm_component_runtime_value_get_data(
    const WASMComponentRuntimeValue *runtime_value)
{
    if (!runtime_value)
        return NULL;

    switch (runtime_value->storage_kind) {
        case WASM_COMP_RUNTIME_VALUE_STORAGE_INLINE:
            return runtime_value->storage.inline_storage;
        case WASM_COMP_RUNTIME_VALUE_STORAGE_BORROWED:
            return runtime_value->storage.borrowed_data;
        case WASM_COMP_RUNTIME_VALUE_STORAGE_OWNED:
            return runtime_value->storage.owned_data;
        default:
            return NULL;
    }
}

void
wasm_component_runtime_value_clear(WASMComponentRuntimeValue *runtime_value)
{
    if (!runtime_value)
        return;

    if (runtime_value->storage_kind == WASM_COMP_RUNTIME_VALUE_STORAGE_OWNED
        && runtime_value->storage.owned_data) {
        if (runtime_value->finalizer)
            runtime_value->finalizer(runtime_value->storage.owned_data,
                                     runtime_value->finalizer_ctx);
        else
            wasm_runtime_free(runtime_value->storage.owned_data);
    }

    memset(runtime_value, 0, sizeof(*runtime_value));
}

static bool
init_component_runtime_value(WASMComponentRuntimeValue *runtime_value,
                             const WASMComponent *component,
                             const WASMComponentValueType *value_type,
                             char *error_buf, uint32 error_buf_size)
{
    WASMComponentRuntimeValueType resolved_type;

    if (!runtime_value)
        return set_component_value_error(error_buf, error_buf_size,
                                         "component runtime value output is null");

    if (!wasm_component_runtime_value_resolve_type(component, value_type,
                                                   &resolved_type, error_buf,
                                                   error_buf_size))
        return false;

    wasm_component_runtime_value_clear(runtime_value);
    runtime_value->type = resolved_type;
    runtime_value->owner_component = component;
    return true;
}

bool
wasm_component_runtime_value_init_inline(
    WASMComponentRuntimeValue *runtime_value, const WASMComponent *component,
    const WASMComponentValueType *value_type, const void *data, uint32 byte_size,
    char *error_buf, uint32 error_buf_size)
{
    if (!init_component_runtime_value(runtime_value, component, value_type,
                                      error_buf, error_buf_size))
        return false;

    if (byte_size > WASM_COMPONENT_RUNTIME_VALUE_INLINE_STORAGE_SIZE)
        return set_component_value_error(
            error_buf, error_buf_size,
            "component runtime inline storage is too small for the value");

    if (byte_size > 0 && !data)
        return set_component_value_error(
            error_buf, error_buf_size,
            "component runtime inline value is missing backing bytes");

    runtime_value->storage_kind = WASM_COMP_RUNTIME_VALUE_STORAGE_INLINE;
    runtime_value->byte_size = byte_size;
    if (byte_size > 0)
        memcpy(runtime_value->storage.inline_storage, data, byte_size);
    return true;
}

bool
wasm_component_runtime_value_init_borrowed(
    WASMComponentRuntimeValue *runtime_value, const WASMComponent *component,
    const WASMComponentValueType *value_type, const void *data, uint32 byte_size,
    char *error_buf, uint32 error_buf_size)
{
    if (!init_component_runtime_value(runtime_value, component, value_type,
                                      error_buf, error_buf_size))
        return false;

    if (byte_size > 0 && !data)
        return set_component_value_error(
            error_buf, error_buf_size,
            "component runtime borrowed value is missing backing bytes");

    runtime_value->storage_kind = WASM_COMP_RUNTIME_VALUE_STORAGE_BORROWED;
    runtime_value->byte_size = byte_size;
    runtime_value->storage.borrowed_data = data;
    return true;
}

bool
wasm_component_runtime_value_init_owned(
    WASMComponentRuntimeValue *runtime_value, const WASMComponent *component,
    const WASMComponentValueType *value_type, void *data, uint32 byte_size,
    WASMComponentRuntimeValueFinalizer finalizer, void *finalizer_ctx,
    char *error_buf, uint32 error_buf_size)
{
    if (!init_component_runtime_value(runtime_value, component, value_type,
                                      error_buf, error_buf_size))
        return false;

    if (byte_size > 0 && !data)
        return set_component_value_error(
            error_buf, error_buf_size,
            "component runtime owned value is missing backing bytes");

    runtime_value->storage_kind = WASM_COMP_RUNTIME_VALUE_STORAGE_OWNED;
    runtime_value->byte_size = byte_size;
    runtime_value->storage.owned_data = data;
    runtime_value->finalizer = finalizer;
    runtime_value->finalizer_ctx = finalizer_ctx;
    return true;
}

bool
wasm_component_runtime_value_clone_borrowed(
    WASMComponentRuntimeValue *dst, const WASMComponentRuntimeValue *src,
    char *error_buf, uint32 error_buf_size)
{
    const void *data;

    if (!dst || !src)
        return set_component_value_error(error_buf, error_buf_size,
                                         "component runtime value clone is null");

    wasm_component_runtime_value_clear(dst);
    dst->type = src->type;
    dst->owner_component = src->owner_component;
    dst->byte_size = src->byte_size;

    switch (src->storage_kind) {
        case WASM_COMP_RUNTIME_VALUE_STORAGE_NONE:
            return true;
        case WASM_COMP_RUNTIME_VALUE_STORAGE_INLINE:
            dst->storage_kind = WASM_COMP_RUNTIME_VALUE_STORAGE_INLINE;
            if (src->byte_size > 0)
                memcpy(dst->storage.inline_storage, src->storage.inline_storage,
                       src->byte_size);
            return true;
        case WASM_COMP_RUNTIME_VALUE_STORAGE_BORROWED:
        case WASM_COMP_RUNTIME_VALUE_STORAGE_OWNED:
            data = wasm_component_runtime_value_get_data(src);
            if (src->byte_size > 0 && !data)
                return set_component_value_error(
                    error_buf, error_buf_size,
                    "component runtime value clone source is missing backing "
                    "bytes");
            dst->storage_kind = WASM_COMP_RUNTIME_VALUE_STORAGE_BORROWED;
            dst->storage.borrowed_data = data;
            return true;
        default:
            return set_component_value_error(
                error_buf, error_buf_size,
                "component runtime value clone source uses an unsupported "
                "storage kind");
    }
}

static bool
set_component_public_value_error(char *error_buf, uint32 error_buf_size,
                                 const char *message)
{
    set_error_buf_ex(error_buf, error_buf_size,
                     "WASM component public value failed: %s", message);
    return false;
}

static const void *
get_component_public_value_data(const wasm_component_value_t *value)
{
    if (!value)
        return NULL;

    switch (value->storage_kind) {
        case WASM_COMPONENT_VALUE_STORAGE_INLINE:
            return value->storage.inline_storage;
        case WASM_COMPONENT_VALUE_STORAGE_OWNED:
            return value->storage.owned_data;
        case WASM_COMPONENT_VALUE_STORAGE_RESOURCE:
        {
            const WASMComponentPublicResourceValue *resource_value =
                (const WASMComponentPublicResourceValue *)value->storage.owned_data;
            return resource_value ? resource_value->data : NULL;
        }
        case WASM_COMPONENT_VALUE_STORAGE_COMPOSITE_RESOURCE:
        {
            const WASMComponentPublicCompositeResourceValue *composite_value =
                (const WASMComponentPublicCompositeResourceValue *)
                    value->storage.owned_data;
            return composite_value ? composite_value->data : NULL;
        }
        default:
            return NULL;
    }
}

WASM_RUNTIME_API_EXTERN const void *
wasm_component_value_get_data(const wasm_component_value_t *value)
{
    return get_component_public_value_data(value);
}

WASM_RUNTIME_API_EXTERN uint32_t
wasm_component_value_get_type_idx(const wasm_component_value_t *value)
{
    if (!value || value->type.kind != WASM_COMPONENT_VALUE_TYPE_DEFINED)
        return UINT32_MAX;
    return value->type.type.type_idx;
}

WASM_RUNTIME_API_EXTERN bool
wasm_component_value_init_owned_imported_resource_result(
    wasm_component_value_t *value, void *data,
    wasm_component_resource_value_finalizer_t finalizer, void *finalizer_ctx)
{
    WASMComponentPublicResourceValue *resource_value;

    if (!value)
        return false;

    wasm_component_value_destroy(value);
    resource_value =
        (WASMComponentPublicResourceValue *)wasm_runtime_malloc(
            sizeof(WASMComponentPublicResourceValue));
    if (!resource_value)
        return false;

    memset(resource_value, 0, sizeof(*resource_value));
    resource_value->magic = WASM_COMPONENT_PUBLIC_RESOURCE_VALUE_MAGIC;
    resource_value->kind =
        WASM_COMPONENT_PUBLIC_RESOURCE_VALUE_PENDING_IMPORTED_RESULT;
    resource_value->data = data;
    resource_value->finalizer = finalizer;
    resource_value->finalizer_ctx = finalizer_ctx;

    value->type.kind = WASM_COMPONENT_VALUE_TYPE_DEFINED;
    value->storage_kind = WASM_COMPONENT_VALUE_STORAGE_RESOURCE;
    value->byte_size = 0;
    value->storage.owned_data = resource_value;
    return true;
}

WASM_RUNTIME_API_EXTERN bool
wasm_component_value_init_owned_local_resource_result(
    wasm_component_value_t *value, void *data,
    wasm_component_resource_value_finalizer_t finalizer, void *finalizer_ctx)
{
    WASMComponentPublicResourceValue *resource_value;

    if (!value)
        return false;

    wasm_component_value_destroy(value);
    resource_value =
        (WASMComponentPublicResourceValue *)wasm_runtime_malloc(
            sizeof(WASMComponentPublicResourceValue));
    if (!resource_value)
        return false;

    memset(resource_value, 0, sizeof(*resource_value));
    resource_value->magic = WASM_COMPONENT_PUBLIC_RESOURCE_VALUE_MAGIC;
    resource_value->kind =
        WASM_COMPONENT_PUBLIC_RESOURCE_VALUE_PENDING_LOCAL_RESULT;
    resource_value->data = data;
    resource_value->finalizer = finalizer;
    resource_value->finalizer_ctx = finalizer_ctx;

    value->type.kind = WASM_COMPONENT_VALUE_TYPE_DEFINED;
    value->storage_kind = WASM_COMPONENT_VALUE_STORAGE_RESOURCE;
    value->byte_size = 0;
    value->storage.owned_data = resource_value;
    return true;
}

WASM_RUNTIME_API_EXTERN bool
wasm_component_value_init_borrowed_resource_result(
    wasm_component_value_t *value, const wasm_component_value_t *borrowed_value)
{
    WASMComponentPublicResourceValue *resource_value;
    WASMComponentPublicResourceValue *source_value;
    char error_buf[128] = { 0 };

    if (!value || !borrowed_value
        || borrowed_value->storage_kind != WASM_COMPONENT_VALUE_STORAGE_RESOURCE
        || !borrowed_value->storage.owned_data)
        return false;

    source_value =
        (WASMComponentPublicResourceValue *)borrowed_value->storage.owned_data;
    if (source_value->magic != WASM_COMPONENT_PUBLIC_RESOURCE_VALUE_MAGIC
        || source_value->kind != WASM_COMPONENT_PUBLIC_RESOURCE_VALUE_BORROWED)
        return false;

    wasm_component_value_destroy(value);
    resource_value =
        (WASMComponentPublicResourceValue *)wasm_runtime_malloc(
            sizeof(WASMComponentPublicResourceValue));
    if (!resource_value)
        return false;

    if (!wasm_component_resource_clone_borrowed_value(
            source_value, resource_value, error_buf, (uint32)sizeof(error_buf))) {
        wasm_runtime_free(resource_value);
        return false;
    }

    value->type.kind = WASM_COMPONENT_VALUE_TYPE_DEFINED;
    value->storage_kind = WASM_COMPONENT_VALUE_STORAGE_RESOURCE;
    value->byte_size = 0;
    value->storage.owned_data = resource_value;
    return true;
}

WASM_RUNTIME_API_EXTERN void
wasm_component_value_destroy(wasm_component_value_t *value)
{
    if (!value)
        return;

    if (value->storage_kind == WASM_COMPONENT_VALUE_STORAGE_RESOURCE
        && value->storage.owned_data) {
        WASMComponentPublicResourceValue *resource_value =
            (WASMComponentPublicResourceValue *)value->storage.owned_data;
        wasm_component_resource_release_public_value(resource_value);
    }
    else if (value->storage_kind == WASM_COMPONENT_VALUE_STORAGE_COMPOSITE_RESOURCE
             && value->storage.owned_data) {
        WASMComponentPublicCompositeResourceValue *composite_value =
            (WASMComponentPublicCompositeResourceValue *)value->storage.owned_data;

            if (composite_value->magic
                == WASM_COMPONENT_PUBLIC_COMPOSITE_RESOURCE_VALUE_MAGIC) {
                if (composite_value->resource_values) {
                    for (uint32 i = 0; i < composite_value->resource_count; i++) {
                        WASMComponentPublicResourceValue *resource_value =
                            &composite_value->resource_values[i];

                        if (resource_value->kind
                            == WASM_COMPONENT_PUBLIC_RESOURCE_VALUE_TRANSFERRED)
                            wasm_component_drop_transferred_public_resource_value(
                                resource_value);
                        else
                            wasm_component_resource_release_public_value(
                                resource_value);
                    }
                    wasm_runtime_free(composite_value->resource_values);
                }
            if (composite_value->data)
                wasm_runtime_free(composite_value->data);
        }
    }

    if ((value->storage_kind == WASM_COMPONENT_VALUE_STORAGE_OWNED
         || value->storage_kind == WASM_COMPONENT_VALUE_STORAGE_RESOURCE
         || value->storage_kind == WASM_COMPONENT_VALUE_STORAGE_COMPOSITE_RESOURCE)
        && value->storage.owned_data)
        wasm_runtime_free(value->storage.owned_data);

    memset(value, 0, sizeof(*value));
}

static bool
restore_component_public_composite_resources(
    const wasm_component_value_t *public_value, char *error_buf,
    uint32 error_buf_size)
{
    WASMComponentPublicCompositeResourceValue *composite_value;

    if (!public_value
        || public_value->storage_kind
               != WASM_COMPONENT_VALUE_STORAGE_COMPOSITE_RESOURCE
        || !public_value->storage.owned_data)
        return set_component_value_error(
            error_buf, error_buf_size,
            "component runtime composite resource value is invalid");

    composite_value = (WASMComponentPublicCompositeResourceValue *)
        public_value->storage.owned_data;
    if (composite_value->magic
        != WASM_COMPONENT_PUBLIC_COMPOSITE_RESOURCE_VALUE_MAGIC)
        return set_component_value_error(
            error_buf, error_buf_size,
            "component runtime composite resource value is malformed");

    for (uint32 i = 0; i < composite_value->resource_count; i++) {
        WASMComponentPublicResourceValue *resource_value =
            &composite_value->resource_values[i];

        if (resource_value->magic != WASM_COMPONENT_PUBLIC_RESOURCE_VALUE_MAGIC)
            return set_component_value_error(
                error_buf, error_buf_size,
                "component runtime composite resource leaf is invalid");
        if (resource_value->kind == WASM_COMPONENT_PUBLIC_RESOURCE_VALUE_TRANSFERRED) {
            if (!resource_value->handle
                || !wasm_component_resource_restore_owned_handle(
                    resource_value, error_buf, error_buf_size))
                return false;
            resource_value->resource_state = NULL;
            resource_value->resource_type_idx =
                WASM_COMPONENT_RESOURCE_INVALID_TYPE_IDX;
            resource_value->canonical_type_idx =
                WASM_COMPONENT_RESOURCE_INVALID_TYPE_IDX;
            resource_value->handle = 0;
            resource_value->data = NULL;
            resource_value->finalizer = NULL;
            resource_value->finalizer_ctx = NULL;
        }
        else if (resource_value->kind
                 == WASM_COMPONENT_PUBLIC_RESOURCE_VALUE_BORROWED) {
            /* Support borrowed resources inside composites by re-borrowing
               from the original owner. */
            WASMComponentRuntimeResourceState *owner_state = NULL;
            uint32 owner_type_idx = WASM_COMPONENT_RESOURCE_INVALID_TYPE_IDX;
            uint32 owner_handle = 0;
            uint32 new_borrowed_handle = 0;
            char local_buf[128] = { 0 };

            if (!wasm_component_resource_get_borrowed_owner(
                    resource_value, &owner_state, &owner_type_idx, &owner_handle,
                    local_buf, (uint32)sizeof(local_buf)))
                return set_component_value_error(
                    error_buf, error_buf_size,
                    "component runtime composite borrowed resource leaf "
                    "could not resolve its owner");

            if (!owner_state || owner_handle == 0)
                return set_component_value_error(
                    error_buf, error_buf_size,
                    "component runtime composite borrowed resource leaf "
                    "has no valid owner");

            if (!wasm_component_resource_create_borrowed_handle(
                    owner_state, owner_type_idx, owner_handle,
                    &new_borrowed_handle, local_buf,
                    (uint32)sizeof(local_buf)))
                return set_component_value_error(
                    error_buf, error_buf_size,
                    "component runtime composite borrowed resource leaf "
                    "could not create borrowed handle");

            if (!wasm_component_resource_borrow_handle(
                    owner_state, owner_type_idx, new_borrowed_handle,
                    resource_value, local_buf, (uint32)sizeof(local_buf))) {
                wasm_component_resource_release_borrowed_handle(
                    owner_state, owner_type_idx, new_borrowed_handle,
                    local_buf, (uint32)sizeof(local_buf));
                return set_component_value_error(
                    error_buf, error_buf_size,
                    "component runtime composite borrowed resource leaf "
                    "could not create borrowed value");
            }
            continue;
        }
        else if (resource_value->kind
                 == WASM_COMPONENT_PUBLIC_RESOURCE_VALUE_PENDING_IMPORTED_RESULT) {
            return set_component_value_error(
                error_buf, error_buf_size,
                "component runtime values do not support pending imported "
                "resource results inside composite values");
        }
    }

    return true;
}

static bool
init_component_runtime_resource_handle_value(
    WASMComponentRuntimeValue *runtime_value, const WASMComponent *component,
    const WASMComponentValueType *value_type,
    const wasm_component_value_t *public_value, char *error_buf,
    uint32 error_buf_size)
{
    const WASMComponentPublicResourceValue *resource_value =
        public_value && public_value->storage_kind == WASM_COMPONENT_VALUE_STORAGE_RESOURCE
            ? (const WASMComponentPublicResourceValue *)public_value->storage.owned_data
            : NULL;
    uint32 handle = 0;
    uint8 encoded_handle[5];
    uint32 encoded_size = 0;

    if (!resource_value
        || resource_value->magic != WASM_COMPONENT_PUBLIC_RESOURCE_VALUE_MAGIC)
        return set_component_value_error(error_buf, error_buf_size,
                                         "component runtime public resource value "
                                         "is invalid");

    if (resource_value->kind == WASM_COMPONENT_PUBLIC_RESOURCE_VALUE_TRANSFERRED) {
        WASMComponentPublicResourceValue *mutable_resource_value =
            (WASMComponentPublicResourceValue *)resource_value;

        handle = resource_value->handle;
        if (!handle
            || !wasm_component_resource_restore_owned_handle(
                mutable_resource_value, error_buf, error_buf_size))
            return false;
    }
    else if (resource_value->kind == WASM_COMPONENT_PUBLIC_RESOURCE_VALUE_BORROWED) {
        if (!wasm_component_resource_get_borrowed_owner(
                resource_value, NULL, NULL, &handle, error_buf, error_buf_size))
            return false;
    }
    else {
        return set_component_value_error(
            error_buf, error_buf_size,
            "component runtime values do not support pending imported "
            "resource results");
    }

    do {
        uint8 byte = (uint8)(handle & 0x7f);
        handle >>= 7;
        if (handle != 0)
            byte |= 0x80;
        encoded_handle[encoded_size++] = byte;
    } while (handle != 0);

    return wasm_component_runtime_value_init_inline(
        runtime_value, component, value_type, encoded_handle, encoded_size,
        error_buf, error_buf_size);
}

bool
wasm_component_runtime_value_init_public(
    WASMComponentRuntimeValue *runtime_value, const WASMComponent *component,
    const WASMComponentValueType *value_type,
    const wasm_component_value_t *public_value, char *error_buf,
    uint32 error_buf_size)
{
    WASMComponentRuntimeValueType resolved_type;
    const void *data = NULL;
    void *owned_data;

    if (!runtime_value || !public_value)
        return set_component_value_error(error_buf, error_buf_size,
                                         "component runtime public value is null");

    if (!wasm_component_runtime_value_resolve_type(component, value_type,
                                                   &resolved_type, error_buf,
                                                   error_buf_size))
        return false;

    if (public_value->type.kind == WASM_COMPONENT_VALUE_TYPE_PRIMITIVE) {
        if (resolved_type.kind != WASM_COMP_RUNTIME_VALUE_TYPE_PRIMITIVE
            || public_value->type.type.primitive_type
                   != (wasm_component_primitive_value_kind_t)
                          resolved_type.type.primitive_type)
            return set_component_value_error(
                error_buf, error_buf_size,
                "component runtime public value type does not match the "
                "declared import type");
    }
    else if (public_value->type.kind == WASM_COMPONENT_VALUE_TYPE_DEFINED) {
        if (resolved_type.kind != WASM_COMP_RUNTIME_VALUE_TYPE_DEFINED)
            return set_component_value_error(
                error_buf, error_buf_size,
                "component runtime public value type does not match the "
                "declared import type");
    }
    else
        return set_component_value_error(error_buf, error_buf_size,
                                         "component runtime public value has an "
                                         "unsupported type kind");

    if (public_value->storage_kind == WASM_COMPONENT_VALUE_STORAGE_RESOURCE)
        return init_component_runtime_resource_handle_value(
            runtime_value, component, value_type, public_value, error_buf,
            error_buf_size);

    if (public_value->storage_kind == WASM_COMPONENT_VALUE_STORAGE_COMPOSITE_RESOURCE
        && !restore_component_public_composite_resources(
            public_value, error_buf, error_buf_size))
        return false;

    if (public_value->byte_size > 0) {
        data = get_component_public_value_data(public_value);
        if (!data)
            return set_component_value_error(
                error_buf, error_buf_size,
                "component runtime public value is missing backing bytes");
    }

    if (public_value->byte_size == 0) {
        wasm_component_runtime_value_clear(runtime_value);
        runtime_value->type = resolved_type;
        runtime_value->owner_component = component;
        return true;
    }

    if (public_value->byte_size <= WASM_COMPONENT_RUNTIME_VALUE_INLINE_STORAGE_SIZE)
        return wasm_component_runtime_value_init_inline(
            runtime_value, component, value_type, data, public_value->byte_size,
            error_buf, error_buf_size);

    owned_data = wasm_runtime_malloc(public_value->byte_size);
    if (!owned_data)
        return set_component_value_error(error_buf, error_buf_size,
                                         "allocate memory failed");

    memcpy(owned_data, data, public_value->byte_size);
    if (!wasm_component_runtime_value_init_owned(
            runtime_value, component, value_type, owned_data,
            public_value->byte_size, NULL, NULL, error_buf, error_buf_size)) {
        wasm_runtime_free(owned_data);
        return false;
    }

    return true;
}

bool
wasm_component_public_value_copy(wasm_component_value_t *dst,
                                 const WASMComponentRuntimeValue *src,
                                 char *error_buf, uint32 error_buf_size)
{
    const void *data;

    if (!dst || !src)
        return set_component_public_value_error(error_buf, error_buf_size,
                                                "component value copy is null");

    wasm_component_value_destroy(dst);
    dst->type.kind =
        src->type.kind == WASM_COMP_RUNTIME_VALUE_TYPE_PRIMITIVE
            ? WASM_COMPONENT_VALUE_TYPE_PRIMITIVE
            : WASM_COMPONENT_VALUE_TYPE_DEFINED;
    if (dst->type.kind == WASM_COMPONENT_VALUE_TYPE_PRIMITIVE)
        dst->type.type.primitive_type =
            (wasm_component_primitive_value_kind_t)
                src->type.type.primitive_type;
    dst->byte_size = src->byte_size;

    if (src->storage_kind == WASM_COMP_RUNTIME_VALUE_STORAGE_NONE
        || src->byte_size == 0)
        return true;

    data = wasm_component_runtime_value_get_data(src);
    if (!data)
        return set_component_public_value_error(
            error_buf, error_buf_size,
            "component value copy source is missing backing bytes");

    if (src->byte_size <= WASM_COMPONENT_VALUE_INLINE_STORAGE_SIZE) {
        dst->storage_kind = WASM_COMPONENT_VALUE_STORAGE_INLINE;
        memcpy(dst->storage.inline_storage, data, src->byte_size);
        return true;
    }

    dst->storage.owned_data = wasm_runtime_malloc(src->byte_size);
    if (!dst->storage.owned_data)
        return set_component_public_value_error(
            error_buf, error_buf_size, "allocate memory failed");

    memcpy(dst->storage.owned_data, data, src->byte_size);
    dst->storage_kind = WASM_COMPONENT_VALUE_STORAGE_OWNED;
    return true;
}
