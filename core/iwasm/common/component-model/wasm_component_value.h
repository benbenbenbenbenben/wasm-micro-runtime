/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef WASM_COMPONENT_VALUE_H
#define WASM_COMPONENT_VALUE_H

#include "wasm_component.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WASM_COMPONENT_RUNTIME_VALUE_INLINE_STORAGE_SIZE 16

typedef enum WASMComponentRuntimeValueTypeKind {
    WASM_COMP_RUNTIME_VALUE_TYPE_PRIMITIVE = 0,
    WASM_COMP_RUNTIME_VALUE_TYPE_DEFINED
} WASMComponentRuntimeValueTypeKind;

typedef struct WASMComponentRuntimeValueType {
    const WASMComponentValueType *declared_type;
    WASMComponentRuntimeValueTypeKind kind;
    union {
        WASMComponentPrimValType primitive_type;
        const WASMComponentDefValType *defined_type;
    } type;
} WASMComponentRuntimeValueType;

typedef enum WASMComponentRuntimeValueStorageKind {
    WASM_COMP_RUNTIME_VALUE_STORAGE_NONE = 0,
    WASM_COMP_RUNTIME_VALUE_STORAGE_INLINE,
    WASM_COMP_RUNTIME_VALUE_STORAGE_BORROWED,
    WASM_COMP_RUNTIME_VALUE_STORAGE_OWNED
} WASMComponentRuntimeValueStorageKind;

typedef void (*WASMComponentRuntimeValueFinalizer)(void *data, void *ctx);

typedef struct WASMComponentRuntimeValue {
    WASMComponentRuntimeValueType type;
    WASMComponentRuntimeValueStorageKind storage_kind;
    uint32 byte_size;
    union {
        uint8 inline_storage[WASM_COMPONENT_RUNTIME_VALUE_INLINE_STORAGE_SIZE];
        const void *borrowed_data;
        void *owned_data;
    } storage;
    WASMComponentRuntimeValueFinalizer finalizer;
    void *finalizer_ctx;
} WASMComponentRuntimeValue;

const WASMComponentTypes *
wasm_component_lookup_type(const WASMComponent *component, uint32 type_idx);

bool
wasm_component_runtime_value_resolve_type(
    const WASMComponent *component, const WASMComponentValueType *value_type,
    WASMComponentRuntimeValueType *out_type, char *error_buf,
    uint32 error_buf_size);

const void *
wasm_component_runtime_value_get_data(
    const WASMComponentRuntimeValue *runtime_value);

void
wasm_component_runtime_value_clear(WASMComponentRuntimeValue *runtime_value);

bool
wasm_component_runtime_value_init_inline(
    WASMComponentRuntimeValue *runtime_value, const WASMComponent *component,
    const WASMComponentValueType *value_type, const void *data, uint32 byte_size,
    char *error_buf, uint32 error_buf_size);

bool
wasm_component_runtime_value_init_borrowed(
    WASMComponentRuntimeValue *runtime_value, const WASMComponent *component,
    const WASMComponentValueType *value_type, const void *data, uint32 byte_size,
    char *error_buf, uint32 error_buf_size);

bool
wasm_component_runtime_value_init_owned(
    WASMComponentRuntimeValue *runtime_value, const WASMComponent *component,
    const WASMComponentValueType *value_type, void *data, uint32 byte_size,
    WASMComponentRuntimeValueFinalizer finalizer, void *finalizer_ctx,
    char *error_buf, uint32 error_buf_size);

bool
wasm_component_runtime_value_clone_borrowed(
    WASMComponentRuntimeValue *dst, const WASMComponentRuntimeValue *src,
    char *error_buf, uint32 error_buf_size);

bool
wasm_component_runtime_value_init_public(
    WASMComponentRuntimeValue *runtime_value, const WASMComponent *component,
    const WASMComponentValueType *value_type,
    const wasm_component_value_t *public_value, char *error_buf,
    uint32 error_buf_size);

bool
wasm_component_public_value_copy(wasm_component_value_t *dst,
                                 const WASMComponentRuntimeValue *src,
                                 char *error_buf, uint32 error_buf_size);

#ifdef __cplusplus
}
#endif

#endif
