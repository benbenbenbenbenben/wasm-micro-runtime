/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wasm_component_resource.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "wasm_runtime_common.h"

static bool
set_component_resource_error(char *error_buf, uint32 error_buf_size,
                             const char *message)
{
    set_error_buf_ex(error_buf, error_buf_size,
                     "WASM component resource failed: %s", message);
    return false;
}

static bool
set_component_resource_error_fmt(char *error_buf, uint32 error_buf_size,
                                 const char *format, ...)
{
    va_list ap;
    char detail[192] = { 0 };

    va_start(ap, format);
    vsnprintf(detail, sizeof(detail), format, ap);
    va_end(ap);

    return set_component_resource_error(error_buf, error_buf_size, detail);
}

static const char *
get_resource_import_name(const WASMComponentImportName *import_name)
{
    if (!import_name || import_name->tag != WASM_COMP_IMPORTNAME_SIMPLE
        || !import_name->imported.simple.name) {
        return NULL;
    }
    return import_name->imported.simple.name->name;
}

static void
imported_resource_noop_finalizer(void *data, void *ctx)
{
    (void)data;
    (void)ctx;
}

static bool
ensure_type_capacity(WASMComponentRuntimeResourceState *resource_state,
                     uint32 min_capacity, char *error_buf,
                     uint32 error_buf_size)
{
    WASMComponentRuntimeResourceType *new_types;
    uint32 new_capacity;

    if (resource_state->type_capacity >= min_capacity)
        return true;

    new_capacity =
        resource_state->type_capacity == 0 ? 8 : resource_state->type_capacity;
    while (new_capacity < min_capacity) {
        if (new_capacity > UINT32_MAX / 2)
            return set_component_resource_error(
                error_buf, error_buf_size,
                "component resource type capacity overflow");
        new_capacity *= 2;
    }

    new_types = wasm_runtime_malloc(
        (uint64)new_capacity * sizeof(WASMComponentRuntimeResourceType));
    if (!new_types)
        return set_component_resource_error(
            error_buf, error_buf_size,
            "allocate memory failed for component resource type table");

    memset(new_types, 0,
           (uint64)new_capacity * sizeof(WASMComponentRuntimeResourceType));
    if (resource_state->types) {
        memcpy(new_types, resource_state->types,
               (uint64)resource_state->type_count
                   * sizeof(WASMComponentRuntimeResourceType));
        wasm_runtime_free(resource_state->types);
    }

    resource_state->types = new_types;
    resource_state->type_capacity = new_capacity;
    return true;
}

static bool
ensure_owned_handle_capacity(WASMComponentRuntimeResourceState *resource_state,
                             uint32 min_capacity, char *error_buf,
                             uint32 error_buf_size)
{
    WASMComponentOwnedResourceHandle *new_handles;
    uint32 new_capacity;

    if (resource_state->owned_handle_capacity >= min_capacity)
        return true;

    new_capacity = resource_state->owned_handle_capacity == 0
                       ? 8
                       : resource_state->owned_handle_capacity;
    while (new_capacity < min_capacity) {
        if (new_capacity > UINT32_MAX / 2)
            return set_component_resource_error(
                error_buf, error_buf_size,
                "component resource owned handle capacity overflow");
        new_capacity *= 2;
    }

    new_handles = wasm_runtime_malloc(
        (uint64)new_capacity * sizeof(WASMComponentOwnedResourceHandle));
    if (!new_handles)
        return set_component_resource_error(
            error_buf, error_buf_size,
            "allocate memory failed for component resource owned handles");

    if (resource_state->owned_handles) {
        memcpy(new_handles, resource_state->owned_handles,
               (uint64)resource_state->owned_handle_count
                   * sizeof(WASMComponentOwnedResourceHandle));
        wasm_runtime_free(resource_state->owned_handles);
    }

    resource_state->owned_handles = new_handles;
    resource_state->owned_handle_capacity = new_capacity;
    return true;
}

static WASMComponentRuntimeResourceType *
append_resource_type_slot(WASMComponentRuntimeResourceState *resource_state,
                          char *error_buf, uint32 error_buf_size)
{
    WASMComponentRuntimeResourceType *type_slot;

    if (!ensure_type_capacity(resource_state, resource_state->type_count + 1,
                              error_buf, error_buf_size))
        return NULL;

    type_slot = &resource_state->types[resource_state->type_count];
    memset(type_slot, 0, sizeof(*type_slot));
    type_slot->type_idx = resource_state->type_count;
    type_slot->canonical_type_idx = WASM_COMPONENT_RESOURCE_INVALID_TYPE_IDX;
    type_slot->source_type_idx = WASM_COMPONENT_RESOURCE_INVALID_TYPE_IDX;
    type_slot->handle_table.next_handle = 1;
    resource_state->type_count++;
    return type_slot;
}

static bool
resolve_resource_alias_slot(WASMComponentRuntimeResourceState *resource_state,
                            uint32 source_type_idx,
                            WASMComponentRuntimeResourceType *type_slot,
                            char *error_buf, uint32 error_buf_size)
{
    WASMComponentRuntimeResourceType *source_type;

    if (source_type_idx >= resource_state->type_count)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component resource type index %u is out of bounds",
            source_type_idx);

    source_type = &resource_state->types[source_type_idx];
    if (source_type->kind == WASM_COMP_RUNTIME_RESOURCE_TYPE_NONE)
        return true;

    type_slot->kind = WASM_COMP_RUNTIME_RESOURCE_TYPE_ALIAS;
    type_slot->source_type_idx = source_type_idx;
    type_slot->canonical_type_idx = source_type->canonical_type_idx;
    type_slot->declared_type = source_type->declared_type;
    type_slot->has_dtor = source_type->has_dtor;
    type_slot->dtor_func_idx = source_type->dtor_func_idx;
    type_slot->has_callback = source_type->has_callback;
    type_slot->callback_func_idx = source_type->callback_func_idx;
    return true;
}

static void
init_local_resource_metadata(WASMComponentRuntimeResourceType *type_slot,
                             const WASMComponentTypes *type_entry)
{
    const WASMComponentResourceType *resource_type = type_entry->type.resource_type;

    type_slot->kind = WASM_COMP_RUNTIME_RESOURCE_TYPE_LOCAL;
    type_slot->canonical_type_idx = type_slot->type_idx;
    type_slot->declared_type = type_entry;
    type_slot->handle_table.next_handle = 1;

    if (type_entry->tag == WASM_COMP_RESOURCE_TYPE_SYNC && resource_type
        && resource_type->resource.sync) {
        type_slot->has_dtor = resource_type->resource.sync->has_dtor;
        type_slot->dtor_func_idx = resource_type->resource.sync->dtor_func_idx;
    }
    else if (type_entry->tag == WASM_COMP_RESOURCE_TYPE_ASYNC && resource_type
             && resource_type->resource.async) {
        type_slot->has_dtor = true;
        type_slot->dtor_func_idx = resource_type->resource.async->dtor_func_idx;
        type_slot->has_callback = true;
        type_slot->callback_func_idx =
            resource_type->resource.async->callback_func_idx;
    }
}

static bool
append_imported_type_slot(WASMComponentRuntimeResourceState *resource_state,
                          const WASMComponentImport *component_import,
                          char *error_buf, uint32 error_buf_size)
{
    const WASMComponentTypeBound *type_bound;
    WASMComponentRuntimeResourceType *type_slot;

    type_slot =
        append_resource_type_slot(resource_state, error_buf, error_buf_size);
    if (!type_slot)
        return false;

    if (!component_import->extern_desc
        || component_import->extern_desc->type != WASM_COMP_EXTERN_TYPE)
        return true;

    type_bound = component_import->extern_desc->extern_desc.type.type_bound;
    if (!type_bound)
        return set_component_resource_error(
            error_buf, error_buf_size,
            "component resource type import is missing a type bound");

    if (type_bound->tag == WASM_COMP_TYPEBOUND_TYPE) {
        type_slot->kind = WASM_COMP_RUNTIME_RESOURCE_TYPE_IMPORTED;
        type_slot->canonical_type_idx = type_slot->type_idx;
        type_slot->import_name =
            get_resource_import_name(component_import->import_name);
        type_slot->handle_table.next_handle = 1;
        return true;
    }

    if (type_bound->tag == WASM_COMP_TYPEBOUND_EQ)
        return resolve_resource_alias_slot(resource_state, type_bound->type_idx,
                                           type_slot, error_buf,
                                           error_buf_size);

    return set_component_resource_error_fmt(
        error_buf, error_buf_size,
        "unsupported component resource type bound tag 0x%02x",
        (unsigned)type_bound->tag);
}

static bool
append_local_type_slots(WASMComponentRuntimeResourceState *resource_state,
                        const WASMComponentTypeSection *type_section,
                        char *error_buf, uint32 error_buf_size)
{
    uint32 i;

    for (i = 0; i < type_section->count; i++) {
        WASMComponentRuntimeResourceType *type_slot =
            append_resource_type_slot(resource_state, error_buf, error_buf_size);
        const WASMComponentTypes *type_entry;

        if (!type_slot)
            return false;

        type_entry = &type_section->types[i];
        if (type_entry->tag == WASM_COMP_RESOURCE_TYPE_SYNC
            || type_entry->tag == WASM_COMP_RESOURCE_TYPE_ASYNC)
            init_local_resource_metadata(type_slot, type_entry);
    }

    return true;
}

static bool
append_alias_type_slots(WASMComponentRuntimeResourceState *resource_state,
                        const WASMComponentAliasSection *alias_section,
                        char *error_buf, uint32 error_buf_size)
{
    uint32 i;

    for (i = 0; i < alias_section->count; i++) {
        WASMComponentRuntimeResourceType *type_slot;
        const WASMComponentAliasDefinition *alias_def = &alias_section->aliases[i];

        if (!alias_def->sort || alias_def->sort->sort != WASM_COMP_SORT_TYPE)
            continue;

        type_slot =
            append_resource_type_slot(resource_state, error_buf, error_buf_size);
        if (!type_slot)
            return false;

        if (alias_def->alias_target_type == WASM_COMP_ALIAS_TARGET_OUTER)
            continue;
    }

    return true;
}

static bool
append_export_type_slots(WASMComponentRuntimeResourceState *resource_state,
                         const WASMComponentExportSection *export_section,
                         char *error_buf, uint32 error_buf_size)
{
    uint32 i;

    for (i = 0; i < export_section->count; i++) {
        const WASMComponentExport *component_export = &export_section->exports[i];
        WASMComponentRuntimeResourceType *type_slot;

        if (!component_export->sort_idx || !component_export->sort_idx->sort
            || component_export->sort_idx->sort->sort != WASM_COMP_SORT_TYPE)
            continue;

        type_slot =
            append_resource_type_slot(resource_state, error_buf, error_buf_size);
        if (!type_slot)
            return false;

        if (!resolve_resource_alias_slot(resource_state,
                                         component_export->sort_idx->idx,
                                         type_slot, error_buf, error_buf_size))
            return false;
    }

    return true;
}

static bool
scan_component_resource_types(WASMComponentRuntimeResourceState *resource_state,
                              const WASMComponent *component, char *error_buf,
                              uint32 error_buf_size)
{
    uint32 i;

    for (i = 0; i < component->section_count; i++) {
        const WASMComponentSection *section = &component->sections[i];

        switch (section->id) {
            case WASM_COMP_SECTION_IMPORTS:
            {
                uint32 j;
                const WASMComponentImportSection *import_section =
                    section->parsed.import_section;

                for (j = 0; j < import_section->count; j++) {
                    const WASMComponentImport *component_import =
                        &import_section->imports[j];

                    if (component_import->extern_desc
                        && component_import->extern_desc->type
                               == WASM_COMP_EXTERN_TYPE
                        && !append_imported_type_slot(resource_state,
                                                     component_import, error_buf,
                                                     error_buf_size))
                        return false;
                }
                break;
            }
            case WASM_COMP_SECTION_ALIASES:
                if (!append_alias_type_slots(resource_state,
                                             section->parsed.alias_section,
                                             error_buf, error_buf_size))
                    return false;
                break;
            case WASM_COMP_SECTION_TYPE:
                if (!append_local_type_slots(resource_state,
                                             section->parsed.type_section,
                                             error_buf, error_buf_size))
                    return false;
                break;
            case WASM_COMP_SECTION_EXPORTS:
                if (!append_export_type_slots(resource_state,
                                              section->parsed.export_section,
                                              error_buf, error_buf_size))
                    return false;
                break;
            default:
                break;
        }
    }

    return true;
}

static WASMComponentRuntimeResourceType *
resolve_canonical_resource_type(WASMComponentRuntimeResourceState *resource_state,
                                uint32 type_idx)
{
    WASMComponentRuntimeResourceType *type_slot;

    if (!resource_state || type_idx >= resource_state->type_count)
        return NULL;

    type_slot = &resource_state->types[type_idx];
    if (type_slot->kind == WASM_COMP_RUNTIME_RESOURCE_TYPE_NONE
        || type_slot->canonical_type_idx == WASM_COMPONENT_RESOURCE_INVALID_TYPE_IDX
        || type_slot->canonical_type_idx >= resource_state->type_count)
        return NULL;

    return &resource_state->types[type_slot->canonical_type_idx];
}

static bool
ensure_handle_entry_capacity(WASMComponentResourceHandleTable *handle_table,
                             uint32 min_entries, char *error_buf,
                             uint32 error_buf_size)
{
    WASMComponentResourceHandleEntry *new_entries;
    uint32 new_entry_count;

    if (handle_table->entry_count >= min_entries)
        return true;

    new_entry_count = handle_table->entry_count == 0 ? 8 : handle_table->entry_count;
    while (new_entry_count < min_entries) {
        if (new_entry_count > UINT32_MAX / 2)
            return set_component_resource_error(
                error_buf, error_buf_size,
                "component resource handle table capacity overflow");
        new_entry_count *= 2;
    }

    new_entries = wasm_runtime_malloc(
        (uint64)new_entry_count * sizeof(WASMComponentResourceHandleEntry));
    if (!new_entries)
        return set_component_resource_error(
            error_buf, error_buf_size,
            "allocate memory failed for component resource handle table");

    memset(new_entries, 0,
           (uint64)new_entry_count * sizeof(WASMComponentResourceHandleEntry));
    if (handle_table->entries) {
        memcpy(new_entries, handle_table->entries,
               (uint64)handle_table->entry_count
                   * sizeof(WASMComponentResourceHandleEntry));
        wasm_runtime_free(handle_table->entries);
    }

    handle_table->entries = new_entries;
    handle_table->entry_count = new_entry_count;
    return true;
}

static void
release_handle_entry(WASMComponentResourceHandleEntry *entry)
{
    uint32 generation;

    if (!entry || !entry->is_live)
        return;

    if (entry->is_owned && entry->data) {
        if (entry->finalizer)
            entry->finalizer(entry->data, entry->finalizer_ctx);
        else
            wasm_runtime_free(entry->data);
    }

    generation = entry->generation;
    memset(entry, 0, sizeof(*entry));
    entry->generation = generation;
}

static void
init_live_handle_entry(WASMComponentResourceHandleEntry *entry, bool is_owned,
                       uint32 handle, void *data,
                       WASMComponentResourceHandleFinalizer finalizer,
                       void *finalizer_ctx)
{
    uint32 generation;

    generation = entry->generation + 1;
    if (generation == 0)
        generation = 1;

    memset(entry, 0, sizeof(*entry));
    entry->is_live = true;
    entry->is_owned = is_owned;
    entry->handle = handle;
    entry->generation = generation;
    entry->data = data;
    entry->finalizer = finalizer;
    entry->finalizer_ctx = finalizer_ctx;
}

WASMComponentRuntimeResourceState *
wasm_component_resource_state_create(const WASMComponent *component,
                                     char *error_buf,
                                     uint32 error_buf_size)
{
    WASMComponentRuntimeResourceState *resource_state =
        wasm_runtime_malloc(sizeof(WASMComponentRuntimeResourceState));

    if (!resource_state) {
        set_component_resource_error(
            error_buf, error_buf_size,
            "allocate memory failed for component resource state");
        return NULL;
    }

    memset(resource_state, 0, sizeof(*resource_state));
    resource_state->component = component;
    if (component && !scan_component_resource_types(resource_state, component,
                                                   error_buf, error_buf_size)) {
        wasm_component_resource_state_destroy(resource_state);
        return NULL;
    }

    return resource_state;
}

void
wasm_component_resource_state_destroy(
    WASMComponentRuntimeResourceState *resource_state)
{
    uint32 i;

    if (!resource_state)
        return;

    for (i = 0; i < resource_state->owned_handle_count; i++) {
        WASMComponentOwnedResourceHandle *owned_handle =
            &resource_state->owned_handles[i];
        WASMComponentRuntimeResourceType *canonical_type =
            resolve_canonical_resource_type(resource_state, owned_handle->type_idx);
        uint32 handle_index;

        if (!canonical_type || owned_handle->handle == 0)
            continue;

        handle_index = owned_handle->handle - 1;
        if (handle_index >= canonical_type->handle_table.entry_count)
            continue;

        if (canonical_type->handle_table.entries[handle_index].is_live) {
            WASMComponentResourceHandleEntry *entry =
                &canonical_type->handle_table.entries[handle_index];
            if (entry->borrow_count > 0) {
                /* Skip owned handles with outstanding borrows to avoid
                   use-after-free when borrowed handles are released later. */
                continue;
            }
            release_handle_entry(entry);
            if (canonical_type->handle_table.live_handle_count > 0)
                canonical_type->handle_table.live_handle_count--;
            if (canonical_type->handle_table.owned_handle_count > 0)
                canonical_type->handle_table.owned_handle_count--;
        }
    }

    if (resource_state->types) {
        for (i = 0; i < resource_state->type_count; i++) {
            if (resource_state->types[i].handle_table.entries) {
                wasm_runtime_free(resource_state->types[i].handle_table.entries);
                resource_state->types[i].handle_table.entries = NULL;
            }
        }
        wasm_runtime_free(resource_state->types);
    }

    if (resource_state->owned_handles)
        wasm_runtime_free(resource_state->owned_handles);

    wasm_runtime_free(resource_state);
}

WASMComponentRuntimeResourceType *
wasm_component_resource_lookup_runtime_type(
    WASMComponentRuntimeResourceState *resource_state, uint32 type_idx)
{
    if (!resource_state || type_idx >= resource_state->type_count)
        return NULL;

    return &resource_state->types[type_idx];
}

const WASMComponentRuntimeResourceType *
wasm_component_resource_lookup_runtime_type_const(
    const WASMComponentRuntimeResourceState *resource_state, uint32 type_idx)
{
    if (!resource_state || type_idx >= resource_state->type_count)
        return NULL;

    return &resource_state->types[type_idx];
}

bool
wasm_component_resource_create_owned_handle(
    WASMComponentRuntimeResourceState *resource_state, uint32 type_idx,
    void *data, WASMComponentResourceHandleFinalizer finalizer,
    void *finalizer_ctx, uint32 *out_handle, char *error_buf,
    uint32 error_buf_size)
{
    WASMComponentRuntimeResourceType *canonical_type;
    WASMComponentResourceHandleTable *handle_table;
    uint32 handle;

    if (!resource_state || !out_handle)
        return set_component_resource_error(
            error_buf, error_buf_size,
            "component resource owned handle output is null");

    canonical_type = resolve_canonical_resource_type(resource_state, type_idx);
    if (!canonical_type)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component type index %u is not a runtime resource type", type_idx);
    if (canonical_type->kind != WASM_COMP_RUNTIME_RESOURCE_TYPE_LOCAL)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component type index %u is not a local resource type", type_idx);

    handle_table = &canonical_type->handle_table;
    if (handle_table->next_handle == 0)
        return set_component_resource_error(
            error_buf, error_buf_size,
            "component resource handle space exhausted");

    handle = handle_table->next_handle++;
    if (!ensure_handle_entry_capacity(handle_table, handle, error_buf,
                                      error_buf_size)
        || !ensure_owned_handle_capacity(resource_state,
                                         resource_state->owned_handle_count + 1,
                                         error_buf, error_buf_size))
        return false;

    init_live_handle_entry(&handle_table->entries[handle - 1], true, handle, data,
                           finalizer, finalizer_ctx);
    handle_table->live_handle_count++;
    handle_table->owned_handle_count++;

    resource_state->owned_handles[resource_state->owned_handle_count].type_idx =
        canonical_type->type_idx;
    resource_state->owned_handles[resource_state->owned_handle_count].handle = handle;
    resource_state->owned_handle_count++;
    *out_handle = handle;
    return true;
}

bool
wasm_component_resource_drop_owned_handle(
    WASMComponentRuntimeResourceState *resource_state, uint32 type_idx,
    uint32 handle, char *error_buf, uint32 error_buf_size)
{
    WASMComponentRuntimeResourceType *canonical_type;
    WASMComponentResourceHandleEntry *entry;

    if (!resource_state || handle == 0)
        return set_component_resource_error(
            error_buf, error_buf_size,
            "component resource handle is invalid");

    canonical_type = resolve_canonical_resource_type(resource_state, type_idx);
    if (!canonical_type)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component type index %u is not a runtime resource type", type_idx);

    if (handle - 1 >= canonical_type->handle_table.entry_count)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component resource handle %u is out of bounds", handle);

    entry = &canonical_type->handle_table.entries[handle - 1];
    if (!entry->is_live || !entry->is_owned)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component resource handle %u is not an owned live handle", handle);
    if (entry->borrow_count > 0)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component resource handle %u has outstanding borrowed handles",
            handle);

    release_handle_entry(entry);
    canonical_type->handle_table.live_handle_count--;
    canonical_type->handle_table.owned_handle_count--;
    return true;
}

bool
wasm_component_resource_bind_imported_drop_callback(
    WASMComponentRuntimeResourceState *resource_state, uint32 type_idx,
    wasm_component_imported_resource_drop_callback_t callback, void *user_data,
    char *error_buf, uint32 error_buf_size)
{
    WASMComponentRuntimeResourceType *canonical_type =
        resolve_canonical_resource_type(resource_state, type_idx);

    if (!canonical_type)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component type index %u is not a runtime resource type", type_idx);
    if (canonical_type->kind != WASM_COMP_RUNTIME_RESOURCE_TYPE_IMPORTED)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component type index %u is not an imported resource type", type_idx);

    canonical_type->imported_drop_callback = callback;
    canonical_type->imported_drop_user_data = user_data;
    return true;
}

bool
wasm_component_resource_bind_imported_new_callback(
    WASMComponentRuntimeResourceState *resource_state, uint32 type_idx,
    wasm_component_imported_resource_new_callback_t callback, void *user_data,
    char *error_buf, uint32 error_buf_size)
{
    WASMComponentRuntimeResourceType *canonical_type =
        resolve_canonical_resource_type(resource_state, type_idx);

    if (!canonical_type)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component type index %u is not a runtime resource type", type_idx);
    if (canonical_type->kind != WASM_COMP_RUNTIME_RESOURCE_TYPE_IMPORTED)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component type index %u is not an imported resource type", type_idx);

    canonical_type->imported_new_callback = callback;
    canonical_type->imported_new_user_data = user_data;
    return true;
}

bool
wasm_component_resource_create_imported_handle(
    WASMComponentRuntimeResourceState *resource_state, uint32 type_idx,
    void *data, bool owned, uint32 *out_handle, char *error_buf,
    uint32 error_buf_size)
{
    WASMComponentRuntimeResourceType *canonical_type;
    WASMComponentResourceHandleTable *handle_table;
    uint32 handle;

    if (!resource_state || !out_handle)
        return set_component_resource_error(
            error_buf, error_buf_size,
            "component imported resource handle output is null");

    canonical_type = resolve_canonical_resource_type(resource_state, type_idx);
    if (!canonical_type)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component type index %u is not a runtime resource type", type_idx);
    if (canonical_type->kind != WASM_COMP_RUNTIME_RESOURCE_TYPE_IMPORTED)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component type index %u is not an imported resource type", type_idx);

    handle_table = &canonical_type->handle_table;
    if (handle_table->next_handle == 0)
        return set_component_resource_error(
            error_buf, error_buf_size,
            "component resource handle space exhausted");

    handle = handle_table->next_handle++;
    if (!ensure_handle_entry_capacity(handle_table, handle, error_buf,
                                      error_buf_size)
        || (owned && !ensure_owned_handle_capacity(resource_state,
                                                   resource_state->owned_handle_count
                                                       + 1,
                                                   error_buf,
                                                   error_buf_size)))
        return false;

    init_live_handle_entry(&handle_table->entries[handle - 1], owned, handle, data,
                           owned ? imported_resource_noop_finalizer : NULL, NULL);
    handle_table->live_handle_count++;

    if (owned) {
        handle_table->owned_handle_count++;
        resource_state->owned_handles[resource_state->owned_handle_count].type_idx =
            canonical_type->type_idx;
        resource_state->owned_handles[resource_state->owned_handle_count].handle =
            handle;
        resource_state->owned_handle_count++;
    }

    *out_handle = handle;
    return true;
}

bool
wasm_component_resource_take_owned_handle(
    WASMComponentRuntimeResourceState *resource_state, uint32 type_idx,
    uint32 handle, WASMComponentPublicResourceValue *resource_value_out,
    char *error_buf, uint32 error_buf_size)
{
    WASMComponentRuntimeResourceType *resource_type;
    WASMComponentRuntimeResourceType *canonical_type;
    WASMComponentResourceHandleEntry *entry;

    if (!resource_state || !resource_value_out || handle == 0)
        return set_component_resource_error(
            error_buf, error_buf_size,
            "component resource owned handle transfer is invalid");

    resource_type = wasm_component_resource_lookup_runtime_type(resource_state, type_idx);
    if (!resource_type)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component type index %u is not a runtime resource type", type_idx);

    canonical_type =
        resolve_canonical_resource_type(resource_state, resource_type->type_idx);
    if (!canonical_type)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component type index %u is not a canonical resource type", type_idx);

    if (handle - 1 >= canonical_type->handle_table.entry_count)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component resource handle %u is out of bounds", handle);

    entry = &canonical_type->handle_table.entries[handle - 1];
    if (!entry->is_live || !entry->is_owned)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component resource handle %u is not an owned live handle", handle);
    if (entry->borrow_count > 0)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component resource handle %u has outstanding borrowed handles",
            handle);

    memset(resource_value_out, 0, sizeof(*resource_value_out));
    resource_value_out->magic = WASM_COMPONENT_PUBLIC_RESOURCE_VALUE_MAGIC;
    resource_value_out->kind = WASM_COMPONENT_PUBLIC_RESOURCE_VALUE_TRANSFERRED;
    resource_value_out->resource_state = resource_state;
    resource_value_out->resource_type_idx = resource_type->type_idx;
    resource_value_out->canonical_type_idx = canonical_type->type_idx;
    resource_value_out->handle = handle;
    resource_value_out->data = entry->data;
    resource_value_out->finalizer = entry->finalizer;
    resource_value_out->finalizer_ctx = entry->finalizer_ctx;

    {
        uint32 generation = entry->generation;

        memset(entry, 0, sizeof(*entry));
        entry->generation = generation;
    }
    if (canonical_type->handle_table.live_handle_count > 0)
        canonical_type->handle_table.live_handle_count--;
    if (canonical_type->handle_table.owned_handle_count > 0)
        canonical_type->handle_table.owned_handle_count--;
    return true;
}

bool
wasm_component_resource_restore_owned_handle(
    WASMComponentPublicResourceValue *resource_value, char *error_buf,
    uint32 error_buf_size)
{
    WASMComponentRuntimeResourceState *resource_state;
    WASMComponentRuntimeResourceType *resource_type;
    WASMComponentRuntimeResourceType *canonical_type;
    WASMComponentResourceHandleEntry *entry;
    uint32 handle;

    if (!resource_value
        || resource_value->magic != WASM_COMPONENT_PUBLIC_RESOURCE_VALUE_MAGIC
        || !resource_value->resource_state || resource_value->handle == 0)
        return set_component_resource_error(
            error_buf, error_buf_size,
            "component public resource value is invalid");

    resource_state = resource_value->resource_state;
    resource_type = wasm_component_resource_lookup_runtime_type(
        resource_state, resource_value->resource_type_idx);
    if (!resource_type)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component type index %u is not a runtime resource type",
            resource_value->resource_type_idx);

    canonical_type = resolve_canonical_resource_type(
        resource_state, resource_value->canonical_type_idx);
    if (!canonical_type)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component type index %u is not a canonical resource type",
            resource_value->canonical_type_idx);

    handle = resource_value->handle;
    if (!ensure_handle_entry_capacity(&canonical_type->handle_table, handle, error_buf,
                                      error_buf_size))
        return false;

    entry = &canonical_type->handle_table.entries[handle - 1];
    if (entry->is_live)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component resource handle %u is already live", handle);

    init_live_handle_entry(entry, true, handle, resource_value->data,
                           resource_value->finalizer,
                           resource_value->finalizer_ctx);

    canonical_type->handle_table.live_handle_count++;
    canonical_type->handle_table.owned_handle_count++;
    if (canonical_type->handle_table.next_handle <= handle)
        canonical_type->handle_table.next_handle = handle + 1;

    resource_value->magic = 0;
    resource_value->resource_state = NULL;
    resource_value->resource_type_idx = WASM_COMPONENT_RESOURCE_INVALID_TYPE_IDX;
    resource_value->canonical_type_idx = WASM_COMPONENT_RESOURCE_INVALID_TYPE_IDX;
    resource_value->handle = 0;
    resource_value->data = NULL;
    resource_value->finalizer = NULL;
    resource_value->finalizer_ctx = NULL;
    return true;
}

bool
wasm_component_resource_borrow_handle(
    WASMComponentRuntimeResourceState *resource_state, uint32 type_idx,
    uint32 handle, WASMComponentPublicResourceValue *resource_value_out,
    char *error_buf, uint32 error_buf_size)
{
    WASMComponentRuntimeResourceType *resource_type;
    WASMComponentRuntimeResourceType *canonical_type;
    WASMComponentResourceHandleEntry *entry;

    if (!resource_state || !resource_value_out || handle == 0)
        return set_component_resource_error(
            error_buf, error_buf_size,
            "component resource borrowed handle view is invalid");

    resource_type = wasm_component_resource_lookup_runtime_type(resource_state, type_idx);
    if (!resource_type)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component type index %u is not a runtime resource type", type_idx);

    canonical_type =
        resolve_canonical_resource_type(resource_state, resource_type->type_idx);
    if (!canonical_type)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component type index %u is not a canonical resource type", type_idx);

    if (handle - 1 >= canonical_type->handle_table.entry_count)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component resource handle %u is out of bounds", handle);

    entry = &canonical_type->handle_table.entries[handle - 1];
    if (!entry->is_live)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component resource handle %u is not a live handle", handle);

    memset(resource_value_out, 0, sizeof(*resource_value_out));
    resource_value_out->magic = WASM_COMPONENT_PUBLIC_RESOURCE_VALUE_MAGIC;
    resource_value_out->kind = WASM_COMPONENT_PUBLIC_RESOURCE_VALUE_BORROWED;
    resource_value_out->resource_state = resource_state;
    resource_value_out->resource_type_idx = resource_type->type_idx;
    resource_value_out->canonical_type_idx = canonical_type->type_idx;
    resource_value_out->handle = handle;
    resource_value_out->data = entry->data;
    resource_value_out->finalizer = entry->finalizer;
    resource_value_out->finalizer_ctx = entry->finalizer_ctx;
    return true;
}

bool
wasm_component_resource_get_borrowed_owner(
    const WASMComponentPublicResourceValue *resource_value,
    WASMComponentRuntimeResourceState **owner_resource_state_out,
    uint32 *owner_resource_type_idx_out, uint32 *owner_handle_out,
    char *error_buf, uint32 error_buf_size)
{
    WASMComponentRuntimeResourceState *resource_state;
    WASMComponentRuntimeResourceType *resource_type;
    WASMComponentRuntimeResourceType *canonical_type;
    WASMComponentResourceHandleEntry *entry;
    WASMComponentResourceHandleEntry *source_entry;
    uint32 owner_handle;

    if (!resource_value
        || resource_value->magic != WASM_COMPONENT_PUBLIC_RESOURCE_VALUE_MAGIC
        || resource_value->kind != WASM_COMPONENT_PUBLIC_RESOURCE_VALUE_BORROWED
        || !resource_value->resource_state || resource_value->handle == 0)
        return set_component_resource_error(
            error_buf, error_buf_size,
            "component public borrowed resource value is invalid");

    resource_state = resource_value->resource_state;
    resource_type = wasm_component_resource_lookup_runtime_type(
        resource_state, resource_value->resource_type_idx);
    if (!resource_type)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component type index %u is not a runtime resource type",
            resource_value->resource_type_idx);

    canonical_type = resolve_canonical_resource_type(resource_state, resource_type->type_idx);
    if (!canonical_type)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component type index %u is not a canonical resource type",
            resource_value->resource_type_idx);

    if (resource_value->handle - 1 >= canonical_type->handle_table.entry_count)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component resource handle %u is out of bounds",
            resource_value->handle);

    entry = &canonical_type->handle_table.entries[resource_value->handle - 1];
    if (!entry->is_live)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component resource handle %u is not a live handle",
            resource_value->handle);

    if (entry->is_owned) {
        owner_handle = entry->handle;
    }
    else {
        if (entry->borrowed_from_handle == 0
            || entry->borrowed_from_handle - 1
                   >= canonical_type->handle_table.entry_count)
            return set_component_resource_error_fmt(
                error_buf, error_buf_size,
                "component borrowed resource handle %u has no owned source",
                resource_value->handle);

        source_entry =
            &canonical_type->handle_table.entries[entry->borrowed_from_handle - 1];
        if (!source_entry->is_live || !source_entry->is_owned
            || source_entry->generation != entry->borrowed_from_generation)
            return set_component_resource_error_fmt(
                error_buf, error_buf_size,
                "component borrowed resource handle %u has a stale owned source",
                resource_value->handle);
        owner_handle = source_entry->handle;
    }

    if (owner_resource_state_out)
        *owner_resource_state_out = resource_state;
    if (owner_resource_type_idx_out)
        *owner_resource_type_idx_out = resource_type->type_idx;
    if (owner_handle_out)
        *owner_handle_out = owner_handle;
    return true;
}

bool
wasm_component_resource_clone_borrowed_value(
    const WASMComponentPublicResourceValue *source_value,
    WASMComponentPublicResourceValue *resource_value_out, char *error_buf,
    uint32 error_buf_size)
{
    WASMComponentRuntimeResourceState *owner_resource_state;
    uint32 owner_resource_type_idx, owner_handle, borrowed_handle = 0;

    if (!resource_value_out)
        return set_component_resource_error(
            error_buf, error_buf_size,
            "component public borrowed resource clone is invalid");

    if (!wasm_component_resource_get_borrowed_owner(
            source_value, &owner_resource_state, &owner_resource_type_idx,
            &owner_handle, error_buf, error_buf_size))
        return false;

    if (!wasm_component_resource_create_borrowed_handle(
            owner_resource_state, owner_resource_type_idx, owner_handle,
            &borrowed_handle, error_buf, error_buf_size))
        return false;

    if (!wasm_component_resource_borrow_handle(
            owner_resource_state, owner_resource_type_idx, borrowed_handle,
            resource_value_out, error_buf, error_buf_size)) {
        if (borrowed_handle > 0)
            (void)wasm_component_resource_release_borrowed_handle(
                owner_resource_state, owner_resource_type_idx, borrowed_handle,
                error_buf, error_buf_size);
        return false;
    }

    return true;
}

bool
wasm_component_resource_create_borrowed_handle(
    WASMComponentRuntimeResourceState *resource_state, uint32 type_idx,
    uint32 source_handle, uint32 *out_handle, char *error_buf,
    uint32 error_buf_size)
{
    WASMComponentRuntimeResourceType *resource_type;
    WASMComponentRuntimeResourceType *canonical_type;
    WASMComponentResourceHandleEntry *source_entry, *entry;
    uint32 handle;

    if (!resource_state || !out_handle || source_handle == 0)
        return set_component_resource_error(
            error_buf, error_buf_size,
            "component resource borrowed handle creation is invalid");

    resource_type = wasm_component_resource_lookup_runtime_type(resource_state, type_idx);
    if (!resource_type)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component type index %u is not a runtime resource type", type_idx);

    canonical_type =
        resolve_canonical_resource_type(resource_state, resource_type->type_idx);
    if (!canonical_type)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component type index %u is not a canonical resource type", type_idx);

    if (source_handle - 1 >= canonical_type->handle_table.entry_count)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component resource handle %u is out of bounds", source_handle);

    source_entry = &canonical_type->handle_table.entries[source_handle - 1];
    if (!source_entry->is_live || !source_entry->is_owned)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component resource handle %u is not an owned live handle",
            source_handle);

    handle = canonical_type->handle_table.next_handle++;
    if (!ensure_handle_entry_capacity(&canonical_type->handle_table, handle, error_buf,
                                      error_buf_size))
        return false;

    entry = &canonical_type->handle_table.entries[handle - 1];
    if (entry->is_live)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component resource handle %u is already live", handle);

    init_live_handle_entry(entry, false, handle, source_entry->data, NULL, NULL);
    entry->borrowed_from_handle = source_handle;
    entry->borrowed_from_generation = source_entry->generation;
    source_entry->borrow_count++;
    canonical_type->handle_table.live_handle_count++;

    *out_handle = handle;
    return true;
}

bool
wasm_component_resource_release_borrowed_handle(
    WASMComponentRuntimeResourceState *resource_state, uint32 type_idx,
    uint32 handle, char *error_buf, uint32 error_buf_size)
{
    WASMComponentRuntimeResourceType *resource_type;
    WASMComponentRuntimeResourceType *canonical_type;
    WASMComponentResourceHandleEntry *entry, *source_entry = NULL;
    uint32 source_handle, source_generation;

    if (!resource_state || handle == 0)
        return set_component_resource_error(
            error_buf, error_buf_size,
            "component resource borrowed handle release is invalid");

    resource_type = wasm_component_resource_lookup_runtime_type(resource_state, type_idx);
    if (!resource_type)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component type index %u is not a runtime resource type", type_idx);

    canonical_type =
        resolve_canonical_resource_type(resource_state, resource_type->type_idx);
    if (!canonical_type)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component type index %u is not a canonical resource type", type_idx);

    if (handle - 1 >= canonical_type->handle_table.entry_count)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component resource handle %u is out of bounds", handle);

    entry = &canonical_type->handle_table.entries[handle - 1];
    if (!entry->is_live)
        return true;

    if (entry->is_owned)
        return set_component_resource_error_fmt(
            error_buf, error_buf_size,
            "component resource handle %u is not a borrowed live handle", handle);

    source_handle = entry->borrowed_from_handle;
    source_generation = entry->borrowed_from_generation;
    if (source_handle > 0 && source_handle - 1 < canonical_type->handle_table.entry_count) {
        source_entry = &canonical_type->handle_table.entries[source_handle - 1];
        if (source_entry->is_live && source_entry->is_owned
            && source_entry->handle == source_handle
            && source_entry->generation == source_generation
            && source_entry->borrow_count > 0)
            source_entry->borrow_count--;
    }

    release_handle_entry(entry);
    if (canonical_type->handle_table.live_handle_count > 0)
        canonical_type->handle_table.live_handle_count--;
    return true;
}

void
wasm_component_resource_release_public_value(
    WASMComponentPublicResourceValue *resource_value)
{
    char error_buf[128] = { 0 };

    if (!resource_value
        || resource_value->magic != WASM_COMPONENT_PUBLIC_RESOURCE_VALUE_MAGIC)
        return;

    switch (resource_value->kind) {
        case WASM_COMPONENT_PUBLIC_RESOURCE_VALUE_PENDING_IMPORTED_RESULT:
            if (resource_value->data && resource_value->finalizer)
                resource_value->finalizer(resource_value->data,
                                          resource_value->finalizer_ctx);
            break;
        case WASM_COMPONENT_PUBLIC_RESOURCE_VALUE_BORROWED:
            if (resource_value->resource_state && resource_value->handle > 0)
                (void)wasm_component_resource_release_borrowed_handle(
                    resource_value->resource_state,
                    resource_value->resource_type_idx, resource_value->handle,
                    error_buf, (uint32)sizeof(error_buf));
            break;
        case WASM_COMPONENT_PUBLIC_RESOURCE_VALUE_TRANSFERRED:
            if (resource_value->resource_state && resource_value->handle > 0
                && wasm_component_resource_restore_owned_handle(
                    resource_value, error_buf, (uint32)sizeof(error_buf)))
                (void)wasm_component_resource_drop_owned_handle(
                    resource_value->resource_state,
                    resource_value->resource_type_idx, resource_value->handle,
                    error_buf, (uint32)sizeof(error_buf));
            break;
        default:
            break;
    }

    memset(resource_value, 0, sizeof(*resource_value));
}
