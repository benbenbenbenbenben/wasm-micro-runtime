/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wasm_component_runtime.h"
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "bh_leb128.h"
#include "wasm_component_resource.h"
#include "wasm_export.h"
#include "wasm_memory.h"
#include "wasm_runtime_common.h"

typedef struct WASMComponentRuntimeAllocCounts {
    uint32 core_module_count;
    uint32 core_instance_count;
    uint32 core_func_count;
    uint32 alias_count;
    uint32 component_count;
    uint32 component_instance_count;
    uint32 component_func_count;
    uint32 lowered_func_count;
    uint32 component_value_count;
    uint32 component_export_count;
} WASMComponentRuntimeAllocCounts;

typedef struct WASMComponentRuntimeScope {
    struct WASMComponentRuntimeScope *parent;
    uint32 component_count;
    WASMComponentRuntimeComponent **components;
} WASMComponentRuntimeScope;

typedef struct WASMNestedComponentLocalBindings {
    uint32 core_module_count;
    uint32 core_module_capacity;
    wasm_module_t *core_modules;
    uint32 core_instance_count;
    uint32 core_instance_capacity;
    WASMComponentCoreRuntimeInstance **core_instances;
    uint32 core_func_count;
    uint32 core_func_capacity;
    WASMComponentCoreRuntimeRef *core_funcs;
    uint32 func_count;
    uint32 func_capacity;
    WASMComponentRuntimeRef *funcs;
    uint32 value_count;
    uint32 value_capacity;
    WASMComponentRuntimeValue **values;
    uint32 instance_count;
    uint32 instance_capacity;
    WASMComponentRuntimeRef *instances;
    uint32 component_count;
    uint32 component_capacity;
    WASMComponentRuntimeComponent **components;
    WASMComponentRuntimeScope *parent_scope;
} WASMNestedComponentLocalBindings;

static uint32
encode_component_unsigned_leb(uint64 value, uint8 *out_buf);

static uint32
encode_component_signed_leb(int64 value, uint8 *out_buf);

static void
set_component_runtime_error(char *error_buf, uint32 error_buf_size,
                            const char *message)
{
    set_error_buf_ex(error_buf, error_buf_size,
                     "WASM component instantiate failed: %s", message);
}

static bool
set_component_runtime_error_fmt(char *error_buf, uint32 error_buf_size,
                                const char *format, ...)
{
    va_list ap;
    char detail[192] = { 0 };

    va_start(ap, format);
    vsnprintf(detail, sizeof(detail), format, ap);
    va_end(ap);

    set_component_runtime_error(error_buf, error_buf_size, detail);
    return false;
}

static bool
set_component_call_error(WASMComponentInstance *inst, const char *message)
{
    wasm_runtime_set_exception((WASMModuleInstanceCommon *)inst, message);
    return false;
}

static bool
set_component_call_error_fmt(WASMComponentInstance *inst, const char *format, ...)
{
    va_list ap;
    char detail[192] = { 0 };

    va_start(ap, format);
    vsnprintf(detail, sizeof(detail), format, ap);
    va_end(ap);

    return set_component_call_error(inst, detail);
}

static bool
alloc_component_scope(WASMComponentRuntimeScope **out_scope,
                      WASMComponentRuntimeScope *parent_scope,
                      WASMComponentRuntimeComponent *const *components,
                      uint32 component_count, char *error_buf,
                      uint32 error_buf_size)
{
    WASMComponentRuntimeScope *scope =
        wasm_runtime_malloc(sizeof(WASMComponentRuntimeScope));
    if (!scope)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "allocate memory failed for component scope");

    memset(scope, 0, sizeof(*scope));
    scope->parent = parent_scope;
    scope->component_count = component_count;
    if (component_count > 0) {
        scope->components = wasm_runtime_malloc(
            sizeof(WASMComponentRuntimeComponent *) * component_count);
        if (!scope->components) {
            wasm_runtime_free(scope);
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "allocate memory failed for %u component scope entries",
                component_count);
        }
        memcpy(scope->components, components,
               sizeof(WASMComponentRuntimeComponent *) * component_count);
    }

    *out_scope = scope;
    return true;
}

static void
collect_component_runtime_alloc_counts(const WASMComponent *component,
                                       WASMComponentRuntimeAllocCounts *counts)
{
    uint32 i;

    memset(counts, 0, sizeof(*counts));

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

                    if (!component_import->extern_desc)
                        continue;

                    switch (component_import->extern_desc->type) {
                        case WASM_COMP_EXTERN_FUNC:
                            counts->component_func_count++;
                            break;
                        case WASM_COMP_EXTERN_VALUE:
                            counts->component_value_count++;
                            break;
                        case WASM_COMP_EXTERN_CORE_MODULE:
                            counts->core_module_count++;
                            break;
                        case WASM_COMP_EXTERN_INSTANCE:
                            counts->component_instance_count++;
                            break;
                        case WASM_COMP_EXTERN_COMPONENT:
                            counts->component_count++;
                            break;
                        default:
                            break;
                    }
                }
                break;
            }
            case WASM_COMP_SECTION_CORE_MODULE:
                counts->core_module_count++;
                break;
            case WASM_COMP_SECTION_CORE_INSTANCE:
                counts->core_instance_count +=
                    section->parsed.core_instance_section->count;
                break;
            case WASM_COMP_SECTION_ALIASES:
            {
                uint32 j;
                const WASMComponentAliasSection *alias_section =
                    section->parsed.alias_section;

                counts->alias_count += alias_section->count;
                for (j = 0; j < alias_section->count; j++) {
                    const WASMComponentAliasDefinition *alias =
                        &alias_section->aliases[j];

                    if (alias->alias_target_type == WASM_COMP_ALIAS_TARGET_CORE_EXPORT
                        && alias->sort
                        && alias->sort->sort == WASM_COMP_SORT_CORE_SORT
                        && alias->sort->core_sort == WASM_COMP_CORE_SORT_FUNC)
                        counts->core_func_count++;

                    if (alias->alias_target_type != WASM_COMP_ALIAS_TARGET_EXPORT
                        || !alias->sort
                        || alias->sort->sort == WASM_COMP_SORT_CORE_SORT)
                        continue;

                    if (alias->sort->sort == WASM_COMP_SORT_FUNC)
                        counts->component_func_count++;
                    else if (alias->sort->sort == WASM_COMP_SORT_VALUE)
                        counts->component_value_count++;
                    else if (alias->sort->sort == WASM_COMP_SORT_INSTANCE)
                        counts->component_instance_count++;
                    else if (alias->sort->sort == WASM_COMP_SORT_COMPONENT)
                        counts->component_count++;
                }
                break;
            }
            case WASM_COMP_SECTION_COMPONENT:
                counts->component_count++;
                break;
            case WASM_COMP_SECTION_INSTANCES:
                counts->component_instance_count +=
                    section->parsed.instance_section->count;
                break;
            case WASM_COMP_SECTION_CANONS:
            {
                uint32 j;
                const WASMComponentCanonSection *canon_section =
                    section->parsed.canon_section;

                for (j = 0; j < canon_section->count; j++) {
                    switch (canon_section->canons[j].tag) {
                        case WASM_COMP_CANON_LIFT:
                            counts->component_func_count++;
                            break;
                        case WASM_COMP_CANON_LOWER:
                            counts->core_func_count++;
                            counts->lowered_func_count++;
                            break;
                        case WASM_COMP_CANON_RESOURCE_NEW:
                        case WASM_COMP_CANON_RESOURCE_DROP:
                        case WASM_COMP_CANON_RESOURCE_DROP_ASYNC:
                        case WASM_COMP_CANON_RESOURCE_REP:
                            counts->core_func_count++;
                            break;
                        default:
                            break;
                    }
                }
                break;
            }
            case WASM_COMP_SECTION_VALUES:
                counts->component_value_count +=
                    section->parsed.value_section->count;
                break;
            case WASM_COMP_SECTION_START:
                counts->component_value_count +=
                    section->parsed.start_section->result;
                break;
            case WASM_COMP_SECTION_EXPORTS:
                counts->component_export_count +=
                    section->parsed.export_section->count;
                break;
            default:
                break;
        }
    }
}

static void
destroy_component_core_instance(WASMComponentCoreRuntimeInstance *core_instance)
{
    if (core_instance->module_inst) {
        wasm_runtime_deinstantiate_internal(
            (WASMModuleInstanceCommon *)core_instance->module_inst, false);
        core_instance->module_inst = NULL;
    }

    if (core_instance->exports) {
        wasm_runtime_free(core_instance->exports);
        core_instance->exports = NULL;
    }
    core_instance->export_count = 0;
}

static void
destroy_component_runtime_instance(WASMComponentRuntimeInstance *component_inst)
{
    uint32 i;

    if (component_inst->owns_resource_state && component_inst->resource_state) {
        wasm_component_resource_state_destroy(component_inst->resource_state);
        component_inst->resource_state = NULL;
    }

    if (component_inst->owned_funcs) {
        wasm_runtime_free(component_inst->owned_funcs);
        component_inst->owned_funcs = NULL;
    }

    if (component_inst->owned_values) {
        for (i = 0; i < component_inst->owned_value_count; i++)
            wasm_component_runtime_value_clear(&component_inst->owned_values[i]);
        wasm_runtime_free(component_inst->owned_values);
        component_inst->owned_values = NULL;
    }

    if (component_inst->owned_core_instances) {
        for (i = 0; i < component_inst->owned_core_instance_count; i++)
            destroy_component_core_instance(
                &component_inst->owned_core_instances[i]);
        wasm_runtime_free(component_inst->owned_core_instances);
        component_inst->owned_core_instances = NULL;
    }

    if (component_inst->owned_lowered_funcs) {
        wasm_runtime_free(component_inst->owned_lowered_funcs);
        component_inst->owned_lowered_funcs = NULL;
    }

    if (component_inst->owned_instances) {
        for (i = 0; i < component_inst->owned_instance_count; i++)
            destroy_component_runtime_instance(&component_inst->owned_instances[i]);
        wasm_runtime_free(component_inst->owned_instances);
        component_inst->owned_instances = NULL;
    }

    if (component_inst->owns_exports && component_inst->exports) {
        wasm_runtime_free(component_inst->exports);
        component_inst->exports = NULL;
    }
    if (component_inst->owned_components) {
        for (i = 0; i < component_inst->owned_component_count; i++) {
            WASMComponentRuntimeScope *scope =
                component_inst->owned_components[i].scope;
            if (component_inst->owned_components[i].owns_scope && scope) {
                if (scope->components)
                    wasm_runtime_free(scope->components);
                wasm_runtime_free(scope);
            }
        }
        wasm_runtime_free(component_inst->owned_components);
        component_inst->owned_components = NULL;
    }
    component_inst->owned_component_count = 0;
    component_inst->owned_core_instance_count = 0;
    component_inst->owned_lowered_func_count = 0;
    component_inst->owned_instance_count = 0;
    component_inst->owned_func_count = 0;
    component_inst->owned_value_count = 0;
    component_inst->owns_exports = false;
    component_inst->owns_resource_state = false;
    component_inst->export_count = 0;
}

static void
destroy_component_instance_graph(WASMComponentInstance *inst)
{
    uint32 i;

    if (!inst)
        return;

    if (inst->core_instances) {
        for (i = 0; i < inst->core_instance_count; i++)
            destroy_component_core_instance(&inst->core_instances[i]);
        wasm_runtime_free(inst->core_instances);
        inst->core_instances = NULL;
    }

    if (inst->core_modules) {
        wasm_runtime_free(inst->core_modules);
        inst->core_modules = NULL;
    }
    if (inst->core_funcs) {
        wasm_runtime_free(inst->core_funcs);
        inst->core_funcs = NULL;
    }
    if (inst->lowered_funcs) {
        wasm_runtime_free(inst->lowered_funcs);
        inst->lowered_funcs = NULL;
    }
    if (inst->core_tables) {
        wasm_runtime_free(inst->core_tables);
        inst->core_tables = NULL;
    }
    if (inst->core_memories) {
        wasm_runtime_free(inst->core_memories);
        inst->core_memories = NULL;
    }
    if (inst->core_globals) {
        wasm_runtime_free(inst->core_globals);
        inst->core_globals = NULL;
    }
    if (inst->resolved_aliases) {
        wasm_runtime_free(inst->resolved_aliases);
        inst->resolved_aliases = NULL;
    }
    if (inst->component_instances) {
        for (i = 0; i < inst->component_instance_count; i++)
            destroy_component_runtime_instance(&inst->component_instances[i]);
        wasm_runtime_free(inst->component_instances);
        inst->component_instances = NULL;
    }
    if (inst->component_exports) {
        wasm_runtime_free(inst->component_exports);
        inst->component_exports = NULL;
    }
    if (inst->components) {
        for (i = 0; i < inst->component_count; i++) {
            WASMComponentRuntimeScope *scope = inst->components[i].scope;
            if (inst->components[i].owns_scope && scope) {
                if (scope->components)
                    wasm_runtime_free(scope->components);
                wasm_runtime_free(scope);
            }
        }
        wasm_runtime_free(inst->components);
        inst->components = NULL;
    }
    if (inst->component_funcs) {
        wasm_runtime_free(inst->component_funcs);
        inst->component_funcs = NULL;
    }
    if (inst->component_values) {
        for (i = 0; i < inst->component_value_count; i++)
            wasm_component_runtime_value_clear(&inst->component_values[i]);
        wasm_runtime_free(inst->component_values);
        inst->component_values = NULL;
    }
    if (inst->resource_state) {
        wasm_component_resource_state_destroy(inst->resource_state);
        inst->resource_state = NULL;
    }

    inst->core_module_count = 0;
    inst->core_instance_count = 0;
    inst->core_func_count = 0;
    inst->lowered_func_count = 0;
    inst->core_table_count = 0;
    inst->core_memory_count = 0;
    inst->core_global_count = 0;
    inst->resolved_alias_count = 0;
    inst->component_count = 0;
    inst->component_func_count = 0;
    inst->component_value_count = 0;
    inst->component_instance_count = 0;
    inst->component_export_count = 0;
}

static bool
alloc_component_runtime_array(void **buffer, uint32 count, uint32 elem_size,
                              char *error_buf, uint32 error_buf_size)
{
    if (count == 0)
        return true;

    *buffer = wasm_runtime_malloc((uint64)count * elem_size);
    if (!*buffer)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "allocate memory failed for %u runtime entries", count);

    memset(*buffer, 0, (uint64)count * elem_size);
    return true;
}

static bool
alloc_nested_component_local_bindings(
    WASMNestedComponentLocalBindings *bindings, uint32 core_module_capacity,
    uint32 core_instance_capacity, uint32 core_func_capacity,
    uint32 func_capacity, uint32 value_capacity, uint32 instance_capacity,
    uint32 component_capacity, char *error_buf, uint32 error_buf_size)
{
    memset(bindings, 0, sizeof(*bindings));

    if (!alloc_component_runtime_array((void **)&bindings->core_modules,
                                       core_module_capacity,
                                       sizeof(*bindings->core_modules),
                                       error_buf, error_buf_size)
        || !alloc_component_runtime_array((void **)&bindings->core_instances,
                                          core_instance_capacity,
                                          sizeof(*bindings->core_instances),
                                          error_buf, error_buf_size)
        || !alloc_component_runtime_array((void **)&bindings->core_funcs,
                                          core_func_capacity,
                                          sizeof(*bindings->core_funcs),
                                          error_buf, error_buf_size)
        || !alloc_component_runtime_array((void **)&bindings->funcs, func_capacity,
                                          sizeof(*bindings->funcs), error_buf,
                                          error_buf_size)
        || !alloc_component_runtime_array((void **)&bindings->values,
                                          value_capacity,
                                          sizeof(*bindings->values), error_buf,
                                          error_buf_size)
        || !alloc_component_runtime_array((void **)&bindings->instances,
                                          instance_capacity,
                                          sizeof(*bindings->instances),
                                          error_buf, error_buf_size)
        || !alloc_component_runtime_array((void **)&bindings->components,
                                          component_capacity,
                                          sizeof(*bindings->components),
                                          error_buf, error_buf_size)) {
        if (bindings->core_modules) {
            wasm_runtime_free(bindings->core_modules);
            bindings->core_modules = NULL;
        }
        if (bindings->core_instances) {
            wasm_runtime_free(bindings->core_instances);
            bindings->core_instances = NULL;
        }
        if (bindings->funcs) {
            wasm_runtime_free(bindings->funcs);
            bindings->funcs = NULL;
        }
        if (bindings->core_funcs) {
            wasm_runtime_free(bindings->core_funcs);
            bindings->core_funcs = NULL;
        }
        if (bindings->values) {
            wasm_runtime_free(bindings->values);
            bindings->values = NULL;
        }
        if (bindings->instances) {
            wasm_runtime_free(bindings->instances);
            bindings->instances = NULL;
        }
        if (bindings->components) {
            wasm_runtime_free(bindings->components);
            bindings->components = NULL;
        }
        return false;
    }

    bindings->core_module_capacity = core_module_capacity;
    bindings->core_instance_capacity = core_instance_capacity;
    bindings->core_func_capacity = core_func_capacity;
    bindings->func_capacity = func_capacity;
    bindings->value_capacity = value_capacity;
    bindings->instance_capacity = instance_capacity;
    bindings->component_capacity = component_capacity;
    return true;
}

static void
free_nested_component_local_bindings(WASMNestedComponentLocalBindings *bindings)
{
    if (bindings->core_modules) {
        wasm_runtime_free(bindings->core_modules);
        bindings->core_modules = NULL;
    }
    if (bindings->core_instances) {
        wasm_runtime_free(bindings->core_instances);
        bindings->core_instances = NULL;
    }
    if (bindings->core_funcs) {
        wasm_runtime_free(bindings->core_funcs);
        bindings->core_funcs = NULL;
    }
    if (bindings->funcs) {
        wasm_runtime_free(bindings->funcs);
        bindings->funcs = NULL;
    }
    if (bindings->values) {
        wasm_runtime_free(bindings->values);
        bindings->values = NULL;
    }
    if (bindings->instances) {
        wasm_runtime_free(bindings->instances);
        bindings->instances = NULL;
    }
    if (bindings->components) {
        wasm_runtime_free(bindings->components);
        bindings->components = NULL;
    }

    bindings->core_module_count = 0;
    bindings->core_module_capacity = 0;
    bindings->core_instance_count = 0;
    bindings->core_instance_capacity = 0;
    bindings->core_func_count = 0;
    bindings->core_func_capacity = 0;
    bindings->func_count = 0;
    bindings->func_capacity = 0;
    bindings->value_count = 0;
    bindings->value_capacity = 0;
    bindings->instance_count = 0;
    bindings->instance_capacity = 0;
    bindings->component_count = 0;
    bindings->component_capacity = 0;
}

static bool
append_nested_component_local_core_module(
    WASMNestedComponentLocalBindings *bindings, wasm_module_t module,
    char *error_buf, uint32 error_buf_size)
{
    if (bindings->core_module_count >= bindings->core_module_capacity)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "nested component local core module space overflow");

    bindings->core_modules[bindings->core_module_count++] = module;
    return true;
}

static bool
append_nested_component_local_core_instance(
    WASMNestedComponentLocalBindings *bindings,
    WASMComponentCoreRuntimeInstance *core_instance, char *error_buf,
    uint32 error_buf_size)
{
    if (bindings->core_instance_count >= bindings->core_instance_capacity)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "nested component local core instance space overflow");

    bindings->core_instances[bindings->core_instance_count++] = core_instance;
    return true;
}

static bool
append_nested_component_local_core_func(
    WASMNestedComponentLocalBindings *bindings, WASMComponentCoreRuntimeRef ref,
    char *error_buf, uint32 error_buf_size)
{
    if (bindings->core_func_count >= bindings->core_func_capacity)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "nested component local core func space overflow");

    bindings->core_funcs[bindings->core_func_count++] = ref;
    return true;
}

static bool
append_nested_component_local_component(WASMNestedComponentLocalBindings *bindings,
                                        WASMComponentRuntimeComponent *component,
                                        char *error_buf,
                                        uint32 error_buf_size);

static bool
append_nested_component_local_value(WASMNestedComponentLocalBindings *bindings,
                                    WASMComponentRuntimeValue *value,
                                    char *error_buf,
                                    uint32 error_buf_size);

static bool
append_nested_component_local_ref(WASMNestedComponentLocalBindings *bindings,
                                  WASMComponentRuntimeRef ref,
                                  char *error_buf, uint32 error_buf_size)
{
    switch (ref.type) {
        case WASM_COMP_RUNTIME_REF_FUNC:
            if (bindings->func_count >= bindings->func_capacity)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "nested component local func space overflow");
            bindings->funcs[bindings->func_count++] = ref;
            return true;
        case WASM_COMP_RUNTIME_REF_VALUE:
            return append_nested_component_local_value(bindings, ref.of.value,
                                                       error_buf,
                                                       error_buf_size);
        case WASM_COMP_RUNTIME_REF_INSTANCE:
            if (bindings->instance_count >= bindings->instance_capacity)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "nested component local instance space overflow");
            bindings->instances[bindings->instance_count++] = ref;
            return true;
        case WASM_COMP_RUNTIME_REF_COMPONENT:
            return append_nested_component_local_component(
                bindings, ref.of.component, error_buf, error_buf_size);
        case WASM_COMP_RUNTIME_REF_CORE_MODULE:
            return append_nested_component_local_core_module(
                bindings, ref.of.core_module, error_buf, error_buf_size);
        default:
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "nested component local ref type %u is not supported yet",
                (unsigned)ref.type);
    }
}

static bool
append_nested_component_local_value(WASMNestedComponentLocalBindings *bindings,
                                    WASMComponentRuntimeValue *value,
                                    char *error_buf,
                                    uint32 error_buf_size)
{
    if (bindings->value_count >= bindings->value_capacity)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "nested component local value space overflow");

    bindings->values[bindings->value_count++] = value;
    return true;
}

static bool
append_nested_component_local_component(WASMNestedComponentLocalBindings *bindings,
                                        WASMComponentRuntimeComponent *component,
                                        char *error_buf,
                                        uint32 error_buf_size)
{
    if (bindings->component_count >= bindings->component_capacity)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "nested component local component space overflow");

    bindings->components[bindings->component_count++] = component;
    return true;
}

static bool
resolve_nested_component_local_sort_idx(
    const WASMNestedComponentLocalBindings *bindings,
    const WASMComponentSortIdx *sort_idx, WASMComponentRuntimeRef *out_ref,
    char *error_buf, uint32 error_buf_size)
{
    if (!sort_idx || !sort_idx->sort)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size, "missing nested component sort index");

    switch (sort_idx->sort->sort) {
        case WASM_COMP_SORT_CORE_SORT:
            if (sort_idx->sort->core_sort != WASM_COMP_CORE_SORT_MODULE)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "only core module sort is supported in nested component "
                    "sort resolution");
            if (sort_idx->idx >= bindings->core_module_count)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "nested component core module index %u is out of bounds",
                    sort_idx->idx);
            memset(out_ref, 0, sizeof(*out_ref));
            out_ref->type = WASM_COMP_RUNTIME_REF_CORE_MODULE;
            out_ref->of.core_module = bindings->core_modules[sort_idx->idx];
            return true;
        case WASM_COMP_SORT_FUNC:
            if (sort_idx->idx >= bindings->func_count)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "nested component func index %u is out of bounds",
                    sort_idx->idx);
            *out_ref = bindings->funcs[sort_idx->idx];
            return true;
        case WASM_COMP_SORT_VALUE:
            if (sort_idx->idx >= bindings->value_count)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "nested component value index %u is out of bounds",
                    sort_idx->idx);
            memset(out_ref, 0, sizeof(*out_ref));
            out_ref->type = WASM_COMP_RUNTIME_REF_VALUE;
            out_ref->of.value = bindings->values[sort_idx->idx];
            return true;
        case WASM_COMP_SORT_INSTANCE:
            if (sort_idx->idx >= bindings->instance_count)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "nested component instance index %u is out of bounds",
                    sort_idx->idx);
            *out_ref = bindings->instances[sort_idx->idx];
            return true;
        case WASM_COMP_SORT_COMPONENT:
            if (sort_idx->idx >= bindings->component_count)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "nested component index %u is out of bounds",
                    sort_idx->idx);
            memset(out_ref, 0, sizeof(*out_ref));
            out_ref->type = WASM_COMP_RUNTIME_REF_COMPONENT;
            out_ref->of.component = bindings->components[sort_idx->idx];
            return true;
        default:
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "nested component sort 0x%02x is not supported yet",
                (unsigned)sort_idx->sort->sort);
    }
}

static bool
resolve_nested_component_local_core_sort_idx(
    const WASMNestedComponentLocalBindings *bindings,
    const WASMComponentSortIdx *sort_idx, WASMComponentCoreRuntimeRef *out_ref,
    char *error_buf, uint32 error_buf_size)
{
    if (!sort_idx || !sort_idx->sort)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size, "missing nested core sort index");

    if (sort_idx->sort->sort != WASM_COMP_SORT_CORE_SORT)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "non-core sort references are not supported in nested core "
            "instance exports");

    switch (sort_idx->sort->core_sort) {
        case WASM_COMP_CORE_SORT_FUNC:
            if (sort_idx->idx >= bindings->core_func_count)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "nested component core func index %u is out of bounds",
                    sort_idx->idx);
            *out_ref = bindings->core_funcs[sort_idx->idx];
            return true;
        case WASM_COMP_CORE_SORT_MODULE:
            if (sort_idx->idx >= bindings->core_module_count)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "nested component core module index %u is out of bounds",
                    sort_idx->idx);
            memset(out_ref, 0, sizeof(*out_ref));
            out_ref->type = WASM_COMP_CORE_RUNTIME_REF_MODULE;
            out_ref->of.module = bindings->core_modules[sort_idx->idx];
            return true;
        case WASM_COMP_CORE_SORT_INSTANCE:
            if (sort_idx->idx >= bindings->core_instance_count)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "nested component core instance index %u is out of bounds",
                    sort_idx->idx);
            memset(out_ref, 0, sizeof(*out_ref));
            out_ref->type = WASM_COMP_CORE_RUNTIME_REF_INSTANCE;
            out_ref->of.instance = bindings->core_instances[sort_idx->idx];
            return true;
        default:
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "nested core instance exports only support core module and "
                "core instance sorts");
    }
}

static bool
resolve_nested_component_local_component_idx(
    const WASMNestedComponentLocalBindings *bindings, uint32 component_idx,
    WASMComponentRuntimeComponent **out_component, char *error_buf,
    uint32 error_buf_size)
{
    if (component_idx >= bindings->component_count)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "nested component index %u is out of bounds", component_idx);

    *out_component = bindings->components[component_idx];
    return true;
}

static bool
resolve_outer_component_alias(WASMComponentRuntimeScope *scope, uint32 ct,
                              uint32 idx,
                              WASMComponentRuntimeComponent **out_component,
                              char *error_buf, uint32 error_buf_size)
{
    uint32 level;

    if (!scope)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "outer alias ct exceeds component nesting depth");

    for (level = 0; level < ct; level++) {
        scope = scope->parent;
        if (!scope)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "outer alias ct exceeds component nesting depth");
    }

    if (idx >= scope->component_count)
        return set_component_runtime_error_fmt(error_buf, error_buf_size,
                                               "outer alias idx %u out of "
                                               "bounds",
                                               idx);

    *out_component = scope->components[idx];
    return true;
}

static bool
append_top_level_component_definition(WASMComponentInstance *inst,
                                      WASMComponent *component, char *error_buf,
                                      uint32 error_buf_size)
{
    WASMComponentRuntimeComponent *runtime_component =
        &inst->components[inst->component_count];
    uint32 i;
    WASMComponentRuntimeComponent **visible_components = NULL;

    if (inst->component_count > 0) {
        visible_components = wasm_runtime_malloc(
            sizeof(WASMComponentRuntimeComponent *) * inst->component_count);
        if (!visible_components)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "allocate memory failed for %u visible component refs",
                inst->component_count);
        for (i = 0; i < inst->component_count; i++)
            visible_components[i] = &inst->components[i];
    }

    memset(runtime_component, 0, sizeof(*runtime_component));
    if (!alloc_component_scope(&runtime_component->scope, NULL, visible_components,
                               inst->component_count, error_buf,
                               error_buf_size)) {
        if (visible_components)
            wasm_runtime_free(visible_components);
        return false;
    }
    if (visible_components)
        wasm_runtime_free(visible_components);

    runtime_component->component = component;
    runtime_component->owns_scope = true;
    inst->component_count++;
    return true;
}

static bool
append_nested_component_definition(WASMComponentRuntimeInstance *runtime_inst,
                                   WASMNestedComponentLocalBindings *bindings,
                                   WASMComponent *component, char *error_buf,
                                   uint32 error_buf_size)
{
    WASMComponentRuntimeComponent *runtime_component =
        &runtime_inst->owned_components[runtime_inst->owned_component_count];

    memset(runtime_component, 0, sizeof(*runtime_component));
    if (!alloc_component_scope(&runtime_component->scope, bindings->parent_scope,
                               bindings->components, bindings->component_count,
                               error_buf, error_buf_size))
        return false;

    runtime_component->component = component;
    runtime_component->owns_scope = true;
    runtime_inst->owned_component_count++;
    return append_nested_component_local_component(bindings, runtime_component,
                                                   error_buf, error_buf_size);
}

static bool
alloc_component_instance_graph(WASMComponentInstance *inst,
                               const WASMComponentRuntimeAllocCounts *counts,
                               char *error_buf, uint32 error_buf_size)
{
    if (!alloc_component_runtime_array((void **)&inst->core_modules,
                                       counts->core_module_count,
                                       sizeof(*inst->core_modules), error_buf,
                                       error_buf_size)
        || !alloc_component_runtime_array((void **)&inst->core_instances,
                                          counts->core_instance_count,
                                          sizeof(*inst->core_instances),
                                          error_buf, error_buf_size)
        || !alloc_component_runtime_array((void **)&inst->core_funcs,
                                          counts->core_func_count,
                                          sizeof(*inst->core_funcs), error_buf,
                                          error_buf_size)
        || !alloc_component_runtime_array((void **)&inst->lowered_funcs,
                                          counts->lowered_func_count,
                                          sizeof(*inst->lowered_funcs), error_buf,
                                          error_buf_size)
        || !alloc_component_runtime_array((void **)&inst->core_tables,
                                          counts->alias_count,
                                          sizeof(*inst->core_tables), error_buf,
                                          error_buf_size)
        || !alloc_component_runtime_array((void **)&inst->core_memories,
                                          counts->alias_count,
                                          sizeof(*inst->core_memories),
                                          error_buf, error_buf_size)
        || !alloc_component_runtime_array((void **)&inst->core_globals,
                                          counts->alias_count,
                                          sizeof(*inst->core_globals),
                                          error_buf, error_buf_size)
        || !alloc_component_runtime_array((void **)&inst->resolved_aliases,
                                          counts->alias_count,
                                          sizeof(*inst->resolved_aliases),
                                          error_buf, error_buf_size)
        || !alloc_component_runtime_array((void **)&inst->components,
                                          counts->component_count,
                                          sizeof(*inst->components), error_buf,
                                          error_buf_size)
        || !alloc_component_runtime_array((void **)&inst->component_funcs,
                                          counts->component_func_count,
                                          sizeof(*inst->component_funcs),
                                          error_buf, error_buf_size)
        || !alloc_component_runtime_array((void **)&inst->component_values,
                                          counts->component_value_count,
                                          sizeof(*inst->component_values),
                                          error_buf, error_buf_size)
        || !alloc_component_runtime_array((void **)&inst->component_instances,
                                          counts->component_instance_count,
                                          sizeof(*inst->component_instances),
                                          error_buf, error_buf_size)
        || !alloc_component_runtime_array((void **)&inst->component_exports,
                                          counts->component_export_count,
                                          sizeof(*inst->component_exports),
                                          error_buf, error_buf_size)) {
        destroy_component_instance_graph(inst);
        return false;
    }

    return true;
}

static bool
resolve_core_sort_idx(const WASMComponentInstance *inst,
                      const WASMComponentSortIdx *sort_idx,
                      WASMComponentCoreRuntimeRef *out_ref, char *error_buf,
                      uint32 error_buf_size)
{
    if (!sort_idx || !sort_idx->sort)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size, "missing core sort index");

    if (sort_idx->sort->sort != WASM_COMP_SORT_CORE_SORT)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "non-core sort references are not supported yet");

    switch (sort_idx->sort->core_sort) {
        case WASM_COMP_CORE_SORT_FUNC:
            if (sort_idx->idx >= inst->core_func_count)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "core func index %u is out of bounds", sort_idx->idx);
            *out_ref = inst->core_funcs[sort_idx->idx];
            return true;
        case WASM_COMP_CORE_SORT_TABLE:
            if (sort_idx->idx >= inst->core_table_count)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "core table index %u is out of bounds", sort_idx->idx);
            *out_ref = inst->core_tables[sort_idx->idx];
            return true;
        case WASM_COMP_CORE_SORT_MEMORY:
            if (sort_idx->idx >= inst->core_memory_count)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "core memory index %u is out of bounds", sort_idx->idx);
            *out_ref = inst->core_memories[sort_idx->idx];
            return true;
        case WASM_COMP_CORE_SORT_GLOBAL:
            if (sort_idx->idx >= inst->core_global_count)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "core global index %u is out of bounds", sort_idx->idx);
            *out_ref = inst->core_globals[sort_idx->idx];
            return true;
        case WASM_COMP_CORE_SORT_MODULE:
            if (sort_idx->idx >= inst->core_module_count)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "core module index %u is out of bounds", sort_idx->idx);
            memset(out_ref, 0, sizeof(*out_ref));
            out_ref->type = WASM_COMP_CORE_RUNTIME_REF_MODULE;
            out_ref->of.module = inst->core_modules[sort_idx->idx];
            return true;
        case WASM_COMP_CORE_SORT_INSTANCE:
            if (sort_idx->idx >= inst->core_instance_count)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "core instance index %u is out of bounds", sort_idx->idx);
            memset(out_ref, 0, sizeof(*out_ref));
            out_ref->type = WASM_COMP_CORE_RUNTIME_REF_INSTANCE;
            out_ref->of.instance = &inst->core_instances[sort_idx->idx];
            return true;
        default:
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "unsupported core sort 0x%02x",
                (unsigned)sort_idx->sort->core_sort);
    }
}

static bool
resolve_component_sort_idx(const WASMComponentInstance *inst,
                           const WASMComponentSortIdx *sort_idx,
                           WASMComponentRuntimeRef *out_ref, char *error_buf,
                           uint32 error_buf_size)
{
    if (!sort_idx || !sort_idx->sort)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size, "missing component sort index");

    if (sort_idx->sort->sort == WASM_COMP_SORT_CORE_SORT) {
        if (sort_idx->sort->core_sort != WASM_COMP_CORE_SORT_MODULE)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "only core module sort is supported in component sort "
                "resolution");

        if (sort_idx->idx >= inst->core_module_count)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "core module index %u is out of bounds", sort_idx->idx);

        out_ref->type = WASM_COMP_RUNTIME_REF_CORE_MODULE;
        out_ref->of.core_module = inst->core_modules[sort_idx->idx];
        return true;
    }

    switch (sort_idx->sort->sort) {
        case WASM_COMP_SORT_FUNC:
            if (sort_idx->idx >= inst->component_func_count)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component func index %u is out of bounds", sort_idx->idx);
            out_ref->type = WASM_COMP_RUNTIME_REF_FUNC;
            out_ref->of.function = &inst->component_funcs[sort_idx->idx];
            return true;
        case WASM_COMP_SORT_VALUE:
            if (sort_idx->idx >= inst->component_value_count)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component value index %u is out of bounds", sort_idx->idx);
            out_ref->type = WASM_COMP_RUNTIME_REF_VALUE;
            out_ref->of.value = &inst->component_values[sort_idx->idx];
            return true;
        case WASM_COMP_SORT_INSTANCE:
            if (sort_idx->idx >= inst->component_instance_count)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component instance index %u is out of bounds",
                    sort_idx->idx);
            out_ref->type = WASM_COMP_RUNTIME_REF_INSTANCE;
            out_ref->of.instance = &inst->component_instances[sort_idx->idx];
            return true;
        case WASM_COMP_SORT_COMPONENT:
            if (sort_idx->idx >= inst->component_count)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component index %u is out of bounds", sort_idx->idx);
            out_ref->type = WASM_COMP_RUNTIME_REF_COMPONENT;
            out_ref->of.component = &inst->components[sort_idx->idx];
            return true;
        default:
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "component sort 0x%02x is not supported yet",
                (unsigned)sort_idx->sort->sort);
    }
}

static const char *
get_component_import_name(const WASMComponentImportName *import_name)
{
    if (!import_name)
        return NULL;

    return import_name->tag == WASM_COMP_IMPORTNAME_VERSIONED
               ? import_name->imported.versioned.name->name
               : import_name->imported.simple.name->name;
}

static const char *
get_component_export_name(const WASMComponentExportName *export_name)
{
    if (!export_name)
        return NULL;

    return export_name->tag == WASM_COMP_IMPORTNAME_VERSIONED
               ? export_name->exported.versioned.name->name
               : export_name->exported.simple.name->name;
}

static bool
component_runtime_ref_to_public_export(const char *name,
                                       WASMComponentRuntimeRef ref,
                                       wasm_component_export_t *export_type)
{
    if (!export_type)
        return false;

    memset(export_type, 0, sizeof(*export_type));
    export_type->name = name;

    switch (ref.type) {
        case WASM_COMP_RUNTIME_REF_FUNC:
            export_type->kind = WASM_COMPONENT_EXTERN_KIND_FUNC;
            export_type->value.function = ref.of.function;
            return true;
        case WASM_COMP_RUNTIME_REF_VALUE:
            export_type->kind = WASM_COMPONENT_EXTERN_KIND_VALUE;
            return true;
        case WASM_COMP_RUNTIME_REF_INSTANCE:
            export_type->kind = WASM_COMPONENT_EXTERN_KIND_INSTANCE;
            export_type->value.instance = ref.of.instance;
            return true;
        case WASM_COMP_RUNTIME_REF_COMPONENT:
            export_type->kind = WASM_COMPONENT_EXTERN_KIND_COMPONENT;
            export_type->value.component = ref.of.component;
            return true;
        case WASM_COMP_RUNTIME_REF_CORE_MODULE:
            export_type->kind = WASM_COMPONENT_EXTERN_KIND_CORE_MODULE;
            export_type->value.core_module = ref.of.core_module;
            return true;
        default:
            return false;
    }
}

static bool
component_named_export_to_public(const WASMComponentNamedExport *export_item,
                                 wasm_component_export_t *export_type)
{
    return component_runtime_ref_to_public_export(export_item->name,
                                                  export_item->ref, export_type);
}

static bool
lookup_component_named_export(const WASMComponentNamedExport *exports,
                              uint32 export_count, const char *name,
                              WASMComponentRuntimeRefType expected_type,
                              WASMComponentRuntimeRef *out_ref)
{
    uint32 i;

    if (!name)
        return false;

    for (i = 0; i < export_count; i++) {
        const WASMComponentNamedExport *export_item = &exports[i];

        if (!strcmp(export_item->name, name)
            && export_item->ref.type == expected_type) {
            if (out_ref)
                *out_ref = export_item->ref;
            return true;
        }
    }

    return false;
}

static bool
component_instance_export_to_public(const WASMComponentRuntimeInstance *component_inst,
                                    int32 export_index,
                                    wasm_component_export_t *export_type)
{
    if (!component_inst || export_index < 0
        || (uint32)export_index >= component_inst->export_count)
        return false;

    return component_named_export_to_public(&component_inst->exports[export_index],
                                            export_type);
}

int32
wasm_component_get_export_count(const WASMComponentInstance *inst)
{
    return inst ? (int32)inst->component_export_count : -1;
}

bool
wasm_component_get_export_type(const WASMComponentInstance *inst,
                               int32 export_index,
                               wasm_component_export_t *export_type)
{
    if (!inst || export_index < 0
        || (uint32)export_index >= inst->component_export_count)
        return false;

    return component_named_export_to_public(&inst->component_exports[export_index],
                                            export_type);
}

bool
wasm_component_get_export_value(const WASMComponentInstance *inst,
                                int32 export_index,
                                wasm_component_value_t *value,
                                char *error_buf, uint32 error_buf_size)
{
    const WASMComponentNamedExport *export_item;

    if (value)
        wasm_component_value_destroy(value);

    if (!inst || !value || export_index < 0
        || (uint32)export_index >= inst->component_export_count)
        return false;

    export_item = &inst->component_exports[export_index];
    if (export_item->ref.type != WASM_COMP_RUNTIME_REF_VALUE)
        return false;

    return wasm_component_public_value_copy(value, export_item->ref.of.value,
                                            error_buf, error_buf_size);
}

int32
wasm_component_instance_get_export_count(
    const wasm_component_instance_t component_inst)
{
    return component_inst ? (int32)component_inst->export_count : -1;
}

bool
wasm_component_instance_get_export_type(
    const wasm_component_instance_t component_inst, int32 export_index,
    wasm_component_export_t *export_type)
{
    return component_instance_export_to_public(component_inst, export_index,
                                               export_type);
}

WASMComponentRuntimeFunc *
wasm_component_lookup_function(const WASMComponentInstance *inst,
                               const char *name)
{
    WASMComponentRuntimeRef ref;

    return inst
               && lookup_component_named_export(
                   inst->component_exports, inst->component_export_count, name,
                   WASM_COMP_RUNTIME_REF_FUNC, &ref)
            ? ref.of.function
           : NULL;
}

bool
wasm_component_lookup_value(const WASMComponentInstance *inst, const char *name,
                            wasm_component_value_t *value, char *error_buf,
                            uint32 error_buf_size)
{
    WASMComponentRuntimeRef ref;

    if (value)
        wasm_component_value_destroy(value);

    if (!inst || !value
        || !lookup_component_named_export(inst->component_exports,
                                          inst->component_export_count, name,
                                          WASM_COMP_RUNTIME_REF_VALUE, &ref))
        return false;

    return wasm_component_public_value_copy(value, ref.of.value, error_buf,
                                            error_buf_size);
}

WASMComponentRuntimeInstance *
wasm_component_lookup_instance(const WASMComponentInstance *inst,
                               const char *name)
{
    WASMComponentRuntimeRef ref;

    return inst
               && lookup_component_named_export(
                   inst->component_exports, inst->component_export_count, name,
                   WASM_COMP_RUNTIME_REF_INSTANCE, &ref)
           ? ref.of.instance
           : NULL;
}

WASMComponentRuntimeComponent *
wasm_component_lookup_component(const WASMComponentInstance *inst,
                                const char *name)
{
    WASMComponentRuntimeRef ref;

    return inst
               && lookup_component_named_export(
                   inst->component_exports, inst->component_export_count, name,
                   WASM_COMP_RUNTIME_REF_COMPONENT, &ref)
           ? ref.of.component
           : NULL;
}

wasm_module_t
wasm_component_lookup_core_module(const WASMComponentInstance *inst,
                                  const char *name)
{
    WASMComponentRuntimeRef ref;

    return inst
               && lookup_component_named_export(
                   inst->component_exports, inst->component_export_count, name,
                   WASM_COMP_RUNTIME_REF_CORE_MODULE, &ref)
           ? ref.of.core_module
           : NULL;
}

wasm_component_func_t
wasm_component_instance_lookup_function(
    const wasm_component_instance_t component_inst, const char *name)
{
    WASMComponentRuntimeRef ref;

    return component_inst
               && lookup_component_named_export(component_inst->exports,
                                                component_inst->export_count,
                                                name,
                                                WASM_COMP_RUNTIME_REF_FUNC,
                                                &ref)
           ? ref.of.function
           : NULL;
}

wasm_component_instance_t
wasm_component_instance_lookup_instance(
    const wasm_component_instance_t component_inst, const char *name)
{
    WASMComponentRuntimeRef ref;

    return component_inst
               && lookup_component_named_export(component_inst->exports,
                                                component_inst->export_count,
                                                name,
                                                WASM_COMP_RUNTIME_REF_INSTANCE,
                                                &ref)
           ? ref.of.instance
           : NULL;
}

wasm_component_component_t
wasm_component_instance_lookup_component(
    const wasm_component_instance_t component_inst, const char *name)
{
    WASMComponentRuntimeRef ref;

    return component_inst
               && lookup_component_named_export(component_inst->exports,
                                                component_inst->export_count,
                                                name,
                                                WASM_COMP_RUNTIME_REF_COMPONENT,
                                                &ref)
           ? ref.of.component
           : NULL;
}

wasm_module_t
wasm_component_instance_lookup_core_module(
    const wasm_component_instance_t component_inst, const char *name)
{
    WASMComponentRuntimeRef ref;

    return component_inst
               && lookup_component_named_export(component_inst->exports,
                                                component_inst->export_count,
                                                name,
                                                WASM_COMP_RUNTIME_REF_CORE_MODULE,
                                                &ref)
           ? ref.of.core_module
           : NULL;
}

static const char *
component_prim_type_name(uint8 prim_type)
{
    switch (prim_type) {
        case WASM_COMP_PRIMVAL_BOOL:
            return "bool";
        case WASM_COMP_PRIMVAL_S8:
            return "s8";
        case WASM_COMP_PRIMVAL_U8:
            return "u8";
        case WASM_COMP_PRIMVAL_S16:
            return "s16";
        case WASM_COMP_PRIMVAL_U16:
            return "u16";
        case WASM_COMP_PRIMVAL_S32:
            return "s32";
        case WASM_COMP_PRIMVAL_U32:
            return "u32";
        case WASM_COMP_PRIMVAL_S64:
            return "s64";
        case WASM_COMP_PRIMVAL_U64:
            return "u64";
        case WASM_COMP_PRIMVAL_F32:
            return "f32";
        case WASM_COMP_PRIMVAL_F64:
            return "f64";
        case WASM_COMP_PRIMVAL_CHAR:
            return "char";
        case WASM_COMP_PRIMVAL_STRING:
            return "string";
        case WASM_COMP_PRIMVAL_ERROR_CONTEXT:
            return "error-context";
        default:
            return "unknown";
    }
}

static const char *
component_def_type_name(WASMComponentDefValTypeTag tag)
{
    switch (tag) {
        case WASM_COMP_DEF_VAL_RECORD:
            return "record";
        case WASM_COMP_DEF_VAL_VARIANT:
            return "variant";
        case WASM_COMP_DEF_VAL_LIST:
            return "list";
        case WASM_COMP_DEF_VAL_LIST_LEN:
            return "fixed-length list";
        case WASM_COMP_DEF_VAL_TUPLE:
            return "tuple";
        case WASM_COMP_DEF_VAL_FLAGS:
            return "flags";
        case WASM_COMP_DEF_VAL_ENUM:
            return "enum";
        case WASM_COMP_DEF_VAL_OPTION:
            return "option";
        case WASM_COMP_DEF_VAL_RESULT:
            return "result";
        case WASM_COMP_DEF_VAL_OWN:
            return "own";
        case WASM_COMP_DEF_VAL_BORROW:
            return "borrow";
        case WASM_COMP_DEF_VAL_STREAM:
            return "stream";
        case WASM_COMP_DEF_VAL_FUTURE:
            return "future";
        default:
            return "non-scalar";
    }
}

static bool
component_scalar_prim_to_core(uint8 prim_type, uint8 *core_type,
                              wasm_valkind_t *public_kind)
{
    switch (prim_type) {
        case WASM_COMP_PRIMVAL_BOOL:
        case WASM_COMP_PRIMVAL_S8:
        case WASM_COMP_PRIMVAL_U8:
        case WASM_COMP_PRIMVAL_S16:
        case WASM_COMP_PRIMVAL_U16:
        case WASM_COMP_PRIMVAL_S32:
        case WASM_COMP_PRIMVAL_U32:
        case WASM_COMP_PRIMVAL_CHAR:
            *core_type = VALUE_TYPE_I32;
            *public_kind = WASM_I32;
            return true;
        case WASM_COMP_PRIMVAL_S64:
        case WASM_COMP_PRIMVAL_U64:
            *core_type = VALUE_TYPE_I64;
            *public_kind = WASM_I64;
            return true;
        case WASM_COMP_PRIMVAL_F32:
            *core_type = VALUE_TYPE_F32;
            *public_kind = WASM_F32;
            return true;
        case WASM_COMP_PRIMVAL_F64:
            *core_type = VALUE_TYPE_F64;
            *public_kind = WASM_F64;
            return true;
        default:
            return false;
    }
}

typedef enum WASMComponentCanonLiftValueKind {
    WASM_COMP_CANON_LIFT_VALUE_SCALAR = 0,
    WASM_COMP_CANON_LIFT_VALUE_STRING,
    WASM_COMP_CANON_LIFT_VALUE_LIST_U8
} WASMComponentCanonLiftValueKind;

typedef struct WASMComponentCanonLiftValueInfo {
    WASMComponentCanonLiftValueKind kind;
    bool declared_as_defined;
    uint8 prim_type;
    uint8 core_type;
    wasm_valkind_t public_kind;
} WASMComponentCanonLiftValueInfo;

typedef struct WASMComponentCanonLiftValueShape {
    bool declared_as_defined;
    bool is_primitive;
    uint8 prim_type;
    const WASMComponentDefValType *def_type;
} WASMComponentCanonLiftValueShape;

typedef struct WASMComponentCanonParamAllocation {
    uint32 ptr;
    uint32 size;
} WASMComponentCanonParamAllocation;

typedef struct WASMComponentCanonParamAllocationTracker {
    WASMComponentCanonParamAllocation *allocations;
    uint32 count;
    uint32 capacity;
} WASMComponentCanonParamAllocationTracker;

static bool
lookup_component_call_primitive_type(const WASMComponent *component,
                                     const WASMComponentValueType *value_type,
                                     const char *position, uint32 index,
                                     bool *is_primitive, uint8 *prim_type_out,
                                     WASMComponentInstance *inst)
{
    const WASMComponentTypes *type_entry;
    WASMComponentDefValType *def_type;

    *is_primitive = false;

    if (!value_type)
        return set_component_call_error_fmt(
            inst, "component canon lift function %s %u is missing a type",
            position, index);

    if (value_type->type == WASM_COMP_VAL_TYPE_PRIMVAL) {
        *is_primitive = true;
        *prim_type_out = value_type->type_specific.primval_type;
        return true;
    }

    type_entry =
        wasm_component_lookup_type(component, value_type->type_specific.type_idx);
    if (!type_entry)
        return set_component_call_error_fmt(
            inst,
            "component canon lift function %s %u uses unresolved type index %u",
            position, index, value_type->type_specific.type_idx);

    if (type_entry->tag != WASM_COMP_DEF_TYPE || !type_entry->type.def_val_type)
        return set_component_call_error_fmt(
            inst,
            "component canon lift function %s %u uses non-value type index %u",
            position, index, value_type->type_specific.type_idx);

    def_type = type_entry->type.def_val_type;
    if (def_type->tag == WASM_COMP_DEF_VAL_PRIMVAL) {
        *is_primitive = true;
        *prim_type_out = def_type->def_val.primval;
    }
    return true;
}

static bool
resolve_component_runtime_primitive_type(const WASMComponent *component,
                                         const WASMComponentValueType *value_type,
                                         bool *is_primitive,
                                         uint8 *prim_type_out, char *error_buf,
                                         uint32 error_buf_size)
{
    const WASMComponentTypes *type_entry;
    WASMComponentDefValType *def_type;

    *is_primitive = false;

    if (!value_type)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component canon lift function is missing a value type");

    if (value_type->type == WASM_COMP_VAL_TYPE_PRIMVAL) {
        *is_primitive = true;
        *prim_type_out = value_type->type_specific.primval_type;
        return true;
    }

    type_entry =
        wasm_component_lookup_type(component, value_type->type_specific.type_idx);
    if (!type_entry)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component canon lift function uses unresolved type index %u",
            value_type->type_specific.type_idx);

    if (type_entry->tag != WASM_COMP_DEF_TYPE || !type_entry->type.def_val_type)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component canon lift function uses non-value type index %u",
            value_type->type_specific.type_idx);

    def_type = type_entry->type.def_val_type;
    if (def_type->tag == WASM_COMP_DEF_VAL_PRIMVAL) {
        *is_primitive = true;
        *prim_type_out = def_type->def_val.primval;
    }
    return true;
}

static bool
resolve_component_canon_lift_value_shape(const WASMComponent *component,
                                         const WASMComponentValueType *value_type,
                                         const char *position, uint32 index,
                                         WASMComponentCanonLiftValueShape *out_shape,
                                         WASMComponentInstance *inst)
{
    const WASMComponentTypes *type_entry;

    memset(out_shape, 0, sizeof(*out_shape));

    if (!value_type)
        return set_component_call_error_fmt(
            inst, "component canon lift function %s %u is missing a type",
            position, index);

    if (value_type->type == WASM_COMP_VAL_TYPE_PRIMVAL) {
        out_shape->is_primitive = true;
        out_shape->prim_type = value_type->type_specific.primval_type;
        return true;
    }

    out_shape->declared_as_defined = true;
    type_entry =
        wasm_component_lookup_type(component, value_type->type_specific.type_idx);
    if (!type_entry)
        return set_component_call_error_fmt(
            inst,
            "component canon lift function %s %u uses unresolved type index %u",
            position, index, value_type->type_specific.type_idx);

    if (type_entry->tag != WASM_COMP_DEF_TYPE || !type_entry->type.def_val_type)
        return set_component_call_error_fmt(
            inst,
            "component canon lift function %s %u uses non-value type index %u",
            position, index, value_type->type_specific.type_idx);

    if (type_entry->type.def_val_type->tag == WASM_COMP_DEF_VAL_PRIMVAL) {
        out_shape->is_primitive = true;
        out_shape->prim_type = type_entry->type.def_val_type->def_val.primval;
    }
    else {
        out_shape->def_type = type_entry->type.def_val_type;
    }

    return true;
}

static bool
resolve_component_lift_list_u8_usage(const WASMComponent *component,
                                     const WASMComponentValueType *value_type,
                                     bool *is_list_u8, char *error_buf,
                                     uint32 error_buf_size)
{
    const WASMComponentTypes *type_entry;
    WASMComponentDefValType *def_type;
    bool is_primitive = false;
    uint8 prim_type = 0;

    *is_list_u8 = false;

    if (!value_type)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component canon lift function is missing a value type");

    if (value_type->type == WASM_COMP_VAL_TYPE_PRIMVAL)
        return true;

    type_entry =
        wasm_component_lookup_type(component, value_type->type_specific.type_idx);
    if (!type_entry)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component canon lift function uses unresolved type index %u",
            value_type->type_specific.type_idx);

    if (type_entry->tag != WASM_COMP_DEF_TYPE || !type_entry->type.def_val_type)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component canon lift function uses non-value type index %u",
            value_type->type_specific.type_idx);

    def_type = type_entry->type.def_val_type;
    if (def_type->tag != WASM_COMP_DEF_VAL_LIST || !def_type->def_val.list
        || !def_type->def_val.list->element_type)
        return true;

    if (!resolve_component_runtime_primitive_type(component,
                                                  def_type->def_val.list->element_type,
                                                  &is_primitive, &prim_type,
                                                  error_buf, error_buf_size))
        return false;

    *is_list_u8 = is_primitive && prim_type == WASM_COMP_PRIMVAL_U8;
    return true;
}

static bool
classify_component_runtime_composite_param(const WASMComponent *component,
                                           const WASMComponentValueType *value_type,
                                           uint32 param_index,
                                           bool *has_string_leaf_out,
                                           bool *has_list_u8_leaf_out,
                                           char *error_buf,
                                           uint32 error_buf_size)
{
    const WASMComponentTypes *type_entry;
    WASMComponentDefValType *def_type;
    bool is_primitive = false;
    uint8 prim_type = 0;

    *has_string_leaf_out = false;
    *has_list_u8_leaf_out = false;

    if (!resolve_component_runtime_primitive_type(component, value_type,
                                                  &is_primitive, &prim_type,
                                                  error_buf, error_buf_size))
        return false;

    if (is_primitive) {
        if (prim_type == WASM_COMP_PRIMVAL_STRING) {
            *has_string_leaf_out = true;
            return true;
        }
        else {
            uint8 ignored_core_type = 0;
            wasm_valkind_t ignored_public_kind = WASM_I32;

            return component_scalar_prim_to_core(prim_type, &ignored_core_type,
                                                 &ignored_public_kind)
                       ? true
                       : set_component_runtime_error_fmt(
                              error_buf, error_buf_size,
                              "component canon lift function parameter %u only "
                              "supports tuple/record parameters with scalar, "
                              "UTF-8 string, or variable-length list<u8> leaves",
                              param_index);
        }
    }

    type_entry =
        wasm_component_lookup_type(component, value_type->type_specific.type_idx);
    if (!type_entry)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component canon lift function uses unresolved type index %u",
            value_type->type_specific.type_idx);

    if (type_entry->tag != WASM_COMP_DEF_TYPE || !type_entry->type.def_val_type)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component canon lift function uses non-value type index %u",
            value_type->type_specific.type_idx);

    def_type = type_entry->type.def_val_type;
    switch (def_type->tag) {
        case WASM_COMP_DEF_VAL_RECORD:
            if (!def_type->def_val.record)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component canon lift function parameter %u only supports "
                    "tuple/record parameters with scalar, UTF-8 string, or "
                    "variable-length list<u8> leaves",
                    param_index);
            for (uint32 i = 0; i < def_type->def_val.record->count; i++) {
                bool nested_has_string = false;
                bool nested_has_list_u8 = false;

                if (!classify_component_runtime_composite_param(
                        component, def_type->def_val.record->fields[i].value_type,
                        param_index, &nested_has_string, &nested_has_list_u8,
                        error_buf,
                        error_buf_size))
                    return false;
                *has_string_leaf_out = *has_string_leaf_out || nested_has_string;
                *has_list_u8_leaf_out =
                    *has_list_u8_leaf_out || nested_has_list_u8;
            }
            return true;
        case WASM_COMP_DEF_VAL_TUPLE:
            if (!def_type->def_val.tuple)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component canon lift function parameter %u only supports "
                    "tuple/record parameters with scalar, UTF-8 string, or "
                    "variable-length list<u8> leaves",
                    param_index);
            for (uint32 i = 0; i < def_type->def_val.tuple->count; i++) {
                bool nested_has_string = false;
                bool nested_has_list_u8 = false;

                if (!classify_component_runtime_composite_param(
                        component, &def_type->def_val.tuple->element_types[i],
                        param_index, &nested_has_string, &nested_has_list_u8,
                        error_buf,
                        error_buf_size))
                    return false;
                *has_string_leaf_out = *has_string_leaf_out || nested_has_string;
                *has_list_u8_leaf_out =
                    *has_list_u8_leaf_out || nested_has_list_u8;
            }
            return true;
        case WASM_COMP_DEF_VAL_LIST:
            if (!def_type->def_val.list || !def_type->def_val.list->element_type)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component canon lift function parameter %u uses malformed "
                    "list type",
                    param_index);
            if (!resolve_component_runtime_primitive_type(
                    component, def_type->def_val.list->element_type,
                    &is_primitive, &prim_type, error_buf, error_buf_size))
                return false;
            if (!is_primitive || prim_type != WASM_COMP_PRIMVAL_U8)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component canon lift function parameter %u only supports "
                    "variable-length list<u8> leaves inside tuple/record "
                    "parameters",
                    param_index);
            *has_list_u8_leaf_out = true;
            return true;
        case WASM_COMP_DEF_VAL_LIST_LEN:
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "component canon lift function parameter %u only supports "
                "variable-length list<u8> leaves inside tuple/record parameters",
                param_index);
        default:
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "component canon lift function parameter %u only supports "
                "tuple/record parameters with scalar, UTF-8 string, or "
                "variable-length list<u8> leaves",
                param_index);
    }
}

static bool
classify_component_runtime_composite_result(const WASMComponent *component,
                                            const WASMComponentValueType *value_type,
                                             bool *has_string_leaf_out,
                                            bool *has_list_u8_leaf_out,
                                            char *error_buf,
                                            uint32 error_buf_size)
{
    const WASMComponentTypes *type_entry;
    WASMComponentDefValType *def_type;
    bool is_primitive = false;
    uint8 prim_type = 0;

    *has_string_leaf_out = false;
    *has_list_u8_leaf_out = false;

    if (!resolve_component_runtime_primitive_type(component, value_type,
                                                  &is_primitive, &prim_type,
                                                  error_buf, error_buf_size))
        return false;

    if (is_primitive) {
        if (prim_type == WASM_COMP_PRIMVAL_STRING) {
            *has_string_leaf_out = true;
            return true;
        }
        else {
            uint8 ignored_core_type = 0;
            wasm_valkind_t ignored_public_kind = WASM_I32;

            return component_scalar_prim_to_core(prim_type, &ignored_core_type,
                                                 &ignored_public_kind)
                       ? true
                        : set_component_runtime_error_fmt(
                              error_buf, error_buf_size,
                              "component canon lift function result 0 only "
                              "supports tuple/record results with scalar, "
                              "UTF-8 string, or variable-length list<u8> "
                              "leaves");
        }
    }

    type_entry =
        wasm_component_lookup_type(component, value_type->type_specific.type_idx);
    if (!type_entry)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component canon lift function uses unresolved type index %u",
            value_type->type_specific.type_idx);

    if (type_entry->tag != WASM_COMP_DEF_TYPE || !type_entry->type.def_val_type)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component canon lift function uses non-value type index %u",
            value_type->type_specific.type_idx);

    def_type = type_entry->type.def_val_type;
    switch (def_type->tag) {
        case WASM_COMP_DEF_VAL_RECORD:
            if (!def_type->def_val.record)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component canon lift function result 0 only supports "
                    "tuple/record results with scalar or UTF-8 string leaves");
            for (uint32 i = 0; i < def_type->def_val.record->count; i++) {
                bool nested_has_string = false;
                bool nested_has_list_u8 = false;

                if (!classify_component_runtime_composite_result(
                        component, def_type->def_val.record->fields[i].value_type,
                        &nested_has_string, &nested_has_list_u8, error_buf,
                        error_buf_size))
                    return false;
                *has_string_leaf_out = *has_string_leaf_out || nested_has_string;
                *has_list_u8_leaf_out =
                    *has_list_u8_leaf_out || nested_has_list_u8;
            }
            return true;
        case WASM_COMP_DEF_VAL_TUPLE:
            if (!def_type->def_val.tuple)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component canon lift function result 0 only supports "
                    "tuple/record results with scalar or UTF-8 string leaves");
            for (uint32 i = 0; i < def_type->def_val.tuple->count; i++) {
                bool nested_has_string = false;
                bool nested_has_list_u8 = false;

                if (!classify_component_runtime_composite_result(
                        component, &def_type->def_val.tuple->element_types[i],
                        &nested_has_string, &nested_has_list_u8, error_buf,
                        error_buf_size))
                    return false;
                *has_string_leaf_out = *has_string_leaf_out || nested_has_string;
                *has_list_u8_leaf_out =
                    *has_list_u8_leaf_out || nested_has_list_u8;
            }
            return true;
        case WASM_COMP_DEF_VAL_LIST:
            if (!def_type->def_val.list || !def_type->def_val.list->element_type)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component canon lift function result 0 uses malformed list "
                    "type");
            if (!resolve_component_runtime_primitive_type(
                    component, def_type->def_val.list->element_type,
                    &is_primitive, &prim_type, error_buf, error_buf_size))
                return false;
            if (!is_primitive || prim_type != WASM_COMP_PRIMVAL_U8)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component canon lift function result 0 only supports "
                    "variable-length list<u8> leaves inside tuple/record "
                    "results");
            *has_list_u8_leaf_out = true;
            return true;
        case WASM_COMP_DEF_VAL_LIST_LEN:
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "component canon lift function result 0 only supports "
                "variable-length list<u8> leaves inside tuple/record results");
        default:
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "component canon lift function result 0 only supports "
                "tuple/record results with scalar, UTF-8 string, or "
                "variable-length list<u8> leaves");
    }
}

static bool
lookup_component_canon_lift_value_type(const WASMComponent *component,
                                       const WASMComponentValueType *value_type,
                                       const char *position, uint32 index,
                                       bool allow_string,
                                       bool allow_list_u8_param,
                                       bool allow_list_u8_result,
                                       WASMComponentCanonLiftValueInfo *out_info,
                                       WASMComponentInstance *inst)
{
    const WASMComponentTypes *type_entry;
    WASMComponentDefValType *def_type;
    uint8 prim_type;
    uint8 core_type = 0;
    wasm_valkind_t public_kind = WASM_I32;
    bool declared_as_defined = false;

    if (!value_type)
        return set_component_call_error_fmt(
            inst, "component canon lift function %s %u is missing a type",
            position, index);

    if (value_type->type == WASM_COMP_VAL_TYPE_PRIMVAL) {
        prim_type = value_type->type_specific.primval_type;
        if (!component_scalar_prim_to_core(prim_type, &core_type, &public_kind)) {
            if (prim_type == WASM_COMP_PRIMVAL_STRING) {
                if (allow_string) {
                    memset(out_info, 0, sizeof(*out_info));
                    out_info->kind = WASM_COMP_CANON_LIFT_VALUE_STRING;
                    out_info->prim_type = prim_type;
                    return true;
                }
                return set_component_call_error_fmt(
                    inst,
                    "component canon lift function %s %u requires memory-backed "
                    "Canonical ABI for string",
                    position, index);
            }
            if (prim_type == WASM_COMP_PRIMVAL_ERROR_CONTEXT)
                return set_component_call_error_fmt(
                    inst,
                    "component canon lift function %s %u uses unsupported "
                    "component scalar type error-context",
                    position, index);
            return set_component_call_error_fmt(
                inst, "component canon lift function %s %u uses unsupported "
                       "component scalar type %s",
                position, index, component_prim_type_name(prim_type));
        }
        memset(out_info, 0, sizeof(*out_info));
        out_info->kind = WASM_COMP_CANON_LIFT_VALUE_SCALAR;
        out_info->declared_as_defined = false;
        out_info->prim_type = prim_type;
        out_info->core_type = core_type;
        out_info->public_kind = public_kind;
        return true;
    }

    declared_as_defined = true;
    type_entry =
        wasm_component_lookup_type(component, value_type->type_specific.type_idx);
    if (!type_entry)
        return set_component_call_error_fmt(
            inst,
            "component canon lift function %s %u uses unresolved type index %u",
            position, index, value_type->type_specific.type_idx);

    if (type_entry->tag != WASM_COMP_DEF_TYPE)
        return set_component_call_error_fmt(
            inst,
            "component canon lift function %s %u uses non-value type index %u",
            position, index, value_type->type_specific.type_idx);

    def_type = type_entry->type.def_val_type;
    if (def_type->tag == WASM_COMP_DEF_VAL_PRIMVAL) {
        prim_type = def_type->def_val.primval;
        if (!component_scalar_prim_to_core(prim_type, &core_type, &public_kind)) {
            if (prim_type == WASM_COMP_PRIMVAL_STRING) {
                if (allow_string) {
                    memset(out_info, 0, sizeof(*out_info));
                    out_info->kind = WASM_COMP_CANON_LIFT_VALUE_STRING;
                    out_info->declared_as_defined = true;
                    out_info->prim_type = prim_type;
                    return true;
                }
                return set_component_call_error_fmt(
                    inst,
                    "component canon lift function %s %u requires memory-backed "
                    "Canonical ABI for string",
                    position, index);
            }
            if (prim_type == WASM_COMP_PRIMVAL_ERROR_CONTEXT)
                return set_component_call_error_fmt(
                    inst,
                    "component canon lift function %s %u uses unsupported "
                    "component scalar type error-context",
                    position, index);
            return set_component_call_error_fmt(
                inst, "component canon lift function %s %u uses unsupported "
                       "component scalar type %s",
                position, index, component_prim_type_name(prim_type));
        }
        memset(out_info, 0, sizeof(*out_info));
        out_info->kind = WASM_COMP_CANON_LIFT_VALUE_SCALAR;
        out_info->declared_as_defined = declared_as_defined;
        out_info->prim_type = prim_type;
        out_info->core_type = core_type;
        out_info->public_kind = public_kind;
        return true;
    }

    if (def_type->tag == WASM_COMP_DEF_VAL_LIST) {
        bool is_primitive = false;
        uint8 element_prim_type = 0;

        if (allow_list_u8_param || allow_list_u8_result) {
            if (!def_type->def_val.list || !def_type->def_val.list->element_type)
                return set_component_call_error_fmt(
                    inst,
                    "component canon lift function %s %u uses malformed list type",
                    position, index);

            if (!lookup_component_call_primitive_type(
                    component, def_type->def_val.list->element_type, position,
                    index, &is_primitive, &element_prim_type, inst))
                return false;

            if (is_primitive && element_prim_type == WASM_COMP_PRIMVAL_U8) {
                memset(out_info, 0, sizeof(*out_info));
                out_info->kind = WASM_COMP_CANON_LIFT_VALUE_LIST_U8;
                out_info->declared_as_defined = true;
                return true;
            }

            return set_component_call_error_fmt(inst,
                                                allow_list_u8_result
                                                    ? "component canon lift "
                                                      "function %s %u only "
                                                      "supports list<u8> results"
                                                    : "component canon lift "
                                                      "function %s %u only "
                                                      "supports list<u8> "
                                                      "parameters",
                                                position, index);
        }

        return set_component_call_error_fmt(
            inst, "component canon lift function %s %u requires memory-backed "
                  "Canonical ABI for %s",
            position, index, component_def_type_name(def_type->tag));
    }

    if (def_type->tag == WASM_COMP_DEF_VAL_LIST_LEN) {
        if (allow_list_u8_param || allow_list_u8_result)
            return set_component_call_error_fmt(
                inst,
                allow_list_u8_result
                    ? "component canon lift function %s %u only supports "
                      "variable-length list<u8> results"
                    : "component canon lift function %s %u only supports "
                      "variable-length list<u8> parameters",
                position, index);
        return set_component_call_error_fmt(
            inst, "component canon lift function %s %u requires memory-backed "
                  "Canonical ABI for %s",
            position, index, component_def_type_name(def_type->tag));
    }

    return set_component_call_error_fmt(
        inst,
        "component canon lift function %s %u uses unsupported non-scalar %s type",
        position, index, component_def_type_name(def_type->tag));
}

static bool
resolve_component_func_type(WASMComponentInstance *inst,
                            const WASMComponentRuntimeFunc *function,
                            const char *function_name,
                            WASMComponentFuncType **out_component_type)
{
    const WASMComponentTypes *type_entry =
        wasm_component_lookup_type(&inst->module->component, function->type_idx);

    if (!type_entry)
        return set_component_call_error_fmt(
            inst, "%s uses unresolved type index %u", function_name,
            function->type_idx);

    if (type_entry->tag != WASM_COMP_FUNC_TYPE || !type_entry->type.func_type)
        return set_component_call_error_fmt(
            inst, "%s type index %u is not a function", function_name,
            function->type_idx);

    *out_component_type = type_entry->type.func_type;
    return true;
}

static uint32
get_component_func_result_count(const WASMComponentFuncType *component_type)
{
    return component_type && component_type->results
                   && component_type->results->tag
                          == WASM_COMP_RESULT_LIST_WITH_TYPE
                   && component_type->results->results
               ? 1
               : 0;
}

static bool
set_component_call_error_from_host_result(WASMComponentInstance *inst,
                                          const char *error_buf)
{
    return error_buf && error_buf[0]
               ? set_component_call_error(inst, error_buf)
               : set_component_call_error(inst,
                                          "host component function call failed");
}

static bool
validate_component_host_import_value_type(
    const WASMComponent *component, const WASMComponentValueType *value_type,
    const char *import_name, const char *position, uint32 index,
    bool *is_string_out, bool *is_list_u8_out, bool *is_composite_out,
    char *error_buf, uint32 error_buf_size)
{
    const WASMComponentTypes *type_entry;
    WASMComponentDefValType *def_type;
    uint8 prim_type;
    uint8 ignored_core_type = 0;
    wasm_valkind_t ignored_public_kind = WASM_I32;
    const WASMComponentValueType *element_type;
    const bool allow_memory_backed_composite_leaves =
        !strcmp(position, "parameter") || !strcmp(position, "result");

    *is_string_out = false;
    *is_list_u8_out = false;
    *is_composite_out = false;

    if (!value_type)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "host component import \"%s\" %s %u is missing a type", import_name,
            position, index);

    if (value_type->type == WASM_COMP_VAL_TYPE_PRIMVAL) {
        prim_type = value_type->type_specific.primval_type;
        if (component_scalar_prim_to_core(prim_type, &ignored_core_type,
                                          &ignored_public_kind))
            return true;
        if (prim_type == WASM_COMP_PRIMVAL_STRING) {
            *is_string_out = true;
            return true;
        }
        if (prim_type == WASM_COMP_PRIMVAL_ERROR_CONTEXT)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "host component import \"%s\" %s %u uses unsupported component "
                "scalar type error-context",
                import_name, position, index);
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "host component import \"%s\" %s %u uses unsupported component "
            "scalar type %s",
            import_name, position, index, component_prim_type_name(prim_type));
    }

    type_entry =
        wasm_component_lookup_type(component, value_type->type_specific.type_idx);
    if (!type_entry)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "host component import \"%s\" %s %u uses unresolved type index %u",
            import_name, position, index, value_type->type_specific.type_idx);

    if (type_entry->tag != WASM_COMP_DEF_TYPE || !type_entry->type.def_val_type)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "host component import \"%s\" %s %u uses non-value type index %u",
            import_name, position, index, value_type->type_specific.type_idx);

    def_type = type_entry->type.def_val_type;
    if (def_type->tag == WASM_COMP_DEF_VAL_PRIMVAL) {
        prim_type = def_type->def_val.primval;
        if (component_scalar_prim_to_core(prim_type, &ignored_core_type,
                                          &ignored_public_kind))
            return true;
        if (prim_type == WASM_COMP_PRIMVAL_STRING) {
            *is_string_out = true;
            return true;
        }
        if (prim_type == WASM_COMP_PRIMVAL_ERROR_CONTEXT)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "host component import \"%s\" %s %u uses unsupported component "
                "scalar type error-context",
                import_name, position, index);
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "host component import \"%s\" %s %u uses unsupported component "
            "scalar type %s",
            import_name, position, index, component_prim_type_name(prim_type));
    }

    if (def_type->tag == WASM_COMP_DEF_VAL_LIST) {
        if (!def_type->def_val.list || !def_type->def_val.list->element_type)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "host component import \"%s\" %s %u uses malformed list type",
                import_name, position, index);

        element_type = def_type->def_val.list->element_type;
        if (element_type->type == WASM_COMP_VAL_TYPE_PRIMVAL)
            prim_type = element_type->type_specific.primval_type;
        else {
            type_entry = wasm_component_lookup_type(
                component, element_type->type_specific.type_idx);
            if (!type_entry)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "host component import \"%s\" %s %u uses unresolved type "
                    "index %u",
                    import_name, position, index,
                    element_type->type_specific.type_idx);
            if (type_entry->tag != WASM_COMP_DEF_TYPE
                || !type_entry->type.def_val_type)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "host component import \"%s\" %s %u uses non-value type "
                    "index %u",
                    import_name, position, index,
                    element_type->type_specific.type_idx);
            if (type_entry->type.def_val_type->tag != WASM_COMP_DEF_VAL_PRIMVAL)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    !strcmp(position, "result")
                        ? "host component import \"%s\" %s %u only supports "
                          "list<u8> results"
                        : "host component import \"%s\" %s %u only supports "
                          "list<u8> parameters",
                    import_name, position, index);
            prim_type = type_entry->type.def_val_type->def_val.primval;
        }

        if (prim_type == WASM_COMP_PRIMVAL_U8) {
            *is_list_u8_out = true;
            return true;
        }

        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            !strcmp(position, "result")
                ? "host component import \"%s\" %s %u only supports list<u8> "
                  "results"
                : "host component import \"%s\" %s %u only supports list<u8> "
                  "parameters",
            import_name, position, index);
    }

    if (def_type->tag == WASM_COMP_DEF_VAL_LIST_LEN)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            !strcmp(position, "result")
                ? "host component import \"%s\" %s %u only supports "
                  "variable-length list<u8> results"
                : "host component import \"%s\" %s %u only supports "
                  "variable-length list<u8> parameters",
            import_name, position, index);

    if (def_type->tag == WASM_COMP_DEF_VAL_RECORD
        || def_type->tag == WASM_COMP_DEF_VAL_TUPLE) {
        switch (def_type->tag) {
            case WASM_COMP_DEF_VAL_RECORD:
                if (!def_type->def_val.record)
                    return set_component_runtime_error_fmt(
                        error_buf, error_buf_size,
                        !strcmp(position, "result")
                            ? "host component import \"%s\" result %u only "
                              "supports tuple/record results with scalar, "
                              "UTF-8 string, or variable-length list<u8> leaves"
                            : "host component import \"%s\" parameter %u only "
                              "supports tuple/record parameters with scalar, "
                              "UTF-8 string, or variable-length list<u8> "
                              "leaves",
                        import_name, index);
                for (uint32 i = 0; i < def_type->def_val.record->count; i++) {
                    bool nested_is_string = false;
                    bool nested_is_list_u8 = false;
                    bool nested_is_composite = false;

                    if (!validate_component_host_import_value_type(
                            component, def_type->def_val.record->fields[i].value_type,
                            import_name, position, index, &nested_is_string,
                            &nested_is_list_u8, &nested_is_composite, error_buf,
                            error_buf_size))
                        return false;
                    if (!allow_memory_backed_composite_leaves
                        && (nested_is_string || nested_is_list_u8))
                        return set_component_runtime_error_fmt(
                            error_buf, error_buf_size,
                            !strcmp(position, "result")
                                ? "host component import \"%s\" result %u only "
                                  "supports tuple/record results with scalar, "
                                  "UTF-8 string, or variable-length list<u8> "
                                  "leaves"
                                : "host component import \"%s\" parameter %u "
                                  "only supports tuple/record parameters with "
                                  "scalar leaves",
                            import_name, index);
                    *is_string_out = *is_string_out || nested_is_string;
                    *is_list_u8_out = *is_list_u8_out || nested_is_list_u8;
                }
                *is_composite_out = true;
                return true;
            case WASM_COMP_DEF_VAL_TUPLE:
                if (!def_type->def_val.tuple)
                    return set_component_runtime_error_fmt(
                        error_buf, error_buf_size,
                        !strcmp(position, "result")
                            ? "host component import \"%s\" result %u only "
                              "supports tuple/record results with scalar, "
                              "UTF-8 string, or variable-length list<u8> leaves"
                            : "host component import \"%s\" parameter %u only "
                              "supports tuple/record parameters with scalar, "
                              "UTF-8 string, or variable-length list<u8> "
                              "leaves",
                        import_name, index);
                for (uint32 i = 0; i < def_type->def_val.tuple->count; i++) {
                    bool nested_is_string = false;
                    bool nested_is_list_u8 = false;
                    bool nested_is_composite = false;

                    if (!validate_component_host_import_value_type(
                            component, &def_type->def_val.tuple->element_types[i],
                            import_name, position, index, &nested_is_string,
                            &nested_is_list_u8, &nested_is_composite, error_buf,
                            error_buf_size))
                        return false;
                    if (!allow_memory_backed_composite_leaves
                        && (nested_is_string || nested_is_list_u8))
                        return set_component_runtime_error_fmt(
                            error_buf, error_buf_size,
                            !strcmp(position, "result")
                                ? "host component import \"%s\" result %u only "
                                  "supports tuple/record results with scalar, "
                                  "UTF-8 string, or variable-length list<u8> "
                                  "leaves"
                                : "host component import \"%s\" parameter %u "
                                  "only supports tuple/record parameters with "
                                  "scalar leaves",
                            import_name, index);
                    *is_string_out = *is_string_out || nested_is_string;
                    *is_list_u8_out = *is_list_u8_out || nested_is_list_u8;
                }
                *is_composite_out = true;
                return true;
            default:
                break;
        }
    }

    return set_component_runtime_error_fmt(
        error_buf, error_buf_size,
        "host component import \"%s\" %s %u uses unsupported non-scalar %s type",
        import_name, position, index, component_def_type_name(def_type->tag));
}

static bool
validate_component_host_import_func_type(WASMComponentInstance *inst,
                                         WASMComponentRuntimeFunc *function,
                                         const char *import_name,
                                         char *error_buf,
                                         uint32 error_buf_size)
{
    const WASMComponentTypes *type_entry;
    WASMComponentFuncType *func_type;
    uint32 i;

    function->has_string_params = false;
    function->has_list_u8_params = false;
    function->has_composite_params = false;
    function->has_string_result = false;
    function->has_list_u8_result = false;
    function->has_composite_result = false;

    type_entry =
        wasm_component_lookup_type(&inst->module->component, function->type_idx);
    if (!type_entry)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "host component import \"%s\" uses unresolved type index %u",
            import_name, function->type_idx);

    if (type_entry->tag != WASM_COMP_FUNC_TYPE || !type_entry->type.func_type)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "host component import \"%s\" type index %u is not a function",
            import_name, function->type_idx);

    func_type = type_entry->type.func_type;
    if (func_type->params) {
        for (i = 0; i < func_type->params->count; i++) {
            bool is_string;
            bool is_list_u8;
            bool is_composite;

            if (!validate_component_host_import_value_type(
                    &inst->module->component,
                    func_type->params->params[i].value_type, import_name,
                    "parameter", i, &is_string, &is_list_u8, &is_composite,
                    error_buf,
                    error_buf_size))
                return false;
            if (is_string)
                function->has_string_params = true;
            if (is_list_u8)
                function->has_list_u8_params = true;
            if (is_composite)
                function->has_composite_params = true;
        }
    }

    if (func_type->results
        && func_type->results->tag == WASM_COMP_RESULT_LIST_WITH_TYPE) {
        bool is_composite = false;

        if (!func_type->results->results)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "host component import \"%s\" result metadata is missing",
                import_name);
        if (!validate_component_host_import_value_type(
                &inst->module->component, func_type->results->results,
                import_name, "result", 0, &function->has_string_result,
                &function->has_list_u8_result, &is_composite, error_buf,
                error_buf_size))
            return false;
        if (is_composite)
            function->has_composite_result = true;
    }

    return true;
}

static bool
lookup_component_scalar_type(const WASMComponent *component,
                             const WASMComponentValueType *value_type,
                             const char *position, uint32 index,
                             uint8 *prim_type_out, uint8 *core_type_out,
                             wasm_valkind_t *public_kind_out,
                             WASMComponentInstance *inst)
{
    WASMComponentCanonLiftValueInfo info;

    if (!lookup_component_canon_lift_value_type(component, value_type, position,
                                                index, false, false, false,
                                                &info, inst))
        return false;

    *prim_type_out = info.prim_type;
    *core_type_out = info.core_type;
    *public_kind_out = info.public_kind;
    return true;
}

static bool
is_valid_unicode_scalar(uint32 value)
{
    return value <= 0x10FFFF && !(value >= 0xD800 && value <= 0xDFFF);
}

static bool
validate_component_scalar_value(WASMComponentInstance *inst,
                                const wasm_val_t *value,
                                wasm_valkind_t expected_kind, uint8 prim_type,
                                const char *position, uint32 index)
{
    if (value->kind != expected_kind)
        return set_component_call_error_fmt(
            inst, "component canon lift function %s %u expects kind %u but got %u",
            position, index, (unsigned)expected_kind, (unsigned)value->kind);

    switch (prim_type) {
        case WASM_COMP_PRIMVAL_BOOL:
            if (value->of.i32 != 0 && value->of.i32 != 1)
                return set_component_call_error_fmt(
                    inst, "component canon lift function %s %u expects bool 0 or 1",
                    position, index);
            break;
        case WASM_COMP_PRIMVAL_S8:
            if (value->of.i32 < INT8_MIN || value->of.i32 > INT8_MAX)
                return set_component_call_error_fmt(
                    inst, "component canon lift function %s %u is out of s8 range",
                    position, index);
            break;
        case WASM_COMP_PRIMVAL_U8:
            if ((uint32)value->of.i32 > UINT8_MAX)
                return set_component_call_error_fmt(
                    inst, "component canon lift function %s %u is out of u8 range",
                    position, index);
            break;
        case WASM_COMP_PRIMVAL_S16:
            if (value->of.i32 < INT16_MIN || value->of.i32 > INT16_MAX)
                return set_component_call_error_fmt(
                    inst, "component canon lift function %s %u is out of s16 range",
                    position, index);
            break;
        case WASM_COMP_PRIMVAL_U16:
            if ((uint32)value->of.i32 > UINT16_MAX)
                return set_component_call_error_fmt(
                    inst, "component canon lift function %s %u is out of u16 range",
                    position, index);
            break;
        case WASM_COMP_PRIMVAL_CHAR:
            if (!is_valid_unicode_scalar((uint32)value->of.i32))
                return set_component_call_error_fmt(
                    inst,
                    "component canon lift function %s %u is not a valid Unicode "
                    "scalar value",
                    position, index);
            break;
        default:
            break;
    }

    return true;
}

static bool
resolve_component_lift_string_usage(const WASMComponent *component,
                                    const WASMComponentValueType *value_type,
                                    bool *is_string, char *error_buf,
                                    uint32 error_buf_size)
{
    const WASMComponentTypes *type_entry;
    WASMComponentDefValType *def_type;

    *is_string = false;
    if (!value_type)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component canon lift function is missing a value type");

    if (value_type->type == WASM_COMP_VAL_TYPE_PRIMVAL) {
        *is_string =
            value_type->type_specific.primval_type == WASM_COMP_PRIMVAL_STRING;
        return true;
    }

    type_entry =
        wasm_component_lookup_type(component, value_type->type_specific.type_idx);
    if (!type_entry)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component canon lift function uses unresolved type index %u",
            value_type->type_specific.type_idx);

    if (type_entry->tag != WASM_COMP_DEF_TYPE)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component canon lift function uses non-value type index %u",
            value_type->type_specific.type_idx);

    def_type = type_entry->type.def_val_type;
    if (def_type->tag == WASM_COMP_DEF_VAL_PRIMVAL)
        *is_string = def_type->def_val.primval == WASM_COMP_PRIMVAL_STRING;

    return true;
}

static bool
validate_canon_helper_signature(const WASMComponentCoreRuntimeRef *ref,
                                uint32 expected_param_count,
                                const uint8 *expected_types,
                                uint32 expected_result_count,
                                char *error_buf, uint32 error_buf_size,
                                const char *helper_name)
{
    WASMModuleInstanceCommon *module_inst;
    WASMFuncType *func_type;
    uint32 i;

    if (!ref || ref->type != WASM_COMP_CORE_RUNTIME_REF_FUNC || !ref->of.function
        || !ref->owner_instance || !ref->owner_instance->module_inst)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component canon lift function %s helper is not bound to an "
            "instantiated core function",
            helper_name);

    module_inst = (WASMModuleInstanceCommon *)ref->owner_instance->module_inst;
    func_type =
        wasm_runtime_get_function_type(ref->of.function, module_inst->module_type);
    if (!func_type)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component canon lift function %s helper type could not be resolved",
            helper_name);

    if (func_type->param_count != expected_param_count
        || func_type->result_count != expected_result_count)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component canon lift function %s helper signature is unsupported",
            helper_name);

    for (i = 0; i < expected_param_count; i++) {
        if (func_type->types[i] != expected_types[i])
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "component canon lift function %s helper signature is unsupported",
                helper_name);
    }

    for (i = 0; i < expected_result_count; i++) {
        if (func_type->types[expected_param_count + i]
            != expected_types[expected_param_count + i])
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "component canon lift function %s helper signature is unsupported",
                helper_name);
    }

    return true;
}

static bool
component_canon_lift_uses_lowered_core_func(
    const WASMComponentRuntimeFunc *function)
{
    return function && function->kind == WASM_COMP_RUNTIME_FUNC_LIFT
           && function->core_func_ref.type
                  == WASM_COMP_CORE_RUNTIME_REF_LOWERED_FUNC
           && function->core_func_ref.of.lowered_function;
}

static bool
resolve_component_canon_lift_abi(WASMComponentInstance *inst,
                                 WASMComponentRuntimeFunc *function,
                                 char *error_buf, uint32 error_buf_size)
{
    const WASMComponentTypes *type_entry;
    const WASMComponentFuncType *func_type;
    uint32 i;
    bool needs_memory_abi;

    function->string_encoding = WASM_COMP_RUNTIME_STRING_ENCODING_NONE;
    memset(&function->canon_memory_ref, 0, sizeof(function->canon_memory_ref));
    memset(&function->canon_realloc_ref, 0, sizeof(function->canon_realloc_ref));
    memset(&function->canon_post_return_ref, 0,
           sizeof(function->canon_post_return_ref));
    function->memory_result_kind = WASM_COMP_RUNTIME_CANON_LIFT_MEMORY_RESULT_NONE;
    function->has_string_params = false;
    function->has_list_u8_params = false;

    type_entry =
        wasm_component_lookup_type(&inst->module->component, function->type_idx);
    if (!type_entry)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component canon lift function uses unresolved type index %u",
            function->type_idx);

    if (type_entry->tag != WASM_COMP_FUNC_TYPE || !type_entry->type.func_type)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component canon lift function type index %u is not a function",
            function->type_idx);

    func_type = type_entry->type.func_type;
    if (func_type->params) {
        for (i = 0; i < func_type->params->count; i++) {
            bool is_string = false;
            bool is_list_u8 = false;
            bool is_primitive_param = false;
            bool is_composite_param = false;
            uint8 param_prim_type = 0;

            if (!resolve_component_runtime_primitive_type(
                    &inst->module->component, func_type->params->params[i].value_type,
                    &is_primitive_param, &param_prim_type, error_buf,
                    error_buf_size))
                return false;
            (void)param_prim_type;

            if (!is_primitive_param
                && func_type->params->params[i].value_type
                && func_type->params->params[i].value_type->type
                       == WASM_COMP_VAL_TYPE_IDX) {
                const WASMComponentTypes *param_type_entry =
                    wasm_component_lookup_type(
                        &inst->module->component,
                        func_type->params->params[i].value_type->type_specific
                            .type_idx);

                if (!param_type_entry)
                    return set_component_runtime_error_fmt(
                        error_buf, error_buf_size,
                        "component canon lift function uses unresolved type "
                        "index %u",
                        func_type->params->params[i].value_type->type_specific
                            .type_idx);

                if (param_type_entry->tag != WASM_COMP_DEF_TYPE
                    || !param_type_entry->type.def_val_type)
                    return set_component_runtime_error_fmt(
                        error_buf, error_buf_size,
                        "component canon lift function uses non-value type index "
                        "%u",
                        func_type->params->params[i].value_type->type_specific
                            .type_idx);

                is_composite_param =
                    param_type_entry->type.def_val_type->tag
                        == WASM_COMP_DEF_VAL_RECORD
                    || param_type_entry->type.def_val_type->tag
                           == WASM_COMP_DEF_VAL_TUPLE;
            }
            if (!resolve_component_lift_string_usage(
                    &inst->module->component, func_type->params->params[i].value_type,
                    &is_string, error_buf, error_buf_size))
                return false;
            if (is_composite_param) {
                bool composite_has_string = false;
                bool composite_has_list_u8 = false;

                if (!classify_component_runtime_composite_param(
                        &inst->module->component,
                        func_type->params->params[i].value_type, i,
                        &composite_has_string, &composite_has_list_u8, error_buf,
                        error_buf_size))
                    return false;
                if (composite_has_string)
                    function->has_string_params = true;
                if (composite_has_list_u8)
                    function->has_list_u8_params = true;
                continue;
            }

            if (is_string)
                function->has_string_params = true;
            if (!is_string
                && !resolve_component_lift_list_u8_usage(
                    &inst->module->component,
                    func_type->params->params[i].value_type, &is_list_u8,
                    error_buf, error_buf_size))
                return false;
            if (is_list_u8)
                function->has_list_u8_params = true;
        }
    }

    if (func_type->results
        && func_type->results->tag == WASM_COMP_RESULT_LIST_WITH_TYPE
        && func_type->results->results) {
        bool has_string_result = false;
        bool has_list_u8_result = false;
        bool is_primitive_result = false;
        uint8 result_prim_type = 0;

        if (!resolve_component_runtime_primitive_type(
                &inst->module->component, func_type->results->results,
                &is_primitive_result, &result_prim_type, error_buf,
                error_buf_size))
            return false;

        if (!is_primitive_result && func_type->results->results->type == WASM_COMP_VAL_TYPE_IDX) {
            const WASMComponentTypes *result_type_entry = wasm_component_lookup_type(
                &inst->module->component,
                func_type->results->results->type_specific.type_idx);

            if (!result_type_entry)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component canon lift function uses unresolved type index %u",
                    func_type->results->results->type_specific.type_idx);

            if (result_type_entry->tag != WASM_COMP_DEF_TYPE
                || !result_type_entry->type.def_val_type)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component canon lift function uses non-value type index %u",
                    func_type->results->results->type_specific.type_idx);

            if (result_type_entry->type.def_val_type->tag == WASM_COMP_DEF_VAL_RECORD
                || result_type_entry->type.def_val_type->tag
                       == WASM_COMP_DEF_VAL_TUPLE) {
                bool composite_has_string = false;
                bool composite_has_list_u8 = false;

                if (!classify_component_runtime_composite_result(
                        &inst->module->component, func_type->results->results,
                        &composite_has_string, &composite_has_list_u8,
                        error_buf, error_buf_size))
                    return false;

                function->has_composite_result = true;
                if (composite_has_string)
                    function->has_string_result = true;
                if (composite_has_list_u8)
                    function->has_list_u8_result = true;
                if (composite_has_string || composite_has_list_u8)
                    function->memory_result_kind =
                        WASM_COMP_RUNTIME_CANON_LIFT_MEMORY_RESULT_COMPOSITE;
            }
            else if (!resolve_component_lift_string_usage(
                         &inst->module->component, func_type->results->results,
                         &has_string_result, error_buf, error_buf_size)
                     || (!has_string_result
                         && !resolve_component_lift_list_u8_usage(
                             &inst->module->component, func_type->results->results,
                             &has_list_u8_result, error_buf, error_buf_size)))
                return false;
        }
        else {
            has_string_result = is_primitive_result
                                && result_prim_type == WASM_COMP_PRIMVAL_STRING;
            if (!has_string_result
                && !resolve_component_lift_list_u8_usage(
                    &inst->module->component, func_type->results->results,
                    &has_list_u8_result, error_buf, error_buf_size))
                return false;
        }

        if (has_string_result)
            function->memory_result_kind =
                WASM_COMP_RUNTIME_CANON_LIFT_MEMORY_RESULT_STRING;
        else if (has_list_u8_result)
            function->memory_result_kind =
                WASM_COMP_RUNTIME_CANON_LIFT_MEMORY_RESULT_LIST_U8;

        if (has_string_result)
            function->has_string_result = true;
        if (has_list_u8_result)
            function->has_list_u8_result = true;
    }

    if (!function->canon_opts)
        goto validate_required_opts;

    for (i = 0; i < function->canon_opts->canon_opts_count; i++) {
        const WASMComponentCanonOpt *opt = &function->canon_opts->canon_opts[i];

        switch (opt->tag) {
            case WASM_COMP_CANON_OPT_STRING_UTF8:
                function->string_encoding =
                    WASM_COMP_RUNTIME_STRING_ENCODING_UTF8;
                break;
            case WASM_COMP_CANON_OPT_STRING_UTF16:
                function->string_encoding =
                    WASM_COMP_RUNTIME_STRING_ENCODING_UTF16;
                break;
            case WASM_COMP_CANON_OPT_STRING_LATIN1_UTF16:
                function->string_encoding =
                    WASM_COMP_RUNTIME_STRING_ENCODING_LATIN1_UTF16;
                break;
            case WASM_COMP_CANON_OPT_MEMORY:
                if (opt->payload.memory.mem_idx >= inst->core_memory_count)
                    return set_component_runtime_error_fmt(
                        error_buf, error_buf_size,
                        "canon lift memory index %u is out of bounds",
                        opt->payload.memory.mem_idx);
                function->canon_memory_ref =
                    inst->core_memories[opt->payload.memory.mem_idx];
                if (function->canon_memory_ref.type
                        != WASM_COMP_CORE_RUNTIME_REF_MEMORY
                    || !function->canon_memory_ref.of.memory)
                    return set_component_runtime_error_fmt(
                        error_buf, error_buf_size,
                        "canon lift memory index %u does not resolve to memory",
                        opt->payload.memory.mem_idx);
                break;
            case WASM_COMP_CANON_OPT_REALLOC:
                if (opt->payload.realloc_opt.func_idx >= inst->core_func_count)
                    return set_component_runtime_error_fmt(
                        error_buf, error_buf_size,
                        "canon lift realloc func index %u is out of bounds",
                        opt->payload.realloc_opt.func_idx);
                function->canon_realloc_ref =
                    inst->core_funcs[opt->payload.realloc_opt.func_idx];
                if (function->canon_realloc_ref.type
                        != WASM_COMP_CORE_RUNTIME_REF_FUNC
                    || !function->canon_realloc_ref.of.function)
                    return set_component_runtime_error_fmt(
                        error_buf, error_buf_size,
                        "canon lift realloc func index %u does not resolve to a "
                        "function",
                        opt->payload.realloc_opt.func_idx);
                break;
            case WASM_COMP_CANON_OPT_POST_RETURN:
                if (opt->payload.post_return.func_idx >= inst->core_func_count)
                    return set_component_runtime_error_fmt(
                        error_buf, error_buf_size,
                        "canon lift post-return func index %u is out of bounds",
                        opt->payload.post_return.func_idx);
                function->canon_post_return_ref =
                    inst->core_funcs[opt->payload.post_return.func_idx];
                if (function->canon_post_return_ref.type
                        != WASM_COMP_CORE_RUNTIME_REF_FUNC
                    || !function->canon_post_return_ref.of.function)
                    return set_component_runtime_error_fmt(
                        error_buf, error_buf_size,
                        "canon lift post-return func index %u does not resolve to "
                        "a function",
                        opt->payload.post_return.func_idx);
                break;
            default:
                break;
        }
    }

validate_required_opts:
    if (component_canon_lift_uses_lowered_core_func(function)) {
        if (function->has_string_params || function->has_list_u8_params
            || function->has_composite_params || function->has_string_result
            || function->has_list_u8_result || function->has_composite_result
            || function->memory_result_kind
                   != WASM_COMP_RUNTIME_CANON_LIFT_MEMORY_RESULT_NONE)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "component canon lift over lowered core functions currently "
                "supports only scalar signatures");

        return true;
    }

    needs_memory_abi = function->has_string_params
                       || function->has_list_u8_params
                       || function->memory_result_kind
                              != WASM_COMP_RUNTIME_CANON_LIFT_MEMORY_RESULT_NONE;
    if (!needs_memory_abi)
        return true;

    if ((function->has_string_params || function->has_string_result)
        && function->string_encoding != WASM_COMP_RUNTIME_STRING_ENCODING_UTF8)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component canon lift function only supports UTF-8 strings");

    if (function->canon_memory_ref.type != WASM_COMP_CORE_RUNTIME_REF_MEMORY
        || !function->canon_memory_ref.of.memory)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            function->has_string_params || function->has_string_result
                    ? "component canon lift function requires memory for string "
                      "Canonical ABI"
                    : function->has_list_u8_params
                              || function->has_list_u8_result
                          ? "component canon lift function requires memory for "
                            "list<u8> Canonical ABI"
                          : "component canon lift function requires memory for "
                            "memory-backed Canonical ABI");

    if (function->canon_memory_ref.of.memory->is_memory64)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            function->has_string_params || function->has_string_result
                    ? "component canon lift function does not support memory64 "
                      "string Canonical ABI"
                    : function->has_list_u8_params
                              || function->has_list_u8_result
                          ? "component canon lift function does not support "
                            "memory64 list<u8> Canonical ABI"
                          : "component canon lift function does not support "
                            "memory64 Canonical ABI");

    if (!function->core_func_ref.owner_instance
        || !function->core_func_ref.owner_instance->module_inst
        || !function->canon_memory_ref.owner_instance
        || function->canon_memory_ref.owner_instance->module_inst
               != function->core_func_ref.owner_instance->module_inst)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component canon lift function requires canon memory in the same "
            "core instance");

    if (function->has_string_params || function->has_list_u8_params) {
        static const uint8 realloc_sig[] = { VALUE_TYPE_I32, VALUE_TYPE_I32,
                                             VALUE_TYPE_I32, VALUE_TYPE_I32,
                                             VALUE_TYPE_I32 };

        if (function->canon_realloc_ref.type != WASM_COMP_CORE_RUNTIME_REF_FUNC
            || !function->canon_realloc_ref.of.function)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                function->has_string_params
                    ? "component canon lift function requires realloc for "
                      "string parameters"
                    : "component canon lift function requires realloc for "
                      "list<u8> parameters");

        if (!function->canon_realloc_ref.owner_instance
            || function->canon_realloc_ref.owner_instance->module_inst
                   != function->core_func_ref.owner_instance->module_inst)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "component canon lift function requires realloc in the same "
                "core instance");

        if (!validate_canon_helper_signature(&function->canon_realloc_ref, 4,
                                             realloc_sig, 1, error_buf,
                                             error_buf_size, "realloc"))
            return false;
    }

    if (function->canon_post_return_ref.of.function) {
        static const uint8 post_return_sig[] = { VALUE_TYPE_I32 };

        if (!function->canon_post_return_ref.owner_instance
            || function->canon_post_return_ref.owner_instance->module_inst
                   != function->core_func_ref.owner_instance->module_inst)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "component canon lift function requires post-return in the same "
                "core instance");

        if (!validate_canon_helper_signature(&function->canon_post_return_ref, 1,
                                             post_return_sig, 0, error_buf,
                                             error_buf_size, "post-return"))
            return false;
    }

    return true;
}

static bool
resolve_component_canon_lift_type(WASMComponentInstance *inst,
                                  const WASMComponentRuntimeFunc *function,
                                  WASMComponentFuncType **out_component_type,
                                  WASMFuncType **out_core_type)
{
    WASMModuleInstanceCommon *core_module_inst;
    if (!resolve_component_func_type(inst, function, "component canon lift function",
                                     out_component_type))
        return false;

    if (component_canon_lift_uses_lowered_core_func(function)) {
        *out_core_type = NULL;
        return true;
    }

    if (!function->core_func_ref.owner_instance
        || !function->core_func_ref.owner_instance->module_inst)
        return set_component_call_error(
            inst, "component canon lift function is not bound to an instantiated "
                  "core function");

    core_module_inst =
        (WASMModuleInstanceCommon *)function->core_func_ref.owner_instance->module_inst;
    *out_core_type = wasm_runtime_get_function_type(function->core_func_ref.of.function,
                                                    core_module_inst->module_type);
    if (!*out_core_type)
        return set_component_call_error(
            inst, "component canon lift function could not resolve the core "
                  "function type");

    return true;
}

static bool
validate_component_public_value_type(
    WASMComponentInstance *inst, const wasm_component_value_t *value,
    const WASMComponentCanonLiftValueInfo *type_info, const char *position,
    uint32 index)
{
    if (!value)
        return set_component_call_error_fmt(
            inst, "component canon lift function %s %u value is null", position,
            index);

    if (type_info->declared_as_defined) {
        if (value->type.kind != WASM_COMPONENT_VALUE_TYPE_DEFINED)
            return set_component_call_error_fmt(
                inst, "component canon lift function %s %u expects a defined "
                      "component value",
                position, index);
    }
    else if (value->type.kind != WASM_COMPONENT_VALUE_TYPE_PRIMITIVE
             || value->type.type.primitive_type
                    != (wasm_component_primitive_value_kind_t)type_info->prim_type)
        return set_component_call_error_fmt(
            inst, "component canon lift function %s %u expects component value "
                  "type %s",
            position, index, component_prim_type_name(type_info->prim_type));

    return true;
}

static bool
decode_component_public_leb_value(WASMComponentInstance *inst, const uint8 *data,
                                  uint32 byte_size, uint32 maxbits, bool sign,
                                  const char *type_name, const char *position,
                                  uint32 index, uint64 *out_value)
{
    size_t offset = 0;
    bh_leb_read_status_t status;

    if (!data || byte_size == 0)
        return set_component_call_error_fmt(
            inst, "component canon lift function %s %u is missing %s bytes",
            position, index, type_name);

    status =
        bh_leb_read(data, data + byte_size, maxbits, sign, out_value, &offset);
    if (status != BH_LEB_READ_SUCCESS || offset != byte_size)
        return set_component_call_error_fmt(
            inst, "component canon lift function %s %u does not contain a valid "
                  "%s value",
            position, index, type_name);

    return true;
}

static bool
decode_component_public_char(WASMComponentInstance *inst, const uint8 *data,
                             uint32 byte_size, const char *position,
                             uint32 index, uint32 *code_point_out)
{
    uint32 code_point;

    if (!data || !wasm_component_validate_single_utf8_scalar(data, byte_size))
        return set_component_call_error_fmt(
            inst, "component canon lift function %s %u does not contain a valid "
                  "char value",
            position, index);

    switch (byte_size) {
        case 1:
            code_point = data[0];
            break;
        case 2:
            code_point =
                ((uint32)(data[0] & 0x1F) << 6) | (uint32)(data[1] & 0x3F);
            break;
        case 3:
            code_point = ((uint32)(data[0] & 0x0F) << 12)
                         | ((uint32)(data[1] & 0x3F) << 6)
                         | (uint32)(data[2] & 0x3F);
            break;
        case 4:
            code_point = ((uint32)(data[0] & 0x07) << 18)
                         | ((uint32)(data[1] & 0x3F) << 12)
                         | ((uint32)(data[2] & 0x3F) << 6)
                         | (uint32)(data[3] & 0x3F);
            break;
        default:
            return set_component_call_error_fmt(
                inst, "component canon lift function %s %u does not contain a "
                      "valid char value",
                position, index);
    }

    *code_point_out = code_point;
    return true;
}

static bool
decode_component_public_scalar_value(
    WASMComponentInstance *inst, const wasm_component_value_t *value,
    const WASMComponentCanonLiftValueInfo *type_info, const char *position,
    uint32 index, wasm_val_t *out_value)
{
    const uint8 *data = wasm_component_value_get_data(value);
    uint64 leb_value = 0;
    uint32 code_point = 0;

    if (!validate_component_public_value_type(inst, value, type_info, position,
                                              index))
        return false;

    if (value->byte_size > 0 && !data)
        return set_component_call_error_fmt(
            inst, "component canon lift function %s %u is missing backing bytes",
            position, index);

    memset(out_value, 0, sizeof(*out_value));
    out_value->kind = type_info->public_kind;

    switch (type_info->prim_type) {
        case WASM_COMP_PRIMVAL_BOOL:
            if (value->byte_size != 1 || (data[0] != 0 && data[0] != 1))
                return set_component_call_error_fmt(
                    inst, "component canon lift function %s %u does not contain "
                          "a valid bool value",
                    position, index);
            out_value->of.i32 = data[0];
            return true;
        case WASM_COMP_PRIMVAL_S8:
            if (value->byte_size != 1)
                return set_component_call_error_fmt(
                    inst, "component canon lift function %s %u does not contain "
                          "a valid s8 value",
                    position, index);
            out_value->of.i32 = (int8)data[0];
            return true;
        case WASM_COMP_PRIMVAL_U8:
            if (value->byte_size != 1)
                return set_component_call_error_fmt(
                    inst, "component canon lift function %s %u does not contain "
                          "a valid u8 value",
                    position, index);
            out_value->of.i32 = data[0];
            return true;
        case WASM_COMP_PRIMVAL_S16:
        case WASM_COMP_PRIMVAL_S32:
            if (!decode_component_public_leb_value(inst, data, value->byte_size,
                                                   type_info->prim_type
                                                               == WASM_COMP_PRIMVAL_S16
                                                           ? 16
                                                           : 32,
                                                   true,
                                                   type_info->prim_type
                                                               == WASM_COMP_PRIMVAL_S16
                                                           ? "s16"
                                                           : "s32",
                                                   position, index, &leb_value))
                return false;
            out_value->of.i32 = (int32)leb_value;
            return true;
        case WASM_COMP_PRIMVAL_U16:
        case WASM_COMP_PRIMVAL_U32:
            if (!decode_component_public_leb_value(inst, data, value->byte_size,
                                                   type_info->prim_type
                                                               == WASM_COMP_PRIMVAL_U16
                                                           ? 16
                                                           : 32,
                                                   false,
                                                   type_info->prim_type
                                                               == WASM_COMP_PRIMVAL_U16
                                                           ? "u16"
                                                           : "u32",
                                                   position, index, &leb_value))
                return false;
            out_value->of.i32 = (int32)(uint32)leb_value;
            return true;
        case WASM_COMP_PRIMVAL_S64:
            if (!decode_component_public_leb_value(inst, data, value->byte_size,
                                                   64, true, "s64", position,
                                                   index, &leb_value))
                return false;
            out_value->of.i64 = (int64)leb_value;
            return true;
        case WASM_COMP_PRIMVAL_U64:
            if (!decode_component_public_leb_value(inst, data, value->byte_size,
                                                   64, false, "u64", position,
                                                   index, &leb_value))
                return false;
            out_value->of.i64 = (uint64)leb_value;
            return true;
        case WASM_COMP_PRIMVAL_F32:
            if (value->byte_size != sizeof(float32))
                return set_component_call_error_fmt(
                    inst, "component canon lift function %s %u does not contain "
                          "a valid f32 value",
                    position, index);
            memcpy(&out_value->of.f32, data, sizeof(float32));
            return true;
        case WASM_COMP_PRIMVAL_F64:
            if (value->byte_size != sizeof(float64))
                return set_component_call_error_fmt(
                    inst, "component canon lift function %s %u does not contain "
                          "a valid f64 value",
                    position, index);
            memcpy(&out_value->of.f64, data, sizeof(float64));
            return true;
        case WASM_COMP_PRIMVAL_CHAR:
            if (!decode_component_public_char(inst, data, value->byte_size,
                                              position, index, &code_point))
                return false;
            out_value->of.i32 = (int32)code_point;
            return true;
        default:
            return set_component_call_error_fmt(
                inst, "component canon lift function %s %u uses unsupported "
                      "component scalar type %s",
                position, index, component_prim_type_name(type_info->prim_type));
    }
}

static bool
decode_component_public_string_value(
    WASMComponentInstance *inst, const wasm_component_value_t *value,
    const WASMComponentCanonLiftValueInfo *type_info, const char *position,
    uint32 index, const uint8 **payload_out, uint32 *payload_len_out);

static bool
decode_component_public_list_u8_value(
    WASMComponentInstance *inst, const wasm_component_value_t *value,
    const WASMComponentCanonLiftValueInfo *type_info, const char *position,
    uint32 index, const uint8 **payload_out, uint32 *payload_len_out);

static bool
get_component_canon_memory_bytes(WASMComponentInstance *inst,
                                 const WASMComponentRuntimeFunc *function,
                                 uint32 offset, uint32 size,
                                 const char *description, uint8 **bytes_out);

static bool
call_component_canon_realloc(WASMComponentInstance *inst,
                             const WASMComponentRuntimeFunc *function,
                             uint32 new_size, uint32 *ptr_out);

static bool
decode_component_public_char_prefix_with_context(
    WASMComponentInstance *inst, const uint8 *data, uint32 byte_size,
    const char *function_desc, const char *position, uint32 index,
    uint32 *code_point_out, uint32 *consumed_out)
{
    uint32 char_len;

    if (!data || byte_size == 0)
        return set_component_call_error_fmt(
            inst, "%s %s %u does not contain a valid char value", function_desc,
            position, index);

    if ((data[0] & 0x80) == 0)
        char_len = 1;
    else if ((data[0] & 0xE0) == 0xC0)
        char_len = 2;
    else if ((data[0] & 0xF0) == 0xE0)
        char_len = 3;
    else if ((data[0] & 0xF8) == 0xF0)
        char_len = 4;
    else
        return set_component_call_error_fmt(
            inst, "%s %s %u does not contain a valid char value", function_desc,
            position, index);

    if (char_len > byte_size)
        return set_component_call_error_fmt(
            inst, "%s %s %u does not contain a valid char value", function_desc,
            position, index);

    if (!decode_component_public_char(inst, data, char_len, position, index,
                                      code_point_out))
        return false;

    *consumed_out = char_len;
    return true;
}

static bool
decode_component_public_char_prefix(WASMComponentInstance *inst, const uint8 *data,
                                    uint32 byte_size, const char *position,
                                    uint32 index, uint32 *code_point_out,
                                    uint32 *consumed_out)
{
    return decode_component_public_char_prefix_with_context(
        inst, data, byte_size, "component canon lift function", position, index,
        code_point_out, consumed_out);
}

static bool
decode_component_public_scalar_prefix_with_context(
    WASMComponentInstance *inst, const uint8 *data, uint32 byte_size,
    uint8 prim_type, wasm_valkind_t public_kind, const char *function_desc,
    const char *position, uint32 index, wasm_val_t *out_value,
    uint32 *consumed_out)
{
    uint64 leb_value = 0;
    uint32 code_point = 0;
    size_t offset = 0;
    bh_leb_read_status_t status;

    memset(out_value, 0, sizeof(*out_value));
    out_value->kind = public_kind;

    switch (prim_type) {
        case WASM_COMP_PRIMVAL_BOOL:
            if (!data || byte_size < 1 || (data[0] != 0 && data[0] != 1))
                return set_component_call_error_fmt(
                    inst, "%s %s %u does not contain a valid bool value",
                    function_desc, position, index);
            out_value->of.i32 = data[0];
            *consumed_out = 1;
            return true;
        case WASM_COMP_PRIMVAL_S8:
            if (!data || byte_size < 1)
                return set_component_call_error_fmt(
                    inst, "%s %s %u does not contain a valid s8 value",
                    function_desc, position, index);
            out_value->of.i32 = (int8)data[0];
            *consumed_out = 1;
            return true;
        case WASM_COMP_PRIMVAL_U8:
            if (!data || byte_size < 1)
                return set_component_call_error_fmt(
                    inst, "%s %s %u does not contain a valid u8 value",
                    function_desc, position, index);
            out_value->of.i32 = data[0];
            *consumed_out = 1;
            return true;
        case WASM_COMP_PRIMVAL_S16:
        case WASM_COMP_PRIMVAL_S32:
            status = bh_leb_read(data, data + byte_size,
                                 prim_type == WASM_COMP_PRIMVAL_S16 ? 16 : 32,
                                 true, &leb_value, &offset);
            if (status != BH_LEB_READ_SUCCESS)
                return set_component_call_error_fmt(
                    inst, "%s %s %u does not contain a valid %s value",
                    function_desc, position, index,
                    prim_type == WASM_COMP_PRIMVAL_S16 ? "s16" : "s32");
            out_value->of.i32 = (int32)leb_value;
            *consumed_out = (uint32)offset;
            return true;
        case WASM_COMP_PRIMVAL_U16:
        case WASM_COMP_PRIMVAL_U32:
            status = bh_leb_read(data, data + byte_size,
                                 prim_type == WASM_COMP_PRIMVAL_U16 ? 16 : 32,
                                 false, &leb_value, &offset);
            if (status != BH_LEB_READ_SUCCESS)
                return set_component_call_error_fmt(
                    inst, "%s %s %u does not contain a valid %s value",
                    function_desc, position, index,
                    prim_type == WASM_COMP_PRIMVAL_U16 ? "u16" : "u32");
            out_value->of.i32 = (int32)(uint32)leb_value;
            *consumed_out = (uint32)offset;
            return true;
        case WASM_COMP_PRIMVAL_S64:
            status = bh_leb_read(data, data + byte_size, 64, true, &leb_value,
                                 &offset);
            if (status != BH_LEB_READ_SUCCESS)
                return set_component_call_error_fmt(
                    inst, "%s %s %u does not contain a valid s64 value",
                    function_desc, position, index);
            out_value->of.i64 = (int64)leb_value;
            *consumed_out = (uint32)offset;
            return true;
        case WASM_COMP_PRIMVAL_U64:
            status = bh_leb_read(data, data + byte_size, 64, false, &leb_value,
                                 &offset);
            if (status != BH_LEB_READ_SUCCESS)
                return set_component_call_error_fmt(
                    inst, "%s %s %u does not contain a valid u64 value",
                    function_desc, position, index);
            out_value->of.i64 = (uint64)leb_value;
            *consumed_out = (uint32)offset;
            return true;
        case WASM_COMP_PRIMVAL_F32:
            if (!data || byte_size < sizeof(float32))
                return set_component_call_error_fmt(
                    inst, "%s %s %u does not contain a valid f32 value",
                    function_desc, position, index);
            memcpy(&out_value->of.f32, data, sizeof(float32));
            *consumed_out = sizeof(float32);
            return true;
        case WASM_COMP_PRIMVAL_F64:
            if (!data || byte_size < sizeof(float64))
                return set_component_call_error_fmt(
                    inst, "%s %s %u does not contain a valid f64 value",
                    function_desc, position, index);
            memcpy(&out_value->of.f64, data, sizeof(float64));
            *consumed_out = sizeof(float64);
            return true;
        case WASM_COMP_PRIMVAL_CHAR:
            if (!decode_component_public_char_prefix_with_context(
                    inst, data, byte_size, function_desc, position, index,
                    &code_point, consumed_out))
                return false;
            out_value->of.i32 = (int32)code_point;
            return true;
        default:
            return set_component_call_error_fmt(
                inst, "%s %s %u uses unsupported component scalar type %s",
                function_desc, position, index,
                component_prim_type_name(prim_type));
    }
}

static bool
decode_component_public_scalar_prefix(
    WASMComponentInstance *inst, const uint8 *data, uint32 byte_size,
    uint8 prim_type, wasm_valkind_t public_kind, const char *position,
    uint32 index, wasm_val_t *out_value, uint32 *consumed_out)
{
    return decode_component_public_scalar_prefix_with_context(
        inst, data, byte_size, prim_type, public_kind,
        "component canon lift function", position, index, out_value,
        consumed_out);
}

static bool
decode_component_public_string_prefix_with_context(
    WASMComponentInstance *inst, const uint8 *data, uint32 byte_size,
    const char *function_desc, const char *position, uint32 index,
    const uint8 **payload_out, uint32 *payload_len_out, uint32 *consumed_out)
{
    uint64 payload_len;
    size_t offset = 0;
    bh_leb_read_status_t status;

    if (!data || byte_size == 0)
        return set_component_call_error_fmt(
            inst, "%s %s %u does not contain a valid string value",
            function_desc, position, index);

    status = bh_leb_read(data, data + byte_size, 32, false, &payload_len, &offset);
    if (status != BH_LEB_READ_SUCCESS || offset > byte_size
        || payload_len > byte_size - offset)
        return set_component_call_error_fmt(
            inst, "%s %s %u does not contain a valid string value",
            function_desc, position, index);

    if (!wasm_component_validate_utf8(data + offset, (uint32)payload_len))
        return set_component_call_error_fmt(
            inst, "%s %s %u does not contain valid UTF-8", function_desc,
            position, index);

    *payload_out = data + offset;
    *payload_len_out = (uint32)payload_len;
    *consumed_out = (uint32)offset + (uint32)payload_len;
    return true;
}

static bool
decode_component_public_string_prefix(
    WASMComponentInstance *inst, const uint8 *data, uint32 byte_size,
    const char *position, uint32 index, const uint8 **payload_out,
    uint32 *payload_len_out, uint32 *consumed_out)
{
    return decode_component_public_string_prefix_with_context(
        inst, data, byte_size, "component canon lift function", position, index,
        payload_out, payload_len_out, consumed_out);
}

static bool
decode_component_public_list_u8_prefix_with_context(
    WASMComponentInstance *inst, const uint8 *data, uint32 byte_size,
    const char *function_desc, const char *position, uint32 index,
    const uint8 **payload_out, uint32 *payload_len_out, uint32 *consumed_out)
{
    uint64 payload_len;
    size_t offset = 0;
    bh_leb_read_status_t status;

    if (!data || byte_size == 0)
        return set_component_call_error_fmt(
            inst, "%s %s %u does not contain a valid list<u8> value",
            function_desc, position, index);

    status = bh_leb_read(data, data + byte_size, 32, false, &payload_len, &offset);
    if (status != BH_LEB_READ_SUCCESS || offset > byte_size
        || payload_len > byte_size - offset)
        return set_component_call_error_fmt(
            inst, "%s %s %u does not contain a valid list<u8> value",
            function_desc, position, index);

    *payload_out = data + offset;
    *payload_len_out = (uint32)payload_len;
    *consumed_out = (uint32)offset + (uint32)payload_len;
    return true;
}

static bool
decode_component_public_list_u8_prefix(
    WASMComponentInstance *inst, const uint8 *data, uint32 byte_size,
    const char *position, uint32 index, const uint8 **payload_out,
    uint32 *payload_len_out, uint32 *consumed_out)
{
    return decode_component_public_list_u8_prefix_with_context(
        inst, data, byte_size, "component canon lift function", position, index,
        payload_out, payload_len_out, consumed_out);
}

static bool
set_component_composite_param_leaf_error(WASMComponentInstance *inst,
                                         uint32 index)
{
    return set_component_call_error_fmt(
        inst, "component canon lift function parameter %u only supports "
              "tuple/record parameters with scalar, UTF-8 string, or "
              "variable-length list<u8> leaves",
        index);
}

static bool
set_component_composite_param_list_u8_leaf_error(WASMComponentInstance *inst,
                                                 uint32 index)
{
    return set_component_call_error_fmt(
        inst, "component canon lift function parameter %u only supports "
              "variable-length list<u8> leaves inside tuple/record parameters",
        index);
}

static bool
set_component_param_flattening_error(WASMComponentInstance *inst)
{
    return set_component_call_error(
        inst, "component canon lift function uses unsupported Canonical ABI "
              "flattening for parameters");
}

static bool
validate_component_public_composite_bytes(
    WASMComponentInstance *inst, const WASMComponent *component,
    const WASMComponentValueType *value_type, const uint8 *data,
    uint32 byte_size, uint32 *offset_io, uint32 param_index)
{
    WASMComponentCanonLiftValueShape shape;

    if (!resolve_component_canon_lift_value_shape(component, value_type,
                                                  "parameter", param_index,
                                                  &shape, inst))
        return false;

    if (shape.is_primitive) {
        uint32 consumed = 0;

        if (shape.prim_type == WASM_COMP_PRIMVAL_STRING) {
            const uint8 *ignored_payload = NULL;
            uint32 ignored_payload_len = 0;

            if (!decode_component_public_string_prefix(
                    inst, data ? data + *offset_io : NULL, byte_size - *offset_io,
                    "parameter", param_index, &ignored_payload,
                    &ignored_payload_len, &consumed))
                return false;
        }
        else {
            wasm_val_t ignored;
            uint8 expected_core_type;
            wasm_valkind_t public_kind;

            if (!component_scalar_prim_to_core(shape.prim_type, &expected_core_type,
                                               &public_kind))
                return set_component_composite_param_leaf_error(inst, param_index);

            if (!decode_component_public_scalar_prefix(
                    inst, data ? data + *offset_io : NULL, byte_size - *offset_io,
                    shape.prim_type, public_kind, "parameter", param_index,
                    &ignored, &consumed))
                return false;
        }

        (*offset_io) += consumed;
        return true;
    }

    if (!shape.def_type)
        return set_component_composite_param_leaf_error(inst, param_index);

    switch (shape.def_type->tag) {
        case WASM_COMP_DEF_VAL_RECORD:
            if (!shape.def_type->def_val.record)
                return set_component_composite_param_leaf_error(inst, param_index);
            for (uint32 i = 0; i < shape.def_type->def_val.record->count; i++) {
                if (!validate_component_public_composite_bytes(
                        inst, component,
                        shape.def_type->def_val.record->fields[i].value_type, data,
                        byte_size, offset_io, param_index))
                    return false;
            }
            return true;
        case WASM_COMP_DEF_VAL_TUPLE:
            if (!shape.def_type->def_val.tuple)
                return set_component_composite_param_leaf_error(inst, param_index);
            for (uint32 i = 0; i < shape.def_type->def_val.tuple->count; i++) {
                if (!validate_component_public_composite_bytes(
                        inst, component,
                        &shape.def_type->def_val.tuple->element_types[i], data,
                        byte_size, offset_io, param_index))
                    return false;
            }
            return true;
        case WASM_COMP_DEF_VAL_LIST:
        {
            bool is_primitive = false;
            uint8 element_prim_type = 0;
            const uint8 *ignored_payload = NULL;
            uint32 ignored_payload_len = 0;
            uint32 consumed = 0;

            if (!shape.def_type->def_val.list
                || !shape.def_type->def_val.list->element_type)
                return set_component_call_error(
                    inst, "component canon lift function parameter uses malformed "
                          "list type");
            if (!lookup_component_call_primitive_type(
                    component, shape.def_type->def_val.list->element_type,
                    "parameter", param_index, &is_primitive, &element_prim_type,
                    inst))
                return false;
            if (!is_primitive || element_prim_type != WASM_COMP_PRIMVAL_U8)
                return set_component_composite_param_list_u8_leaf_error(inst,
                                                                        param_index);
            if (!decode_component_public_list_u8_prefix(
                    inst, data ? data + *offset_io : NULL, byte_size - *offset_io,
                    "parameter", param_index, &ignored_payload,
                    &ignored_payload_len, &consumed))
                return false;
            (*offset_io) += consumed;
            return true;
        }
        case WASM_COMP_DEF_VAL_LIST_LEN:
            return set_component_composite_param_list_u8_leaf_error(inst,
                                                                    param_index);
        default:
            return set_component_composite_param_leaf_error(inst, param_index);
    }
}

static bool
flatten_component_public_composite_bytes(
    WASMComponentInstance *inst, const WASMComponent *component,
    const WASMComponentRuntimeFunc *function,
    const WASMComponentValueType *value_type, const uint8 *data,
    uint32 byte_size, uint32 *offset_io, uint32 param_index,
    const WASMFuncType *core_type, wasm_val_t *core_args,
    uint32 *core_arg_index_io,
    WASMComponentCanonParamAllocationTracker *allocation_tracker)
{
    WASMComponentCanonLiftValueShape shape;

    if (!resolve_component_canon_lift_value_shape(component, value_type,
                                                  "parameter", param_index,
                                                  &shape, inst))
        return false;

    if (shape.is_primitive) {
        uint32 consumed = 0;

        if (shape.prim_type == WASM_COMP_PRIMVAL_STRING) {
            const uint8 *payload = NULL;
            uint32 payload_len = 0;
            uint32 guest_ptr = 0;
            uint8 *guest_bytes = NULL;

            if (*core_arg_index_io + 1 >= core_type->param_count)
                return set_component_param_flattening_error(inst);

            if (core_type->types[*core_arg_index_io] != VALUE_TYPE_I32
                || core_type->types[*core_arg_index_io + 1] != VALUE_TYPE_I32)
                return set_component_call_error_fmt(
                    inst, "component canon lift function parameter %u does not "
                          "match the core function signature",
                    param_index);

            if (function->string_encoding
                != WASM_COMP_RUNTIME_STRING_ENCODING_UTF8)
                return set_component_call_error(
                    inst, "component canon lift function only supports UTF-8 "
                          "string encoding");

            if (!decode_component_public_string_prefix(
                    inst, data ? data + *offset_io : NULL, byte_size - *offset_io,
                    "parameter", param_index, &payload, &payload_len, &consumed))
                return false;

            if (!call_component_canon_realloc(inst, function, payload_len,
                                              &guest_ptr))
                return false;

            if (guest_ptr != 0 && allocation_tracker
                && allocation_tracker->count < allocation_tracker->capacity) {
                allocation_tracker->allocations[allocation_tracker->count].ptr =
                    guest_ptr;
                allocation_tracker->allocations[allocation_tracker->count].size =
                    payload_len;
                allocation_tracker->count++;
            }

            if (payload_len > 0) {
                if (!get_component_canon_memory_bytes(inst, function, guest_ptr,
                                                      payload_len,
                                                      "string parameter buffer",
                                                      &guest_bytes))
                    return false;
                memcpy(guest_bytes, payload, payload_len);
            }

            core_args[*core_arg_index_io].kind = WASM_I32;
            core_args[*core_arg_index_io].of.i32 = (int32)guest_ptr;
            core_args[*core_arg_index_io + 1].kind = WASM_I32;
            core_args[*core_arg_index_io + 1].of.i32 = (int32)payload_len;
            (*offset_io) += consumed;
            (*core_arg_index_io) += 2;
            return true;
        }
        else {
            wasm_val_t flattened_value;
            uint8 expected_core_type;
            wasm_valkind_t public_kind;

            if (!component_scalar_prim_to_core(shape.prim_type, &expected_core_type,
                                               &public_kind))
                return set_component_composite_param_leaf_error(inst, param_index);

            if (*core_arg_index_io >= core_type->param_count)
                return set_component_param_flattening_error(inst);

            if (core_type->types[*core_arg_index_io] != expected_core_type)
                return set_component_call_error_fmt(
                    inst, "component canon lift function parameter %u does not "
                          "match the core function signature",
                    param_index);

            if (!decode_component_public_scalar_prefix(
                    inst, data ? data + *offset_io : NULL, byte_size - *offset_io,
                    shape.prim_type, public_kind, "parameter", param_index,
                    &flattened_value, &consumed))
                return false;

            core_args[*core_arg_index_io] = flattened_value;
            (*offset_io) += consumed;
            (*core_arg_index_io)++;
            return true;
        }
    }

    if (!shape.def_type)
        return set_component_composite_param_leaf_error(inst, param_index);

    switch (shape.def_type->tag) {
        case WASM_COMP_DEF_VAL_RECORD:
            if (!shape.def_type->def_val.record)
                return set_component_composite_param_leaf_error(inst, param_index);
            for (uint32 i = 0; i < shape.def_type->def_val.record->count; i++) {
                if (!flatten_component_public_composite_bytes(
                        inst, component, function,
                        shape.def_type->def_val.record->fields[i].value_type, data,
                        byte_size, offset_io, param_index, core_type, core_args,
                        core_arg_index_io, allocation_tracker))
                    return false;
            }
            return true;
        case WASM_COMP_DEF_VAL_TUPLE:
            if (!shape.def_type->def_val.tuple)
                return set_component_composite_param_leaf_error(inst, param_index);
            for (uint32 i = 0; i < shape.def_type->def_val.tuple->count; i++) {
                if (!flatten_component_public_composite_bytes(
                        inst, component, function,
                        &shape.def_type->def_val.tuple->element_types[i], data,
                        byte_size, offset_io, param_index, core_type, core_args,
                        core_arg_index_io, allocation_tracker))
                    return false;
            }
            return true;
        case WASM_COMP_DEF_VAL_LIST:
        {
            bool is_primitive = false;
            uint8 element_prim_type = 0;
            const uint8 *payload = NULL;
            uint32 payload_len = 0;
            uint32 guest_ptr = 0;
            uint8 *guest_bytes = NULL;
            uint32 consumed = 0;

            if (!shape.def_type->def_val.list
                || !shape.def_type->def_val.list->element_type)
                return set_component_call_error(
                    inst, "component canon lift function parameter uses malformed "
                          "list type");
            if (!lookup_component_call_primitive_type(
                    component, shape.def_type->def_val.list->element_type,
                    "parameter", param_index, &is_primitive, &element_prim_type,
                    inst))
                return false;
            if (!is_primitive || element_prim_type != WASM_COMP_PRIMVAL_U8)
                return set_component_composite_param_list_u8_leaf_error(inst,
                                                                        param_index);

            if (*core_arg_index_io + 1 >= core_type->param_count)
                return set_component_param_flattening_error(inst);

            if (core_type->types[*core_arg_index_io] != VALUE_TYPE_I32
                || core_type->types[*core_arg_index_io + 1] != VALUE_TYPE_I32)
                return set_component_call_error_fmt(
                    inst, "component canon lift function parameter %u does not "
                          "match the core function signature",
                    param_index);

            if (!decode_component_public_list_u8_prefix(
                    inst, data ? data + *offset_io : NULL, byte_size - *offset_io,
                    "parameter", param_index, &payload, &payload_len, &consumed))
                return false;

            if (!call_component_canon_realloc(inst, function, payload_len,
                                              &guest_ptr))
                return false;

            if (guest_ptr != 0 && allocation_tracker
                && allocation_tracker->count < allocation_tracker->capacity) {
                allocation_tracker->allocations[allocation_tracker->count].ptr =
                    guest_ptr;
                allocation_tracker->allocations[allocation_tracker->count].size =
                    payload_len;
                allocation_tracker->count++;
            }

            if (payload_len > 0) {
                if (!get_component_canon_memory_bytes(
                        inst, function, guest_ptr, payload_len,
                        "composite list<u8> parameter buffer", &guest_bytes))
                    return false;
                memcpy(guest_bytes, payload, payload_len);
            }

            core_args[*core_arg_index_io].kind = WASM_I32;
            core_args[*core_arg_index_io].of.i32 = (int32)guest_ptr;
            core_args[*core_arg_index_io + 1].kind = WASM_I32;
            core_args[*core_arg_index_io + 1].of.i32 = (int32)payload_len;
            (*offset_io) += consumed;
            (*core_arg_index_io) += 2;
            return true;
        }
        case WASM_COMP_DEF_VAL_LIST_LEN:
            return set_component_composite_param_list_u8_leaf_error(inst,
                                                                    param_index);
        default:
            return set_component_composite_param_leaf_error(inst, param_index);
    }
}

static bool
validate_component_public_composite_param_value(
    WASMComponentInstance *inst, const WASMComponent *component,
    const WASMComponentValueType *value_type, const wasm_component_value_t *value,
    uint32 param_index)
{
    const uint8 *data;
    uint32 offset = 0;

    if (!value)
        return set_component_call_error_fmt(
            inst, "host component function parameter %u value is null", param_index);
    if (value->type.kind != WASM_COMPONENT_VALUE_TYPE_DEFINED)
        return set_component_call_error_fmt(
            inst, "host component function parameter %u expects a defined "
                  "component value",
            param_index);

    data = wasm_component_value_get_data(value);
    if (value->byte_size > 0 && !data)
        return set_component_call_error_fmt(
            inst, "host component function parameter %u is missing backing bytes",
            param_index);

    if (!validate_component_public_composite_bytes(
            inst, component, value_type, data, value->byte_size, &offset,
            param_index))
        return false;

    if (offset != value->byte_size)
        return set_component_call_error_fmt(
            inst, "host component function parameter %u contains trailing bytes",
            param_index);
    return true;
}

static bool
set_host_component_composite_result_leaf_error(WASMComponentInstance *inst,
                                               uint32 index)
{
    return set_component_call_error_fmt(
        inst, "host component function result %u only supports tuple/record "
              "results with scalar, UTF-8 string, or variable-length list<u8> "
              "leaves",
        index);
}

static bool
set_host_component_composite_result_list_u8_leaf_error(
    WASMComponentInstance *inst, uint32 index)
{
    return set_component_call_error_fmt(
        inst, "host component function result %u only supports variable-length "
              "list<u8> leaves inside tuple/record results",
        index);
}

static bool
validate_host_component_public_composite_result_bytes(
    WASMComponentInstance *inst, const WASMComponent *component,
    const WASMComponentValueType *value_type, const uint8 *data,
    uint32 byte_size, uint32 *offset_io, uint32 result_index)
{
    WASMComponentCanonLiftValueShape shape;

    if (!resolve_component_canon_lift_value_shape(component, value_type, "result",
                                                  result_index, &shape, inst))
        return false;

    if (shape.is_primitive) {
        uint32 consumed = 0;

        if (shape.prim_type == WASM_COMP_PRIMVAL_STRING) {
            const uint8 *ignored_payload = NULL;
            uint32 ignored_payload_len = 0;

            if (!decode_component_public_string_prefix_with_context(
                    inst, data ? data + *offset_io : NULL, byte_size - *offset_io,
                    "host component function", "result", result_index,
                    &ignored_payload, &ignored_payload_len, &consumed))
                return false;
        }
        else {
            wasm_val_t ignored;
            uint8 expected_core_type;
            wasm_valkind_t public_kind;

            if (!component_scalar_prim_to_core(shape.prim_type, &expected_core_type,
                                               &public_kind))
                return set_host_component_composite_result_leaf_error(inst,
                                                                      result_index);

            if (!decode_component_public_scalar_prefix_with_context(
                    inst, data ? data + *offset_io : NULL,
                    byte_size - *offset_io, shape.prim_type, public_kind,
                    "host component function", "result", result_index, &ignored,
                    &consumed))
                return false;
        }

        (*offset_io) += consumed;
        return true;
    }

    if (!shape.def_type)
        return set_host_component_composite_result_leaf_error(inst, result_index);

    switch (shape.def_type->tag) {
        case WASM_COMP_DEF_VAL_RECORD:
            if (!shape.def_type->def_val.record)
                return set_host_component_composite_result_leaf_error(inst,
                                                                      result_index);
            for (uint32 i = 0; i < shape.def_type->def_val.record->count; i++) {
                if (!validate_host_component_public_composite_result_bytes(
                        inst, component,
                        shape.def_type->def_val.record->fields[i].value_type, data,
                        byte_size, offset_io, result_index))
                    return false;
            }
            return true;
        case WASM_COMP_DEF_VAL_TUPLE:
            if (!shape.def_type->def_val.tuple)
                return set_host_component_composite_result_leaf_error(inst,
                                                                      result_index);
            for (uint32 i = 0; i < shape.def_type->def_val.tuple->count; i++) {
                if (!validate_host_component_public_composite_result_bytes(
                        inst, component,
                        &shape.def_type->def_val.tuple->element_types[i], data,
                        byte_size, offset_io, result_index))
                    return false;
            }
            return true;
        case WASM_COMP_DEF_VAL_LIST:
        {
            bool is_primitive = false;
            uint8 element_prim_type = 0;
            const uint8 *ignored_payload = NULL;
            uint32 ignored_payload_len = 0;
            uint32 consumed = 0;

            if (!shape.def_type->def_val.list
                || !shape.def_type->def_val.list->element_type)
                return set_component_call_error(
                    inst, "host component function result uses malformed list "
                          "type");
            if (!lookup_component_call_primitive_type(
                    component, shape.def_type->def_val.list->element_type,
                    "result", result_index, &is_primitive, &element_prim_type,
                    inst))
                return false;
            if (!is_primitive || element_prim_type != WASM_COMP_PRIMVAL_U8)
                return set_host_component_composite_result_list_u8_leaf_error(
                    inst, result_index);
            if (!decode_component_public_list_u8_prefix_with_context(
                    inst, data ? data + *offset_io : NULL, byte_size - *offset_io,
                    "host component function", "result", result_index,
                    &ignored_payload, &ignored_payload_len, &consumed))
                return false;
            (*offset_io) += consumed;
            return true;
        }
        case WASM_COMP_DEF_VAL_LIST_LEN:
            return set_host_component_composite_result_list_u8_leaf_error(
                inst, result_index);
        default:
            return set_host_component_composite_result_leaf_error(inst,
                                                                  result_index);
    }
}

static bool
validate_host_component_public_composite_result_value(
    WASMComponentInstance *inst, const WASMComponent *component,
    const WASMComponentValueType *value_type, const wasm_component_value_t *value,
    uint32 result_index)
{
    const uint8 *data;
    uint32 offset = 0;

    if (!value)
        return set_component_call_error_fmt(
            inst, "host component function result %u value is null", result_index);
    if (value->type.kind != WASM_COMPONENT_VALUE_TYPE_DEFINED)
        return set_component_call_error_fmt(
            inst, "host component function result %u expects a defined component "
                  "value",
            result_index);

    data = wasm_component_value_get_data(value);
    if (value->byte_size > 0 && !data)
        return set_component_call_error_fmt(
            inst, "host component function result %u is missing backing bytes",
            result_index);

    if (!validate_host_component_public_composite_result_bytes(
            inst, component, value_type, data, value->byte_size, &offset,
            result_index))
        return false;

    if (offset != value->byte_size)
        return set_component_call_error_fmt(
            inst, "host component function result %u contains trailing bytes",
            result_index);
    return true;
}

static bool
flatten_component_public_param_value(
    WASMComponentInstance *inst, const WASMComponentRuntimeFunc *function,
    const WASMComponent *component, const WASMComponentValueType *value_type,
    const wasm_component_value_t *value, uint32 param_index,
    const WASMFuncType *core_type, wasm_val_t *core_args,
    uint32 *core_arg_index_io,
    WASMComponentCanonParamAllocationTracker *allocation_tracker)
{
    WASMComponentCanonLiftValueShape shape;
    WASMComponentCanonLiftValueInfo type_info;

    if (!resolve_component_canon_lift_value_shape(component, value_type, "parameter",
                                                  param_index, &shape, inst))
        return false;

    if (!shape.is_primitive && shape.def_type
        && (shape.def_type->tag == WASM_COMP_DEF_VAL_RECORD
            || shape.def_type->tag == WASM_COMP_DEF_VAL_TUPLE)) {
        const uint8 *data;
        uint32 offset = 0;

        if (!value)
            return set_component_call_error_fmt(
                inst, "component canon lift function parameter %u value is null",
                param_index);
        if (value->type.kind != WASM_COMPONENT_VALUE_TYPE_DEFINED)
            return set_component_call_error_fmt(
                inst, "component canon lift function parameter %u expects a "
                      "defined component value",
                param_index);

        data = wasm_component_value_get_data(value);
        if (value->byte_size > 0 && !data)
            return set_component_call_error_fmt(
                inst, "component canon lift function parameter %u is missing "
                      "backing bytes",
                param_index);

        if (!flatten_component_public_composite_bytes(
                inst, component, function, value_type, data, value->byte_size,
                &offset, param_index, core_type, core_args, core_arg_index_io,
                allocation_tracker))
            return false;

        if (offset != value->byte_size)
            return set_component_call_error_fmt(
                inst, "component canon lift function parameter %u contains "
                      "trailing bytes",
                param_index);
        return true;
    }

    if (!lookup_component_canon_lift_value_type(component, value_type, "parameter",
                                                param_index, true, true, false,
                                                &type_info, inst))
        return false;

    if (type_info.kind == WASM_COMP_CANON_LIFT_VALUE_SCALAR) {
        if (*core_arg_index_io >= core_type->param_count)
            return set_component_param_flattening_error(inst);

        if (!decode_component_public_scalar_value(
                inst, value, &type_info, "parameter", param_index,
                &core_args[*core_arg_index_io]))
            return false;

        if (core_type->types[*core_arg_index_io] != type_info.core_type)
            return set_component_call_error_fmt(
                inst, "component canon lift function parameter %u does not match "
                      "the core function signature",
                param_index);

        (*core_arg_index_io)++;
        return true;
    }
    else {
        const uint8 *payload;
        uint32 payload_len;
        uint32 guest_ptr = 0;
        uint8 *guest_bytes = NULL;

        if (*core_arg_index_io + 1 >= core_type->param_count)
            return set_component_param_flattening_error(inst);

        if (type_info.kind == WASM_COMP_CANON_LIFT_VALUE_STRING) {
            if (function->string_encoding != WASM_COMP_RUNTIME_STRING_ENCODING_UTF8)
                return set_component_call_error(
                    inst, "component canon lift function only supports UTF-8 "
                          "string encoding");

            if (!decode_component_public_string_value(inst, value, &type_info,
                                                      "parameter", param_index,
                                                      &payload, &payload_len))
                return false;

            if (!call_component_canon_realloc(inst, function, payload_len,
                                              &guest_ptr))
                return false;
        }
        else if (!decode_component_public_list_u8_value(
                     inst, value, &type_info, "parameter", param_index,
                     &payload, &payload_len)
                 || !call_component_canon_realloc(inst, function, payload_len,
                                                  &guest_ptr))
            return false;

        if (guest_ptr != 0 && allocation_tracker
            && allocation_tracker->count < allocation_tracker->capacity) {
            allocation_tracker->allocations[allocation_tracker->count].ptr =
                guest_ptr;
            allocation_tracker->allocations[allocation_tracker->count].size =
                payload_len;
            allocation_tracker->count++;
        }

        if (payload_len > 0) {
            if (!get_component_canon_memory_bytes(
                    inst, function, guest_ptr, payload_len,
                    type_info.kind == WASM_COMP_CANON_LIFT_VALUE_STRING
                        ? "string parameter buffer"
                        : "list<u8> parameter buffer",
                    &guest_bytes))
                return false;
            memcpy(guest_bytes, payload, payload_len);
        }

        if (core_type->types[*core_arg_index_io] != VALUE_TYPE_I32
            || core_type->types[*core_arg_index_io + 1] != VALUE_TYPE_I32)
            return set_component_call_error_fmt(
                inst, "component canon lift function parameter %u does not match "
                      "the core function signature",
                param_index);

        core_args[*core_arg_index_io].kind = WASM_I32;
        core_args[*core_arg_index_io].of.i32 = (int32)guest_ptr;
        core_args[*core_arg_index_io + 1].kind = WASM_I32;
        core_args[*core_arg_index_io + 1].of.i32 = (int32)payload_len;
        (*core_arg_index_io) += 2;
        return true;
    }
}

static bool
decode_component_public_string_value(
    WASMComponentInstance *inst, const wasm_component_value_t *value,
    const WASMComponentCanonLiftValueInfo *type_info, const char *position,
    uint32 index, const uint8 **payload_out, uint32 *payload_len_out)
{
    const uint8 *data = wasm_component_value_get_data(value);
    uint64 payload_len;
    size_t offset = 0;
    bh_leb_read_status_t status;

    if (!validate_component_public_value_type(inst, value, type_info, position,
                                              index))
        return false;

    if (!data || value->byte_size == 0)
        return set_component_call_error_fmt(
            inst, "component canon lift function %s %u is missing string bytes",
            position, index);

    status = bh_leb_read(data, data + value->byte_size, 32, false, &payload_len,
                         &offset);
    if (status != BH_LEB_READ_SUCCESS || offset > value->byte_size
        || payload_len > value->byte_size - offset
        || offset + payload_len != value->byte_size)
        return set_component_call_error_fmt(
            inst, "component canon lift function %s %u does not contain a valid "
                  "string value",
            position, index);

    if (!wasm_component_validate_utf8(data + offset, (uint32)payload_len))
        return set_component_call_error_fmt(
            inst, "component canon lift function %s %u does not contain valid "
                  "UTF-8",
            position, index);

    *payload_out = data + offset;
    *payload_len_out = (uint32)payload_len;
    return true;
}

static bool
decode_component_public_list_u8_value(
    WASMComponentInstance *inst, const wasm_component_value_t *value,
    const WASMComponentCanonLiftValueInfo *type_info, const char *position,
    uint32 index, const uint8 **payload_out, uint32 *payload_len_out)
{
    const uint8 *data = wasm_component_value_get_data(value);

    if (!validate_component_public_value_type(inst, value, type_info, position,
                                              index))
        return false;

    if (value->byte_size > 0 && !data)
        return set_component_call_error_fmt(
            inst, "component canon lift function %s %u is missing backing bytes",
            position, index);

    *payload_out = data;
    *payload_len_out = value->byte_size;
    return true;
}

static bool
init_component_public_result_type(
    wasm_component_value_t *value,
    const WASMComponentCanonLiftValueInfo *type_info)
{
    wasm_component_value_destroy(value);
    if (type_info->declared_as_defined)
        value->type.kind = WASM_COMPONENT_VALUE_TYPE_DEFINED;
    else {
        value->type.kind = WASM_COMPONENT_VALUE_TYPE_PRIMITIVE;
        value->type.type.primitive_type =
            (wasm_component_primitive_value_kind_t)type_info->prim_type;
    }
    return true;
}

static bool
copy_component_memory_payload(WASMComponentInstance *inst, const uint8 *payload,
                              uint32 payload_len, const char *description,
                              uint8 **copy_out)
{
    uint8 *payload_copy = NULL;

    *copy_out = NULL;
    if (payload_len == 0)
        return true;

    payload_copy = wasm_runtime_malloc(payload_len);
    if (!payload_copy)
        return set_component_call_error_fmt(
            inst, "component canon lift function could not allocate %s storage",
            description);

    memcpy(payload_copy, payload, payload_len);
    *copy_out = payload_copy;
    return true;
}

static bool
init_component_public_scalar_result(
    WASMComponentInstance *inst, const WASMComponentCanonLiftValueInfo *type_info,
    const wasm_val_t *result, wasm_component_value_t *value);

typedef struct WASMComponentCompositeFlatLeaf {
    WASMComponentCanonLiftValueInfo type_info;
} WASMComponentCompositeFlatLeaf;

typedef struct WASMComponentResultPayloadBuilder {
    uint8 inline_storage[WASM_COMPONENT_VALUE_INLINE_STORAGE_SIZE];
    uint8 *storage;
    uint32 size;
    uint32 capacity;
} WASMComponentResultPayloadBuilder;

static bool
set_component_composite_result_leaf_error(WASMComponentInstance *inst,
                                          uint32 index)
{
    return set_component_call_error_fmt(
        inst, "component canon lift function result %u only supports "
              "tuple/record results with scalar, UTF-8 string, or "
              "variable-length list<u8> leaves",
        index);
}

static bool
set_component_result_flattening_error(WASMComponentInstance *inst)
{
    return set_component_call_error(
        inst, "component canon lift function uses unsupported Canonical ABI "
              "flattening for results");
}

static bool
align_component_canon_abi_offset(uint32 offset, uint32 align, uint32 *aligned_out)
{
    uint64 aligned;

    if (align == 0 || (align & (align - 1)) != 0)
        return false;

    aligned = ((uint64)offset + (uint64)align - 1) & ~((uint64)align - 1);
    if (aligned > UINT32_MAX)
        return false;

    *aligned_out = (uint32)aligned;
    return true;
}

static bool
lookup_component_canon_abi_scalar_size_align(uint8 prim_type, uint32 *size_out,
                                             uint32 *align_out)
{
    switch (prim_type) {
        case WASM_COMP_PRIMVAL_BOOL:
        case WASM_COMP_PRIMVAL_S8:
        case WASM_COMP_PRIMVAL_U8:
            *size_out = 1;
            *align_out = 1;
            return true;
        case WASM_COMP_PRIMVAL_S16:
        case WASM_COMP_PRIMVAL_U16:
            *size_out = 2;
            *align_out = 2;
            return true;
        case WASM_COMP_PRIMVAL_S32:
        case WASM_COMP_PRIMVAL_U32:
        case WASM_COMP_PRIMVAL_F32:
        case WASM_COMP_PRIMVAL_CHAR:
            *size_out = 4;
            *align_out = 4;
            return true;
        case WASM_COMP_PRIMVAL_S64:
        case WASM_COMP_PRIMVAL_U64:
        case WASM_COMP_PRIMVAL_F64:
            *size_out = 8;
            *align_out = 8;
            return true;
        default:
            return false;
    }
}

static bool
compute_component_canon_abi_layout(WASMComponentInstance *inst,
                                   const WASMComponent *component,
                                   const WASMComponentValueType *value_type,
                                   uint32 result_index, uint32 *size_out,
                                   uint32 *align_out, bool *has_string_leaf_out,
                                   bool *has_list_u8_leaf_out)
{
    WASMComponentCanonLiftValueShape shape;

    *size_out = 0;
    *align_out = 1;
    *has_string_leaf_out = false;
    *has_list_u8_leaf_out = false;

    if (!resolve_component_canon_lift_value_shape(component, value_type, "result",
                                                  result_index, &shape, inst))
        return false;

    if (shape.is_primitive) {
        if (shape.prim_type == WASM_COMP_PRIMVAL_STRING) {
            *size_out = 8;
            *align_out = 4;
            *has_string_leaf_out = true;
            return true;
        }

        if (!lookup_component_canon_abi_scalar_size_align(shape.prim_type, size_out,
                                                          align_out))
            return set_component_composite_result_leaf_error(inst, result_index);
        return true;
    }

    if (!shape.def_type)
        return set_component_composite_result_leaf_error(inst, result_index);

    switch (shape.def_type->tag) {
        case WASM_COMP_DEF_VAL_RECORD:
        {
            uint32 size = 0, max_align = 1;

            if (!shape.def_type->def_val.record)
                return set_component_composite_result_leaf_error(inst, result_index);

            for (uint32 i = 0; i < shape.def_type->def_val.record->count; i++) {
                uint32 field_size = 0, field_align = 1, field_offset = 0;
                bool field_has_string = false;
                bool field_has_list_u8 = false;
                uint64 next_size;

                if (!compute_component_canon_abi_layout(
                        inst, component,
                        shape.def_type->def_val.record->fields[i].value_type,
                        result_index, &field_size, &field_align,
                        &field_has_string, &field_has_list_u8))
                    return false;

                if (!align_component_canon_abi_offset(size, field_align,
                                                      &field_offset))
                    return set_component_call_error(
                        inst, "component canon lift function result area is too "
                              "large");

                next_size = (uint64)field_offset + field_size;
                if (next_size > UINT32_MAX)
                    return set_component_call_error(
                        inst, "component canon lift function result area is too "
                              "large");

                size = (uint32)next_size;
                if (field_align > max_align)
                    max_align = field_align;
                *has_string_leaf_out = *has_string_leaf_out || field_has_string;
                *has_list_u8_leaf_out =
                    *has_list_u8_leaf_out || field_has_list_u8;
            }

            if (!align_component_canon_abi_offset(size, max_align, size_out))
                return set_component_call_error(
                    inst, "component canon lift function result area is too "
                          "large");
            *align_out = max_align;
            return true;
        }
        case WASM_COMP_DEF_VAL_TUPLE:
        {
            uint32 size = 0, max_align = 1;

            if (!shape.def_type->def_val.tuple)
                return set_component_composite_result_leaf_error(inst, result_index);

            for (uint32 i = 0; i < shape.def_type->def_val.tuple->count; i++) {
                uint32 field_size = 0, field_align = 1, field_offset = 0;
                bool field_has_string = false;
                bool field_has_list_u8 = false;
                uint64 next_size;

                if (!compute_component_canon_abi_layout(
                        inst, component,
                        &shape.def_type->def_val.tuple->element_types[i],
                        result_index, &field_size, &field_align,
                        &field_has_string, &field_has_list_u8))
                    return false;

                if (!align_component_canon_abi_offset(size, field_align,
                                                      &field_offset))
                    return set_component_call_error(
                        inst, "component canon lift function result area is too "
                              "large");

                next_size = (uint64)field_offset + field_size;
                if (next_size > UINT32_MAX)
                    return set_component_call_error(
                        inst, "component canon lift function result area is too "
                              "large");

                size = (uint32)next_size;
                if (field_align > max_align)
                    max_align = field_align;
                *has_string_leaf_out = *has_string_leaf_out || field_has_string;
                *has_list_u8_leaf_out =
                    *has_list_u8_leaf_out || field_has_list_u8;
            }

            if (!align_component_canon_abi_offset(size, max_align, size_out))
                return set_component_call_error(
                    inst, "component canon lift function result area is too "
                          "large");
            *align_out = max_align;
            return true;
        }
        case WASM_COMP_DEF_VAL_LIST:
        {
            bool is_primitive = false;
            uint8 element_prim_type = 0;

            if (!shape.def_type->def_val.list
                || !shape.def_type->def_val.list->element_type)
                return set_component_call_error(
                    inst, "component canon lift function result uses malformed "
                          "list type");
            if (!lookup_component_call_primitive_type(
                    component, shape.def_type->def_val.list->element_type,
                    "result", result_index, &is_primitive, &element_prim_type,
                    inst))
                return false;
            if (!is_primitive || element_prim_type != WASM_COMP_PRIMVAL_U8)
                return set_component_call_error_fmt(
                    inst, "component canon lift function result %u only supports "
                          "variable-length list<u8> leaves inside tuple/record "
                          "results",
                    result_index);

            *size_out = 8;
            *align_out = 4;
            *has_list_u8_leaf_out = true;
            return true;
        }
        case WASM_COMP_DEF_VAL_LIST_LEN:
            return set_component_call_error_fmt(
                inst, "component canon lift function result %u only supports "
                      "variable-length list<u8> leaves inside tuple/record "
                      "results",
                result_index);
        default:
            return set_component_composite_result_leaf_error(inst, result_index);
    }
}

static void
init_component_result_payload_builder(
    WASMComponentResultPayloadBuilder *builder)
{
    memset(builder, 0, sizeof(*builder));
    builder->storage = builder->inline_storage;
    builder->capacity = sizeof(builder->inline_storage);
}

static void
destroy_component_result_payload_builder(
    WASMComponentResultPayloadBuilder *builder)
{
    if (builder->storage != builder->inline_storage && builder->storage)
        wasm_runtime_free(builder->storage);
}

static bool
append_component_result_payload_bytes(
    WASMComponentInstance *inst, WASMComponentResultPayloadBuilder *builder,
    const uint8 *bytes, uint32 byte_count)
{
    uint32 required_capacity;
    uint32 new_capacity;
    uint8 *new_storage;

    if (byte_count == 0)
        return true;

    if (!bytes)
        return set_component_call_error(
            inst, "component canon lift function result is missing backing bytes");

    required_capacity = builder->size + byte_count;
    if (required_capacity < builder->size)
        return set_component_call_error(
            inst, "component canon lift function result is too large");

    if (required_capacity > builder->capacity) {
        new_capacity = builder->capacity;
        while (new_capacity < required_capacity) {
            if (new_capacity > UINT32_MAX / 2)
                new_capacity = required_capacity;
            else
                new_capacity *= 2;
        }

        new_storage = wasm_runtime_malloc(new_capacity);
        if (!new_storage)
            return set_component_call_error(
                inst, "component canon lift function could not allocate result "
                      "storage");

        if (builder->size > 0)
            memcpy(new_storage, builder->storage, builder->size);
        if (builder->storage != builder->inline_storage)
            wasm_runtime_free(builder->storage);
        builder->storage = new_storage;
        builder->capacity = new_capacity;
    }

    memcpy(builder->storage + builder->size, bytes, byte_count);
    builder->size += byte_count;
    return true;
}

static bool
finish_component_result_payload(
    wasm_component_value_t *value, WASMComponentResultPayloadBuilder *builder)
{
    wasm_component_value_destroy(value);
    value->type.kind = WASM_COMPONENT_VALUE_TYPE_DEFINED;
    value->byte_size = builder->size;

    if (builder->size == 0) {
        builder->storage = builder->inline_storage;
        builder->capacity = sizeof(builder->inline_storage);
        return true;
    }

    if (builder->storage == builder->inline_storage) {
        value->storage_kind = WASM_COMPONENT_VALUE_STORAGE_INLINE;
        memcpy(value->storage.inline_storage, builder->inline_storage,
               builder->size);
        return true;
    }

    value->storage_kind = WASM_COMPONENT_VALUE_STORAGE_OWNED;
    value->storage.owned_data = builder->storage;
    builder->storage = builder->inline_storage;
    builder->capacity = sizeof(builder->inline_storage);
    builder->size = 0;
    return true;
}

static bool
decode_component_canon_memory_scalar_value(
    WASMComponentInstance *inst, uint8 prim_type, const uint8 *data,
    uint32 byte_size, wasm_val_t *out_value, wasm_valkind_t *public_kind_out)
{
    out_value->kind = WASM_I32;

    switch (prim_type) {
        case WASM_COMP_PRIMVAL_BOOL:
            if (byte_size != 1 || (data[0] != 0 && data[0] != 1))
                return set_component_call_error(
                    inst, "component canon lift function result is not a valid "
                          "bool value");
            out_value->kind = WASM_I32;
            out_value->of.i32 = (int32)data[0];
            *public_kind_out = WASM_I32;
            return true;
        case WASM_COMP_PRIMVAL_S8:
            if (byte_size != 1)
                break;
            out_value->kind = WASM_I32;
            out_value->of.i32 = (int32)(int8)data[0];
            *public_kind_out = WASM_I32;
            return true;
        case WASM_COMP_PRIMVAL_U8:
            if (byte_size != 1)
                break;
            out_value->kind = WASM_I32;
            out_value->of.i32 = (int32)data[0];
            *public_kind_out = WASM_I32;
            return true;
        case WASM_COMP_PRIMVAL_S16:
            if (byte_size != 2)
                break;
            out_value->kind = WASM_I32;
            out_value->of.i32 =
                (int32)(int16)((uint16)data[0] | ((uint16)data[1] << 8));
            *public_kind_out = WASM_I32;
            return true;
        case WASM_COMP_PRIMVAL_U16:
            if (byte_size != 2)
                break;
            out_value->kind = WASM_I32;
            out_value->of.i32 =
                (int32)((uint16)data[0] | ((uint16)data[1] << 8));
            *public_kind_out = WASM_I32;
            return true;
        case WASM_COMP_PRIMVAL_S32:
            if (byte_size != 4)
                break;
            out_value->kind = WASM_I32;
            out_value->of.i32 =
                (int32)((uint32)data[0] | ((uint32)data[1] << 8)
                        | ((uint32)data[2] << 16) | ((uint32)data[3] << 24));
            *public_kind_out = WASM_I32;
            return true;
        case WASM_COMP_PRIMVAL_U32:
        case WASM_COMP_PRIMVAL_CHAR:
            if (byte_size != 4)
                break;
            out_value->kind = WASM_I32;
            out_value->of.i32 =
                (int32)((uint32)data[0] | ((uint32)data[1] << 8)
                        | ((uint32)data[2] << 16) | ((uint32)data[3] << 24));
            *public_kind_out = WASM_I32;
            return true;
        case WASM_COMP_PRIMVAL_S64:
            if (byte_size != 8)
                break;
            out_value->kind = WASM_I64;
            out_value->of.i64 = (int64)(
                (uint64)data[0] | ((uint64)data[1] << 8) | ((uint64)data[2] << 16)
                | ((uint64)data[3] << 24) | ((uint64)data[4] << 32)
                | ((uint64)data[5] << 40) | ((uint64)data[6] << 48)
                | ((uint64)data[7] << 56));
            *public_kind_out = WASM_I64;
            return true;
        case WASM_COMP_PRIMVAL_U64:
            if (byte_size != 8)
                break;
            out_value->kind = WASM_I64;
            out_value->of.i64 = (uint64)(
                (uint64)data[0] | ((uint64)data[1] << 8) | ((uint64)data[2] << 16)
                | ((uint64)data[3] << 24) | ((uint64)data[4] << 32)
                | ((uint64)data[5] << 40) | ((uint64)data[6] << 48)
                | ((uint64)data[7] << 56));
            *public_kind_out = WASM_I64;
            return true;
        case WASM_COMP_PRIMVAL_F32:
            if (byte_size != 4)
                break;
            out_value->kind = WASM_F32;
            memcpy(&out_value->of.f32, data, sizeof(out_value->of.f32));
            *public_kind_out = WASM_F32;
            return true;
        case WASM_COMP_PRIMVAL_F64:
            if (byte_size != 8)
                break;
            out_value->kind = WASM_F64;
            memcpy(&out_value->of.f64, data, sizeof(out_value->of.f64));
            *public_kind_out = WASM_F64;
            return true;
        default:
            break;
    }

    return set_component_composite_result_leaf_error(inst, 0);
}

static bool
append_component_result_string_leaf(WASMComponentInstance *inst,
                                    WASMComponentResultPayloadBuilder *builder,
                                    const uint8 *payload, uint32 payload_len)
{
    uint8 len_buf[5];
    uint32 len_len = encode_component_unsigned_leb(payload_len, len_buf);

    return append_component_result_payload_bytes(inst, builder, len_buf, len_len)
           && append_component_result_payload_bytes(inst, builder, payload,
                                                    payload_len);
}

static bool
append_component_result_list_u8_leaf(WASMComponentInstance *inst,
                                     WASMComponentResultPayloadBuilder *builder,
                                     const uint8 *payload, uint32 payload_len)
{
    uint8 len_buf[5];
    uint32 len_len = encode_component_unsigned_leb(payload_len, len_buf);

    return append_component_result_payload_bytes(inst, builder, len_buf, len_len)
           && append_component_result_payload_bytes(inst, builder, payload,
                                                    payload_len);
}

static bool
decode_component_canon_composite_result_value(
    WASMComponentInstance *inst, const WASMComponentRuntimeFunc *function,
    const WASMComponent *component, const WASMComponentValueType *value_type,
    uint32 result_index, const uint8 *ret_area_bytes, uint32 ret_area_size,
    uint32 offset, WASMComponentResultPayloadBuilder *builder)
{
    WASMComponentCanonLiftValueShape shape;

    if (!resolve_component_canon_lift_value_shape(component, value_type, "result",
                                                  result_index, &shape, inst))
        return false;

    if (shape.is_primitive) {
        if (shape.prim_type == WASM_COMP_PRIMVAL_STRING) {
            uint32 payload_ptr = 0, payload_len = 0;
            uint8 *payload_bytes = NULL;

            if (offset > ret_area_size || ret_area_size - offset < 8)
                return set_component_call_error(
                    inst, "component canon lift function composite result area "
                          "is out of bounds");

            memcpy(&payload_ptr, ret_area_bytes + offset, sizeof(payload_ptr));
            memcpy(&payload_len, ret_area_bytes + offset + sizeof(payload_ptr),
                   sizeof(payload_len));

            if (payload_len > 0) {
                if (!get_component_canon_memory_bytes(
                        inst, function, payload_ptr, payload_len,
                        "composite string result payload", &payload_bytes))
                    return false;
                if (!wasm_component_validate_utf8(payload_bytes, payload_len))
                    return set_component_call_error(
                        inst, "component canon lift function result does not "
                              "contain valid UTF-8");
            }

            return append_component_result_string_leaf(inst, builder, payload_bytes,
                                                       payload_len);
        }
        else {
            WASMComponentCanonLiftValueInfo type_info;
            wasm_component_value_t leaf_value = { 0 };
            wasm_val_t leaf_result = { 0 };
            const uint8 *leaf_bytes;
            uint32 leaf_size = 0, leaf_align = 1, leaf_offset = 0;

            if (!lookup_component_canon_abi_scalar_size_align(shape.prim_type,
                                                              &leaf_size,
                                                              &leaf_align)
                || !align_component_canon_abi_offset(offset, leaf_align,
                                                     &leaf_offset)
                || leaf_offset > ret_area_size
                || ret_area_size - leaf_offset < leaf_size)
                return set_component_call_error(
                    inst, "component canon lift function composite result area "
                          "is out of bounds");

            memset(&type_info, 0, sizeof(type_info));
            type_info.kind = WASM_COMP_CANON_LIFT_VALUE_SCALAR;
            type_info.prim_type = shape.prim_type;
            if (!component_scalar_prim_to_core(shape.prim_type, &type_info.core_type,
                                               &type_info.public_kind))
                return set_component_composite_result_leaf_error(inst, result_index);

            if (!decode_component_canon_memory_scalar_value(
                    inst, shape.prim_type, ret_area_bytes + leaf_offset, leaf_size,
                    &leaf_result, &type_info.public_kind)
                || !init_component_public_scalar_result(inst, &type_info,
                                                        &leaf_result, &leaf_value))
                return false;

            leaf_bytes = wasm_component_value_get_data(&leaf_value);
            if (!append_component_result_payload_bytes(inst, builder, leaf_bytes,
                                                       leaf_value.byte_size)) {
                wasm_component_value_destroy(&leaf_value);
                return false;
            }
            wasm_component_value_destroy(&leaf_value);
            return true;
        }
    }

    if (!shape.def_type)
        return set_component_composite_result_leaf_error(inst, result_index);

    switch (shape.def_type->tag) {
        case WASM_COMP_DEF_VAL_RECORD:
        {
            uint32 cursor = 0;

            if (!shape.def_type->def_val.record)
                return set_component_composite_result_leaf_error(inst, result_index);
            for (uint32 i = 0; i < shape.def_type->def_val.record->count; i++) {
                uint32 field_size = 0, field_align = 1, field_offset = 0;
                bool ignored_has_string = false;
                bool ignored_has_list_u8 = false;
                uint64 nested_offset;

                if (!compute_component_canon_abi_layout(
                        inst, component,
                        shape.def_type->def_val.record->fields[i].value_type,
                        result_index, &field_size, &field_align,
                        &ignored_has_string, &ignored_has_list_u8)
                    || !align_component_canon_abi_offset(cursor, field_align,
                                                         &field_offset))
                    return false;

                nested_offset = (uint64)offset + field_offset;
                if (nested_offset > UINT32_MAX)
                    return set_component_call_error(
                        inst, "component canon lift function composite result "
                              "area is too large");

                if (!decode_component_canon_composite_result_value(
                        inst, function, component,
                        shape.def_type->def_val.record->fields[i].value_type,
                        result_index, ret_area_bytes, ret_area_size,
                        (uint32)nested_offset, builder))
                    return false;

                cursor = field_offset + field_size;
            }
            return true;
        }
        case WASM_COMP_DEF_VAL_TUPLE:
        {
            uint32 cursor = 0;

            if (!shape.def_type->def_val.tuple)
                return set_component_composite_result_leaf_error(inst, result_index);
            for (uint32 i = 0; i < shape.def_type->def_val.tuple->count; i++) {
                uint32 field_size = 0, field_align = 1, field_offset = 0;
                bool ignored_has_string = false;
                bool ignored_has_list_u8 = false;
                uint64 nested_offset;

                if (!compute_component_canon_abi_layout(
                        inst, component,
                        &shape.def_type->def_val.tuple->element_types[i],
                        result_index, &field_size, &field_align,
                        &ignored_has_string, &ignored_has_list_u8)
                    || !align_component_canon_abi_offset(cursor, field_align,
                                                         &field_offset))
                    return false;

                nested_offset = (uint64)offset + field_offset;
                if (nested_offset > UINT32_MAX)
                    return set_component_call_error(
                        inst, "component canon lift function composite result "
                              "area is too large");

                if (!decode_component_canon_composite_result_value(
                        inst, function, component,
                        &shape.def_type->def_val.tuple->element_types[i],
                        result_index, ret_area_bytes, ret_area_size,
                        (uint32)nested_offset, builder))
                    return false;

                cursor = field_offset + field_size;
            }
            return true;
        }
        case WASM_COMP_DEF_VAL_LIST:
        {
            bool is_primitive = false;
            uint8 element_prim_type = 0;
            uint32 payload_ptr = 0, payload_len = 0;
            uint8 *payload_bytes = NULL;

            if (!shape.def_type->def_val.list
                || !shape.def_type->def_val.list->element_type)
                return set_component_call_error(
                    inst, "component canon lift function result uses malformed "
                          "list type");
            if (!lookup_component_call_primitive_type(
                    component, shape.def_type->def_val.list->element_type,
                    "result", result_index, &is_primitive, &element_prim_type,
                    inst))
                return false;
            if (!is_primitive || element_prim_type != WASM_COMP_PRIMVAL_U8)
                return set_component_call_error_fmt(
                    inst, "component canon lift function result %u only supports "
                          "variable-length list<u8> leaves inside tuple/record "
                          "results",
                    result_index);

            if (offset > ret_area_size || ret_area_size - offset < 8)
                return set_component_call_error(
                    inst, "component canon lift function composite result area "
                          "is out of bounds");

            memcpy(&payload_ptr, ret_area_bytes + offset, sizeof(payload_ptr));
            memcpy(&payload_len, ret_area_bytes + offset + sizeof(payload_ptr),
                   sizeof(payload_len));

            if (payload_len > 0
                && !get_component_canon_memory_bytes(
                    inst, function, payload_ptr, payload_len,
                    "composite list<u8> result payload", &payload_bytes))
                return false;

            return append_component_result_list_u8_leaf(inst, builder,
                                                        payload_bytes, payload_len);
        }
        case WASM_COMP_DEF_VAL_LIST_LEN:
            return set_component_call_error_fmt(
                inst, "component canon lift function result %u only supports "
                      "variable-length list<u8> leaves inside tuple/record "
                      "results",
                result_index);
        default:
            return set_component_composite_result_leaf_error(inst, result_index);
    }
}

static bool
collect_component_composite_result_leaves(
    WASMComponentInstance *inst, const WASMComponent *component,
    const WASMComponentValueType *value_type, uint32 result_index,
    WASMComponentCompositeFlatLeaf *flat_leaves, uint32 leaf_capacity,
    uint32 *leaf_count_io)
{
    WASMComponentCanonLiftValueShape shape;

    if (!resolve_component_canon_lift_value_shape(component, value_type, "result",
                                                  result_index, &shape, inst))
        return false;

    if (shape.is_primitive) {
        WASMComponentCompositeFlatLeaf *flat_leaf;

        if (*leaf_count_io >= leaf_capacity)
            return set_component_result_flattening_error(inst);

        flat_leaf = &flat_leaves[*leaf_count_io];
        memset(flat_leaf, 0, sizeof(*flat_leaf));
        flat_leaf->type_info.kind = WASM_COMP_CANON_LIFT_VALUE_SCALAR;
        flat_leaf->type_info.prim_type = shape.prim_type;
        if (!component_scalar_prim_to_core(shape.prim_type,
                                           &flat_leaf->type_info.core_type,
                                           &flat_leaf->type_info.public_kind))
            return set_component_composite_result_leaf_error(inst, result_index);
        (*leaf_count_io)++;
        return true;
    }

    if (!shape.def_type)
        return set_component_composite_result_leaf_error(inst, result_index);

    switch (shape.def_type->tag) {
        case WASM_COMP_DEF_VAL_RECORD:
            if (!shape.def_type->def_val.record)
                return set_component_composite_result_leaf_error(inst, result_index);
            for (uint32 i = 0; i < shape.def_type->def_val.record->count; i++) {
                if (!collect_component_composite_result_leaves(
                        inst, component,
                        shape.def_type->def_val.record->fields[i].value_type,
                        result_index, flat_leaves, leaf_capacity, leaf_count_io))
                    return false;
            }
            return true;
        case WASM_COMP_DEF_VAL_TUPLE:
            if (!shape.def_type->def_val.tuple)
                return set_component_composite_result_leaf_error(inst, result_index);
            for (uint32 i = 0; i < shape.def_type->def_val.tuple->count; i++) {
                if (!collect_component_composite_result_leaves(
                        inst, component,
                        &shape.def_type->def_val.tuple->element_types[i],
                        result_index, flat_leaves, leaf_capacity, leaf_count_io))
                    return false;
            }
            return true;
        default:
            return set_component_composite_result_leaf_error(inst, result_index);
    }
}

static bool
validate_component_composite_result_signature(
    WASMComponentInstance *inst, const WASMComponent *component,
    const WASMComponentValueType *value_type, uint32 result_index,
    const WASMFuncType *core_type,
    WASMComponentCompositeFlatLeaf *flat_leaves, uint32 flat_leaf_capacity,
    uint32 *flat_leaf_count_out)
{
    uint32 i;

    *flat_leaf_count_out = 0;
    if (!collect_component_composite_result_leaves(
            inst, component, value_type, result_index, flat_leaves,
            flat_leaf_capacity, flat_leaf_count_out))
        return false;

    if (*flat_leaf_count_out != core_type->result_count)
        return set_component_result_flattening_error(inst);

    for (i = 0; i < *flat_leaf_count_out; i++) {
        if (core_type->types[core_type->param_count + i]
            != flat_leaves[i].type_info.core_type)
            return set_component_call_error(
                inst, "component canon lift function result does not match the "
                      "core function signature");
    }

    return true;
}

static bool
init_component_public_memory_composite_result(
    WASMComponentInstance *inst, const WASMComponentRuntimeFunc *function,
    const WASMComponent *component, const WASMComponentValueType *value_type,
    uint32 result_index, uint32 retptr, wasm_component_value_t *value)
{
    WASMComponentResultPayloadBuilder builder;
    uint8 *ret_area_bytes = NULL;
    uint32 ret_area_size = 0, ret_area_align = 1;
    bool has_string_leaf = false;
    bool has_list_u8_leaf = false;

    if (!compute_component_canon_abi_layout(inst, component, value_type, result_index,
                                            &ret_area_size, &ret_area_align,
                                            &has_string_leaf, &has_list_u8_leaf))
        return false;
    (void)ret_area_align;

    if (!has_string_leaf && !has_list_u8_leaf)
        return set_component_call_error(
            inst, "component canon lift function result does not require "
                  "memory-backed lifting");

    if (!get_component_canon_memory_bytes(inst, function, retptr, ret_area_size,
                                          "composite result area",
                                          &ret_area_bytes))
        return false;

    init_component_result_payload_builder(&builder);
    if (!decode_component_canon_composite_result_value(
            inst, function, component, value_type, result_index, ret_area_bytes,
            ret_area_size, 0, &builder)
        || !finish_component_result_payload(value, &builder)) {
        destroy_component_result_payload_builder(&builder);
        return false;
    }

    return true;
}

static bool
init_component_public_composite_result(
    WASMComponentInstance *inst,
    const WASMComponentCompositeFlatLeaf *flat_leaves, uint32 flat_leaf_count,
    const wasm_val_t *core_results, wasm_component_value_t *value)
{
    WASMComponentResultPayloadBuilder builder;
    wasm_component_value_t leaf_value = { 0 };
    const uint8 *leaf_bytes;
    uint32 i;

    init_component_result_payload_builder(&builder);

    for (i = 0; i < flat_leaf_count; i++) {
        if (!validate_component_scalar_value(inst, &core_results[i],
                                             flat_leaves[i].type_info.public_kind,
                                             flat_leaves[i].type_info.prim_type,
                                             "result", 0)
            || !init_component_public_scalar_result(
                inst, &flat_leaves[i].type_info, &core_results[i], &leaf_value)) {
            destroy_component_result_payload_builder(&builder);
            return false;
        }

        leaf_bytes = wasm_component_value_get_data(&leaf_value);
        if (!append_component_result_payload_bytes(inst, &builder, leaf_bytes,
                                                   leaf_value.byte_size)) {
            destroy_component_result_payload_builder(&builder);
            return false;
        }
    }

    if (!finish_component_result_payload(value, &builder)) {
        destroy_component_result_payload_builder(&builder);
        return false;
    }
    return true;
}

static bool
init_component_public_scalar_result(
    WASMComponentInstance *inst, const WASMComponentCanonLiftValueInfo *type_info,
    const wasm_val_t *result, wasm_component_value_t *value)
{
    uint8 storage[16];
    uint32 storage_len = 0;

    if (!result || result->kind != type_info->public_kind)
        return set_component_call_error_fmt(
            inst, "component canon lift function result kind %u does not match "
                  "expected kind %u",
            result ? (unsigned)result->kind : UINT_MAX,
            (unsigned)type_info->public_kind);

    switch (type_info->prim_type) {
        case WASM_COMP_PRIMVAL_BOOL:
            if (result->of.i32 != 0 && result->of.i32 != 1)
                return set_component_call_error(
                    inst, "component canon lift function result is not a valid "
                          "bool value");
            storage[0] = (uint8)result->of.i32;
            storage_len = 1;
            break;
        case WASM_COMP_PRIMVAL_S8:
        case WASM_COMP_PRIMVAL_U8:
            storage[0] = (uint8)result->of.i32;
            storage_len = 1;
            break;
        case WASM_COMP_PRIMVAL_S16:
        case WASM_COMP_PRIMVAL_S32:
            storage_len =
                encode_component_signed_leb((int64)result->of.i32, storage);
            break;
        case WASM_COMP_PRIMVAL_U16:
        case WASM_COMP_PRIMVAL_U32:
            storage_len =
                encode_component_unsigned_leb((uint32)result->of.i32, storage);
            break;
        case WASM_COMP_PRIMVAL_S64:
            storage_len = encode_component_signed_leb(result->of.i64, storage);
            break;
        case WASM_COMP_PRIMVAL_U64:
            storage_len =
                encode_component_unsigned_leb((uint64)result->of.i64, storage);
            break;
        case WASM_COMP_PRIMVAL_F32:
            memcpy(storage, &result->of.f32, sizeof(result->of.f32));
            storage_len = sizeof(result->of.f32);
            break;
        case WASM_COMP_PRIMVAL_F64:
            memcpy(storage, &result->of.f64, sizeof(result->of.f64));
            storage_len = sizeof(result->of.f64);
            break;
        case WASM_COMP_PRIMVAL_CHAR:
            if (!is_valid_unicode_scalar((uint32)result->of.i32))
                return set_component_call_error(
                    inst, "component canon lift function result is not a valid "
                          "Unicode scalar value");
            if ((uint32)result->of.i32 <= 0x7F) {
                storage[0] = (uint8)result->of.i32;
                storage_len = 1;
            }
            else if ((uint32)result->of.i32 <= 0x7FF) {
                storage[0] = (uint8)(0xC0 | ((uint32)result->of.i32 >> 6));
                storage[1] =
                    (uint8)(0x80 | ((uint32)result->of.i32 & 0x3F));
                storage_len = 2;
            }
            else if ((uint32)result->of.i32 <= 0xFFFF) {
                storage[0] = (uint8)(0xE0 | ((uint32)result->of.i32 >> 12));
                storage[1] = (uint8)(0x80
                                     | (((uint32)result->of.i32 >> 6) & 0x3F));
                storage[2] =
                    (uint8)(0x80 | ((uint32)result->of.i32 & 0x3F));
                storage_len = 3;
            }
            else {
                storage[0] = (uint8)(0xF0 | ((uint32)result->of.i32 >> 18));
                storage[1] = (uint8)(0x80
                                     | (((uint32)result->of.i32 >> 12) & 0x3F));
                storage[2] = (uint8)(0x80
                                     | (((uint32)result->of.i32 >> 6) & 0x3F));
                storage[3] =
                    (uint8)(0x80 | ((uint32)result->of.i32 & 0x3F));
                storage_len = 4;
            }
            break;
        default:
            return set_component_call_error_fmt(
                inst, "component canon lift function result uses unsupported "
                      "component scalar type %s",
                component_prim_type_name(type_info->prim_type));
    }

    init_component_public_result_type(value, type_info);
    value->storage_kind = WASM_COMPONENT_VALUE_STORAGE_INLINE;
    value->byte_size = storage_len;
    memcpy(value->storage.inline_storage, storage, storage_len);
    return true;
}

static bool
encode_component_public_scalar_value(
    const WASMComponentCanonLiftValueInfo *type_info, const wasm_val_t *input,
    wasm_component_value_t *value)
{
    uint8 storage[16];
    uint32 storage_len = 0;

    memset(value, 0, sizeof(*value));
    if (type_info->declared_as_defined)
        value->type.kind = WASM_COMPONENT_VALUE_TYPE_DEFINED;
    else {
        value->type.kind = WASM_COMPONENT_VALUE_TYPE_PRIMITIVE;
        value->type.type.primitive_type =
            (wasm_component_primitive_value_kind_t)type_info->prim_type;
    }

    switch (type_info->prim_type) {
        case WASM_COMP_PRIMVAL_BOOL:
            storage[0] = (uint8)input->of.i32;
            storage_len = 1;
            break;
        case WASM_COMP_PRIMVAL_S8:
        case WASM_COMP_PRIMVAL_U8:
            storage[0] = (uint8)input->of.i32;
            storage_len = 1;
            break;
        case WASM_COMP_PRIMVAL_S16:
        case WASM_COMP_PRIMVAL_S32:
            storage_len =
                encode_component_signed_leb((int64)input->of.i32, storage);
            break;
        case WASM_COMP_PRIMVAL_U16:
        case WASM_COMP_PRIMVAL_U32:
            storage_len =
                encode_component_unsigned_leb((uint32)input->of.i32, storage);
            break;
        case WASM_COMP_PRIMVAL_S64:
            storage_len = encode_component_signed_leb(input->of.i64, storage);
            break;
        case WASM_COMP_PRIMVAL_U64:
            storage_len =
                encode_component_unsigned_leb((uint64)input->of.i64, storage);
            break;
        case WASM_COMP_PRIMVAL_F32:
            memcpy(storage, &input->of.f32, sizeof(input->of.f32));
            storage_len = sizeof(input->of.f32);
            break;
        case WASM_COMP_PRIMVAL_F64:
            memcpy(storage, &input->of.f64, sizeof(input->of.f64));
            storage_len = sizeof(input->of.f64);
            break;
        case WASM_COMP_PRIMVAL_CHAR:
            if ((uint32)input->of.i32 <= 0x7F) {
                storage[0] = (uint8)input->of.i32;
                storage_len = 1;
            }
            else if ((uint32)input->of.i32 <= 0x7FF) {
                storage[0] = (uint8)(0xC0 | ((uint32)input->of.i32 >> 6));
                storage[1] =
                    (uint8)(0x80 | ((uint32)input->of.i32 & 0x3F));
                storage_len = 2;
            }
            else if ((uint32)input->of.i32 <= 0xFFFF) {
                storage[0] = (uint8)(0xE0 | ((uint32)input->of.i32 >> 12));
                storage[1] = (uint8)(0x80
                                     | (((uint32)input->of.i32 >> 6) & 0x3F));
                storage[2] =
                    (uint8)(0x80 | ((uint32)input->of.i32 & 0x3F));
                storage_len = 3;
            }
            else {
                storage[0] = (uint8)(0xF0 | ((uint32)input->of.i32 >> 18));
                storage[1] = (uint8)(0x80
                                     | (((uint32)input->of.i32 >> 12) & 0x3F));
                storage[2] = (uint8)(0x80
                                     | (((uint32)input->of.i32 >> 6) & 0x3F));
                storage[3] =
                    (uint8)(0x80 | ((uint32)input->of.i32 & 0x3F));
                storage_len = 4;
            }
            break;
        default:
            return false;
    }

    value->storage_kind = WASM_COMPONENT_VALUE_STORAGE_INLINE;
    value->byte_size = storage_len;
    memcpy(value->storage.inline_storage, storage, storage_len);
    return true;
}

static bool
init_component_public_string_result(
    WASMComponentInstance *inst, const WASMComponentCanonLiftValueInfo *type_info,
    const uint8 *payload, uint32 payload_len, wasm_component_value_t *value)
{
    uint8 len_buf[5];
    uint32 len_len = encode_component_unsigned_leb(payload_len, len_buf);
    uint32 total_len;
    uint8 *storage;

    if ((uint64)payload_len + len_len > UINT32_MAX)
        return set_component_call_error(
            inst, "component canon lift function result string is too large");

    total_len = payload_len + len_len;
    storage = wasm_runtime_malloc(total_len);
    if (!storage)
        return set_component_call_error(
            inst, "component canon lift function could not allocate result "
                  "storage");

    memcpy(storage, len_buf, len_len);
    if (payload_len > 0)
        memcpy(storage + len_len, payload, payload_len);

    init_component_public_result_type(value, type_info);
    value->storage_kind = WASM_COMPONENT_VALUE_STORAGE_OWNED;
    value->byte_size = total_len;
    value->storage.owned_data = storage;
    return true;
}

static bool
init_component_public_list_u8_result(
    WASMComponentInstance *inst, const WASMComponentCanonLiftValueInfo *type_info,
    uint8 *payload_copy, uint32 payload_len, wasm_component_value_t *value)
{
    if (payload_len > 0 && !payload_copy)
        return set_component_call_error(
            inst, "component canon lift function result list<u8> is missing "
                  "backing bytes");

    init_component_public_result_type(value, type_info);
    value->byte_size = payload_len;
    if (payload_len == 0)
        return true;

    value->storage_kind = WASM_COMPONENT_VALUE_STORAGE_OWNED;
    value->storage.owned_data = payload_copy;
    return true;
}

static bool
get_component_canon_memory_bytes(WASMComponentInstance *inst,
                                 const WASMComponentRuntimeFunc *function,
                                 uint32 offset, uint32 size,
                                 const char *description, uint8 **bytes_out)
{
    WASMMemoryInstance *memory = function->canon_memory_ref.of.memory;
    uint64 memory_size;

    if (!memory || !memory->memory_data)
        return set_component_call_error(
            inst, "component canon lift function memory is unavailable");

    memory_size = GET_LINEAR_MEMORY_SIZE(memory);
    if ((uint64)offset + size > memory_size)
        return set_component_call_error_fmt(
            inst, "component canon lift function %s is out of bounds", description);

    *bytes_out = memory->memory_data + offset;
    return true;
}

static bool
call_component_canon_realloc_raw(WASMComponentInstance *inst,
                                 const WASMComponentRuntimeFunc *function,
                                 uint32 old_ptr, uint32 old_size, uint32 align,
                                 uint32 new_size, uint32 *ptr_out)
{
    WASMExecEnv *exec_env;
    wasm_val_t args[4] = { 0 };
    wasm_val_t result = { 0 };

    exec_env = wasm_runtime_get_exec_env_singleton(
        (WASMModuleInstanceCommon *)function->canon_realloc_ref.owner_instance
            ->module_inst);
    if (!exec_env)
        return set_component_call_error(
            inst, "component canon lift function could not acquire a realloc "
                  "execution environment");

    args[0].kind = WASM_I32;
    args[1].kind = WASM_I32;
    args[2].kind = WASM_I32;
    args[3].kind = WASM_I32;
    args[0].of.i32 = (int32)old_ptr;
    args[1].of.i32 = (int32)old_size;
    args[2].of.i32 = (int32)align;
    args[3].of.i32 = (int32)new_size;

    if (!wasm_runtime_call_wasm_a(exec_env, function->canon_realloc_ref.of.function,
                                  1, &result, 4, args)) {
        const char *core_exception = wasm_runtime_get_exception(
            (WASMModuleInstanceCommon *)function->canon_realloc_ref.owner_instance
                ->module_inst);
        if (core_exception)
            wasm_runtime_set_exception((WASMModuleInstanceCommon *)inst,
                                       core_exception);
        else
            wasm_runtime_set_exception((WASMModuleInstanceCommon *)inst,
                                       "component canon lift realloc failed");
        return false;
    }

    if (result.kind != WASM_I32)
        return set_component_call_error(
            inst, "component canon lift realloc returned an unexpected result "
                  "kind");

    *ptr_out = (uint32)result.of.i32;
    return true;
}

static bool
call_component_canon_realloc(WASMComponentInstance *inst,
                             const WASMComponentRuntimeFunc *function,
                             uint32 new_size, uint32 *ptr_out)
{
    if (new_size == 0) {
        *ptr_out = 0;
        return true;
    }

    return call_component_canon_realloc_raw(inst, function, 0, 0, 1, new_size,
                                            ptr_out);
}

static void
cleanup_component_canon_param_allocations(
    WASMComponentInstance *inst, const WASMComponentRuntimeFunc *function,
    WASMComponentCanonParamAllocationTracker *allocation_tracker)
{
    char saved_exception[256];
    const char *exception = wasm_runtime_get_exception((WASMModuleInstanceCommon *)inst);
    uint32 i;

    if (!allocation_tracker || !allocation_tracker->allocations
        || allocation_tracker->count == 0)
        return;

    if (exception && exception[0]) {
        snprintf(saved_exception, sizeof(saved_exception), "%s", exception);
    }
    else {
        saved_exception[0] = '\0';
    }

    for (i = allocation_tracker->count; i > 0; i--) {
        WASMComponentCanonParamAllocation *allocation =
            &allocation_tracker->allocations[i - 1];
        uint32 ignored_ptr = 0;

        if (allocation->ptr == 0)
            continue;

        wasm_runtime_clear_exception((WASMModuleInstanceCommon *)inst);
        wasm_runtime_clear_exception(
            (WASMModuleInstanceCommon *)function->canon_realloc_ref.owner_instance
                ->module_inst);
        if (!call_component_canon_realloc_raw(inst, function, allocation->ptr,
                                              allocation->size, 1, 0,
                                              &ignored_ptr)) {
            wasm_runtime_clear_exception((WASMModuleInstanceCommon *)inst);
            wasm_runtime_clear_exception(
                (WASMModuleInstanceCommon *)function->canon_realloc_ref
                    .owner_instance->module_inst);
        }
    }

    if (saved_exception[0])
        wasm_runtime_set_exception((WASMModuleInstanceCommon *)inst,
                                   saved_exception);
    else
        wasm_runtime_clear_exception((WASMModuleInstanceCommon *)inst);

    allocation_tracker->count = 0;
}

static bool
call_component_canon_post_return(WASMComponentInstance *inst,
                                 const WASMComponentRuntimeFunc *function,
                                 uint32 retptr)
{
    WASMExecEnv *exec_env;
    wasm_val_t arg = { 0 };

    if (!function->canon_post_return_ref.of.function)
        return true;

    exec_env = wasm_runtime_get_exec_env_singleton(
        (WASMModuleInstanceCommon *)function->canon_post_return_ref.owner_instance
            ->module_inst);
    if (!exec_env)
        return set_component_call_error(
            inst, "component canon lift function could not acquire a post-return "
                  "execution environment");

    arg.kind = WASM_I32;
    arg.of.i32 = (int32)retptr;
    if (!wasm_runtime_call_wasm_a(exec_env,
                                  function->canon_post_return_ref.of.function, 0,
                                  NULL, 1, &arg)) {
        const char *core_exception = wasm_runtime_get_exception(
            (WASMModuleInstanceCommon *)function->canon_post_return_ref
                .owner_instance->module_inst);
        if (core_exception)
            wasm_runtime_set_exception((WASMModuleInstanceCommon *)inst,
                                       core_exception);
        else
            wasm_runtime_set_exception((WASMModuleInstanceCommon *)inst,
                                       "component canon lift post-return failed");
        return false;
    }

    return true;
}

static bool
wasm_component_call_values_internal(WASMComponentInstance *inst,
                                    const WASMComponentRuntimeFunc *function,
                                    uint32 num_results,
                                    wasm_component_value_t *results,
                                    uint32 num_args,
                                    const wasm_component_value_t *args,
                                    bool require_top_level_export)
{
    WASMComponentFuncType *component_type = NULL;
    WASMFuncType *core_type = NULL;
    WASMExecEnv *exec_env;
    WASMComponentCanonLiftValueShape result_shape;
    WASMComponentCompositeFlatLeaf stack_result_leaves[16];
    WASMComponentCompositeFlatLeaf *result_leaves = stack_result_leaves;
    WASMComponentCanonLiftValueInfo result_info;
    wasm_val_t stack_args[16];
    wasm_val_t *core_args = stack_args;
    wasm_val_t stack_results[16];
    wasm_val_t *core_results = stack_results;
    WASMComponentCanonParamAllocation stack_param_allocations[16];
    WASMComponentCanonParamAllocation *param_allocations = stack_param_allocations;
    WASMComponentCanonParamAllocationTracker param_allocation_tracker;
    uint32 expected_result_count;
    uint32 i, core_arg_index = 0, flat_result_count = 0;
    bool call_succeeded = false;
    bool core_call_attempted = false;
    bool has_composite_result = false;
    bool composite_result_has_string = false;
    bool composite_result_has_list_u8 = false;
    bool composite_result_needs_memory = false;
    bool have_memory_result_ptr = false;
    uint32 memory_result_retptr = 0;

    if (!inst)
        return false;

    if (!function) {
        wasm_runtime_set_exception((WASMModuleInstanceCommon *)inst,
                                   "component function handle is null");
        return false;
    }

    wasm_runtime_clear_exception((WASMModuleInstanceCommon *)inst);

    if (function->kind == WASM_COMP_RUNTIME_FUNC_HOST_IMPORT) {
        wasm_component_value_t callback_results[1];
        wasm_val_t ignored = { 0 };
        char host_error_buf[128] = { 0 };

        if (require_top_level_export && !function->is_top_level_export)
            return set_component_call_error(
                inst, "component call only supports top-level exported host "
                      "component functions");

        if (!resolve_component_func_type(inst, function, "host component function",
                                         &component_type))
            return false;

        expected_result_count = get_component_func_result_count(component_type);
        if (!component_type || !component_type->params)
            return set_component_call_error(
                inst, "host component function is missing parameter metadata");

        if (num_args != component_type->params->count)
            return set_component_call_error_fmt(
                inst,
                "host component function expects %u arguments but received %u",
                component_type->params->count, num_args);
        if (num_results != expected_result_count)
            return set_component_call_error_fmt(
                inst,
                "host component function expects %u results but received %u",
                expected_result_count, num_results);
        memset(callback_results, 0, sizeof(callback_results));

        if (num_args > 0 && !args)
            return set_component_call_error(
                inst, "host component function arguments buffer is null");
        if (num_results > 0 && !results)
            return set_component_call_error(
                inst, "host component function results buffer is null");

        for (i = 0; i < component_type->params->count; i++) {
            WASMComponentCanonLiftValueShape shape;
            WASMComponentCanonLiftValueInfo type_info;

            if (!resolve_component_canon_lift_value_shape(
                    &inst->module->component,
                    component_type->params->params[i].value_type, "parameter", i,
                    &shape, inst))
                return false;

            if (!shape.is_primitive && shape.def_type
                && (shape.def_type->tag == WASM_COMP_DEF_VAL_RECORD
                    || shape.def_type->tag == WASM_COMP_DEF_VAL_TUPLE)) {
                if (!validate_component_public_composite_param_value(
                        inst, &inst->module->component,
                        component_type->params->params[i].value_type, &args[i], i))
                    return false;
                continue;
            }

            if (!lookup_component_canon_lift_value_type(
                    &inst->module->component,
                    component_type->params->params[i].value_type, "parameter", i,
                    true, true, false, &type_info, inst))
                return false;

            if (type_info.kind == WASM_COMP_CANON_LIFT_VALUE_SCALAR) {
                if (!decode_component_public_scalar_value(inst, &args[i], &type_info,
                                                          "parameter", i,
                                                          &ignored))
                    return false;
            }
            else {
                const uint8 *payload;
                uint32 payload_len;

                if ((type_info.kind == WASM_COMP_CANON_LIFT_VALUE_STRING
                     && !decode_component_public_string_value(
                         inst, &args[i], &type_info, "parameter", i, &payload,
                         &payload_len))
                    || (type_info.kind == WASM_COMP_CANON_LIFT_VALUE_LIST_U8
                        && !decode_component_public_list_u8_value(
                            inst, &args[i], &type_info, "parameter", i,
                            &payload, &payload_len)))
                    return false;
            }
        }

        if (!function->host_callback)
            return set_component_call_error(
                inst, "host component function callback is missing");

        if (!function->host_callback((WASMModuleInstanceCommon *)inst,
                                     function->host_user_data,
                                     expected_result_count, callback_results,
                                     num_args, args, host_error_buf,
                                     (uint32)sizeof(host_error_buf))) {
            wasm_component_value_destroy(&callback_results[0]);
            return set_component_call_error_from_host_result(inst, host_error_buf);
        }

        if (expected_result_count == 1) {
            if (function->has_composite_result) {
                if (!validate_host_component_public_composite_result_value(
                        inst, &inst->module->component,
                        component_type->results->results, &callback_results[0],
                        0)) {
                    wasm_component_value_destroy(&callback_results[0]);
                    return false;
                }
            }
            else {
                if (!lookup_component_canon_lift_value_type(
                        &inst->module->component, component_type->results->results,
                        "result", 0, true, false, true, &result_info, inst)) {
                    wasm_component_value_destroy(&callback_results[0]);
                    return false;
                }

                if (result_info.kind == WASM_COMP_CANON_LIFT_VALUE_SCALAR) {
                    if (!decode_component_public_scalar_value(
                            inst, &callback_results[0], &result_info, "result", 0,
                            &ignored)) {
                        wasm_component_value_destroy(&callback_results[0]);
                        return false;
                    }
                }
                else {
                    const uint8 *payload;
                    uint32 payload_len;

                    if ((result_info.kind == WASM_COMP_CANON_LIFT_VALUE_STRING
                         && !decode_component_public_string_value(
                             inst, &callback_results[0], &result_info, "result",
                             0, &payload, &payload_len))
                        || (result_info.kind == WASM_COMP_CANON_LIFT_VALUE_LIST_U8
                            && !decode_component_public_list_u8_value(
                                inst, &callback_results[0], &result_info, "result",
                                0, &payload, &payload_len))) {
                        wasm_component_value_destroy(&callback_results[0]);
                        return false;
                    }
                }
            }

            wasm_component_value_destroy(&results[0]);
            results[0] = callback_results[0];
            memset(&callback_results[0], 0, sizeof(callback_results[0]));
        }

        return true;
    }

    memset(&param_allocation_tracker, 0, sizeof(param_allocation_tracker));
    param_allocation_tracker.allocations = param_allocations;

    if (function->kind != WASM_COMP_RUNTIME_FUNC_LIFT)
        return set_component_call_error(
            inst, "component call only supports canon lift functions");

    if (require_top_level_export && !function->is_top_level_export)
        return set_component_call_error(
            inst, "component call only supports top-level exported canon lift "
                  "functions");

    if (component_canon_lift_uses_lowered_core_func(function)) {
        if (function->core_func_ref.of.lowered_function->lowered_target == function)
            return set_component_call_error(
                inst, "component canon lift function forms an unsupported "
                      "lowered recursion cycle");
        return wasm_component_call_values_internal(
            inst, function->core_func_ref.of.lowered_function->lowered_target,
            num_results, results, num_args, args, false);
    }

    if (function->core_func_ref.type != WASM_COMP_CORE_RUNTIME_REF_FUNC
        || !function->core_func_ref.of.function)
        return set_component_call_error(
            inst, "component canon lift function is not bound to a core function");

    if (!resolve_component_canon_lift_type(inst, function, &component_type,
                                           &core_type))
        return false;

    if (!component_type || !component_type->params)
        return set_component_call_error(
            inst, "component canon lift function is missing parameter metadata");

    expected_result_count = component_type->results
                                    && component_type->results->tag
                                           == WASM_COMP_RESULT_LIST_WITH_TYPE
                                    && component_type->results->results
                                ? 1
                                : 0;

    if (num_args != component_type->params->count)
        return set_component_call_error_fmt(
            inst,
            "component canon lift function expects %u arguments but received %u",
            component_type->params->count, num_args);

    if (num_results != expected_result_count)
        return set_component_call_error_fmt(
            inst,
            "component canon lift function expects %u results but received %u",
            expected_result_count, num_results);

    if (num_args > 0 && !args)
        return set_component_call_error(
            inst, "component canon lift function arguments buffer is null");

    if (num_results > 0 && !results)
        return set_component_call_error(
            inst, "component canon lift function results buffer is null");

    if (expected_result_count == 1) {
        if (!resolve_component_canon_lift_value_shape(
                &inst->module->component, component_type->results->results, "result",
                0, &result_shape, inst))
            return false;

        has_composite_result =
            !result_shape.is_primitive && result_shape.def_type
            && (result_shape.def_type->tag == WASM_COMP_DEF_VAL_RECORD
                || result_shape.def_type->tag == WASM_COMP_DEF_VAL_TUPLE);

        if (has_composite_result) {
            uint32 composite_result_size = 0, composite_result_align = 1;

            if (!compute_component_canon_abi_layout(
                    inst, &inst->module->component, component_type->results->results,
                    0, &composite_result_size, &composite_result_align,
                    &composite_result_has_string,
                    &composite_result_has_list_u8))
                goto cleanup;
            (void)composite_result_size;
            (void)composite_result_align;

            composite_result_needs_memory =
                composite_result_has_string || composite_result_has_list_u8;
            if (composite_result_needs_memory) {
                if (core_type->result_count != 1
                    || core_type->types[core_type->param_count] != VALUE_TYPE_I32) {
                    set_component_call_error(
                        inst, "component canon lift function only supports a "
                              "single memory-backed result returned through "
                              "memory");
                    goto cleanup;
                }
            }
            else {
                if (core_type->result_count
                    > sizeof(stack_result_leaves) / sizeof(stack_result_leaves[0])) {
                    result_leaves = wasm_runtime_malloc(
                        sizeof(WASMComponentCompositeFlatLeaf)
                        * core_type->result_count);
                    if (!result_leaves)
                        return set_component_call_error(
                            inst, "component canon lift function could not allocate "
                                  "result flattening metadata");
                }

                if (!validate_component_composite_result_signature(
                        inst, &inst->module->component,
                        component_type->results->results, 0, core_type,
                        result_leaves, core_type->result_count,
                        &flat_result_count))
                    goto cleanup;
            }
        }
        else {
            if (!lookup_component_canon_lift_value_type(
                    &inst->module->component, component_type->results->results,
                    "result", 0, true, false, true, &result_info, inst))
                goto cleanup;

            if (result_info.kind == WASM_COMP_CANON_LIFT_VALUE_STRING
                || result_info.kind == WASM_COMP_CANON_LIFT_VALUE_LIST_U8) {
                if (core_type->result_count != 1
                    || core_type->types[core_type->param_count] != VALUE_TYPE_I32) {
                    set_component_call_error(
                        inst, "component canon lift function only supports a "
                              "single memory-backed result returned through "
                              "memory");
                    goto cleanup;
                }
            }
            else if (core_type->result_count != 1
                     || core_type->types[core_type->param_count]
                            != result_info.core_type) {
                set_component_call_error(
                    inst, "component canon lift function result does not match the "
                          "core function signature");
                goto cleanup;
            }
        }
    }
    else if (core_type->result_count != 0)
        return set_component_call_error(
            inst, "component canon lift function only supports at most one "
                  "result");

    if ((composite_result_has_string || function->has_string_result)
        && function->string_encoding != WASM_COMP_RUNTIME_STRING_ENCODING_UTF8)
        return set_component_call_error(
            inst, "component canon lift function only supports UTF-8 string "
                  "encoding");

    if ((function->has_string_params || function->has_list_u8_params
         || function->memory_result_kind
                != WASM_COMP_RUNTIME_CANON_LIFT_MEMORY_RESULT_NONE
         || composite_result_needs_memory)
        && function->canon_memory_ref.of.memory
        && function->canon_memory_ref.of.memory->is_memory64)
        return set_component_call_error(
            inst, function->has_string_params || composite_result_has_string
                          || function->has_string_result
                       ? "component canon lift function does not support memory64 "
                         "string Canonical ABI"
                       : function->has_list_u8_params
                                 || composite_result_has_list_u8
                                 || function->has_list_u8_result
                             ? "component canon lift function does not support "
                               "memory64 list<u8> Canonical ABI"
                             : "component canon lift function does not support "
                               "memory64 Canonical ABI");

    if (core_type->param_count > sizeof(stack_args) / sizeof(stack_args[0])) {
        core_args = wasm_runtime_malloc(sizeof(wasm_val_t) * core_type->param_count);
        if (!core_args)
            return set_component_call_error(
                inst, "component canon lift function could not allocate argument "
                      "storage");
    }
    memset(core_args, 0, sizeof(wasm_val_t) * core_type->param_count);
    if (core_type->param_count
        > sizeof(stack_param_allocations) / sizeof(stack_param_allocations[0])) {
        param_allocations = wasm_runtime_malloc(sizeof(*param_allocations)
                                                * core_type->param_count);
        if (!param_allocations) {
            set_component_call_error(
                inst, "component canon lift function could not allocate parameter "
                      "allocation tracking");
            goto cleanup;
        }
    }
    memset(param_allocations, 0,
           sizeof(*param_allocations) * core_type->param_count);
    param_allocation_tracker.allocations = param_allocations;
    param_allocation_tracker.count = 0;
    param_allocation_tracker.capacity = core_type->param_count;
    if (core_type->result_count
        > sizeof(stack_results) / sizeof(stack_results[0])) {
        core_results =
            wasm_runtime_malloc(sizeof(wasm_val_t) * core_type->result_count);
        if (!core_results) {
            set_component_call_error(
                inst, "component canon lift function could not allocate result "
                      "storage");
            goto cleanup;
        }
    }
    memset(core_results, 0, sizeof(wasm_val_t) * core_type->result_count);

    for (i = 0; i < component_type->params->count; i++) {
        if (!flatten_component_public_param_value(
                inst, function, &inst->module->component,
                component_type->params->params[i].value_type, &args[i], i,
                core_type, core_args, &core_arg_index,
                &param_allocation_tracker)) {
            call_succeeded = false;
            goto cleanup;
        }
    }

    if (core_arg_index != core_type->param_count) {
        set_component_param_flattening_error(inst);
        call_succeeded = false;
        goto cleanup;
    }

    exec_env = wasm_runtime_get_exec_env_singleton(
        (WASMModuleInstanceCommon *)function->core_func_ref.owner_instance->module_inst);
    if (!exec_env) {
        set_component_call_error(
            inst, "component canon lift function could not acquire a core "
                  "execution environment");
        call_succeeded = false;
        goto cleanup;
    }

    wasm_runtime_clear_exception(
        (WASMModuleInstanceCommon *)function->core_func_ref.owner_instance->module_inst);
    core_call_attempted = true;
    if (!wasm_runtime_call_wasm_a(exec_env, function->core_func_ref.of.function,
                                  core_type->result_count,
                                  core_type->result_count ? core_results : NULL,
                                  core_type->param_count, core_args)) {
        const char *core_exception = wasm_runtime_get_exception(
            (WASMModuleInstanceCommon *)function->core_func_ref.owner_instance
                ->module_inst);
        if (core_exception)
            wasm_runtime_set_exception((WASMModuleInstanceCommon *)inst,
                                       core_exception);
        else
            wasm_runtime_set_exception((WASMModuleInstanceCommon *)inst,
                                       "component canon lift call failed");
        call_succeeded = false;
        goto cleanup;
    }

    if (expected_result_count == 1) {
        if (has_composite_result) {
            if (composite_result_needs_memory) {
                if (core_results[0].kind != WASM_I32) {
                    set_component_call_error(
                        inst, "component canon lift function composite result "
                              "returned an unexpected result kind");
                    call_succeeded = false;
                    goto cleanup;
                }

                memory_result_retptr = (uint32)core_results[0].of.i32;
                have_memory_result_ptr = true;
                if (!init_component_public_memory_composite_result(
                        inst, function, &inst->module->component,
                        component_type->results->results, 0, memory_result_retptr,
                        &results[0])) {
                    call_succeeded = false;
                    goto cleanup;
                }
            }
            else if (!init_component_public_composite_result(
                         inst, result_leaves, flat_result_count, core_results,
                         &results[0])) {
                call_succeeded = false;
                goto cleanup;
            }
        }
        else if (result_info.kind == WASM_COMP_CANON_LIFT_VALUE_SCALAR) {
            if (!validate_component_scalar_value(inst, &core_results[0],
                                                 result_info.public_kind,
                                                 result_info.prim_type, "result",
                                                 0)
                || !init_component_public_scalar_result(inst, &result_info,
                                                        &core_results[0],
                                                        &results[0])) {
                call_succeeded = false;
                goto cleanup;
            }
        }
        else {
            uint8 *ret_area_bytes = NULL;
            uint8 *payload_bytes = NULL;
            uint32 payload_ptr = 0, payload_len = 0;
            uint8 *payload_copy = NULL;
            const char *result_desc =
                result_info.kind == WASM_COMP_CANON_LIFT_VALUE_STRING
                    ? "string"
                    : "list<u8>";

            if (core_results[0].kind != WASM_I32) {
                set_component_call_error(
                    inst,
                    result_info.kind == WASM_COMP_CANON_LIFT_VALUE_STRING
                        ? "component canon lift function string result returned "
                          "an unexpected result kind"
                        : "component canon lift function list<u8> result "
                          "returned an unexpected result kind");
                call_succeeded = false;
                goto cleanup;
            }

            memory_result_retptr = (uint32)core_results[0].of.i32;
            have_memory_result_ptr = true;
            if (!get_component_canon_memory_bytes(
                    inst, function, memory_result_retptr, 8,
                    result_info.kind == WASM_COMP_CANON_LIFT_VALUE_STRING
                        ? "string result area"
                        : "list<u8> result area",
                                                  &ret_area_bytes)) {
                call_succeeded = false;
                goto cleanup;
            }

            memcpy(&payload_ptr, ret_area_bytes, sizeof(payload_ptr));
            memcpy(&payload_len, ret_area_bytes + sizeof(payload_ptr),
                   sizeof(payload_len));

            if (payload_len > 0) {
                if (!get_component_canon_memory_bytes(
                        inst, function, payload_ptr, payload_len,
                        result_info.kind == WASM_COMP_CANON_LIFT_VALUE_STRING
                            ? "string result payload"
                            : "list<u8> result payload",
                                                      &payload_bytes)) {
                    call_succeeded = false;
                    goto cleanup;
                }
                if (result_info.kind == WASM_COMP_CANON_LIFT_VALUE_STRING
                    && !wasm_component_validate_utf8(payload_bytes, payload_len)) {
                    set_component_call_error(
                        inst, "component canon lift function result does not "
                              "contain valid UTF-8");
                    call_succeeded = false;
                    goto cleanup;
                }

                if (!copy_component_memory_payload(inst, payload_bytes, payload_len,
                                                   result_desc, &payload_copy)) {
                    call_succeeded = false;
                    goto cleanup;
                }
            }

            if (result_info.kind == WASM_COMP_CANON_LIFT_VALUE_STRING) {
                if (!init_component_public_string_result(
                        inst, &result_info, payload_copy, payload_len,
                        &results[0])) {
                    if (payload_copy)
                        wasm_runtime_free(payload_copy);
                    call_succeeded = false;
                    goto cleanup;
                }
                if (payload_copy)
                    wasm_runtime_free(payload_copy);
            }
            else if (!init_component_public_list_u8_result(
                         inst, &result_info, payload_copy, payload_len,
                         &results[0])) {
                if (payload_copy)
                    wasm_runtime_free(payload_copy);
                call_succeeded = false;
                goto cleanup;
            }
        }
    }

    call_succeeded = true;

cleanup:
    if (!core_call_attempted)
        cleanup_component_canon_param_allocations(inst, function,
                                                  &param_allocation_tracker);
    if (have_memory_result_ptr) {
        bool preserve_exception = !call_succeeded;
        char saved_exception[256];
        bool post_return_ok;

        saved_exception[0] = '\0';
        if (preserve_exception) {
            const char *exception =
                wasm_runtime_get_exception((WASMModuleInstanceCommon *)inst);
            if (exception && exception[0])
                snprintf(saved_exception, sizeof(saved_exception), "%s", exception);
        }

        post_return_ok =
            call_component_canon_post_return(inst, function, memory_result_retptr);
        if (call_succeeded && !post_return_ok)
            call_succeeded = false;
        else if (preserve_exception && saved_exception[0])
            wasm_runtime_set_exception((WASMModuleInstanceCommon *)inst,
                                       saved_exception);
    }

    if (core_args != stack_args)
        wasm_runtime_free(core_args);
    if (core_results != stack_results)
        wasm_runtime_free(core_results);
    if (param_allocations != stack_param_allocations)
        wasm_runtime_free(param_allocations);
    if (result_leaves != stack_result_leaves)
        wasm_runtime_free(result_leaves);
    return call_succeeded;
}

static bool
wasm_component_call_internal(WASMComponentInstance *inst,
                             const WASMComponentRuntimeFunc *function,
                             uint32 num_results, wasm_val_t *results,
                             uint32 num_args, wasm_val_t *args,
                             bool require_top_level_export)
{
    WASMComponentFuncType *component_type = NULL;
    WASMFuncType *core_type = NULL;
    WASMExecEnv *exec_env;
    uint32 i;
    uint32 expected_result_count;

    if (!inst)
        return false;

    if (!function) {
        wasm_runtime_set_exception((WASMModuleInstanceCommon *)inst,
                                   "component function handle is null");
        return false;
    }

    wasm_runtime_clear_exception((WASMModuleInstanceCommon *)inst);

    if (function->kind == WASM_COMP_RUNTIME_FUNC_HOST_IMPORT) {
        wasm_component_value_t stack_public_args[16];
        wasm_component_value_t *public_args = stack_public_args;
        wasm_component_value_t public_result = { 0 };

        if (require_top_level_export && !function->is_top_level_export)
            return set_component_call_error(
                inst, "component call only supports top-level exported host "
                      "component functions");

        if (!resolve_component_func_type(inst, function, "host component function",
                                         &component_type))
            return false;

        if (function->has_string_params || function->has_string_result
            || function->has_list_u8_params || function->has_list_u8_result
            || function->has_composite_params || function->has_composite_result)
            return set_component_call_error(
                inst,
                function->has_string_params || function->has_string_result
                    ? "host component function uses string values; call through "
                      "the component value API"
                    : function->has_list_u8_params || function->has_list_u8_result
                          ? "host component function uses memory-backed values; "
                            "call through the component value API"
                          : "host component function uses composite values; call "
                            "through the component value API");

        if (!component_type || !component_type->params)
            return set_component_call_error(
                inst, "host component function is missing parameter metadata");

        expected_result_count = get_component_func_result_count(component_type);
        if (num_args != component_type->params->count)
            return set_component_call_error_fmt(
                inst,
                "host component function expects %u arguments but received %u",
                component_type->params->count, num_args);
        if (num_results != expected_result_count)
            return set_component_call_error_fmt(
                inst,
                "host component function expects %u results but received %u",
                expected_result_count, num_results);
        if (num_args > 0 && !args)
            return set_component_call_error(
                inst, "host component function arguments buffer is null");
        if (num_results > 0 && !results)
            return set_component_call_error(
                inst, "host component function results buffer is null");

        if (num_args > sizeof(stack_public_args) / sizeof(stack_public_args[0])) {
            public_args =
                wasm_runtime_malloc(sizeof(wasm_component_value_t) * num_args);
            if (!public_args)
                return set_component_call_error(
                    inst, "host component function could not allocate argument "
                          "storage");
        }
        memset(public_args, 0, sizeof(wasm_component_value_t) * num_args);

        for (i = 0; i < num_args; i++) {
            WASMComponentCanonLiftValueInfo type_info;

            if (!lookup_component_canon_lift_value_type(
                    &inst->module->component,
                    component_type->params->params[i].value_type, "parameter", i,
                    false, false, false, &type_info, inst)
                || !validate_component_scalar_value(inst, &args[i],
                                                    type_info.public_kind,
                                                    type_info.prim_type,
                                                    "parameter", i)
                || !encode_component_public_scalar_value(&type_info, &args[i],
                                                         &public_args[i])) {
                for (uint32 j = 0; j < num_args; j++)
                    wasm_component_value_destroy(&public_args[j]);
                if (public_args != stack_public_args)
                    wasm_runtime_free(public_args);
                return false;
            }
        }

        if (!wasm_component_call_values_internal(inst, function, expected_result_count,
                                                 expected_result_count
                                                     ? &public_result
                                                     : NULL,
                                                 num_args, public_args, false)) {
            for (uint32 j = 0; j < num_args; j++)
                wasm_component_value_destroy(&public_args[j]);
            if (public_args != stack_public_args)
                wasm_runtime_free(public_args);
            return false;
        }

        for (uint32 j = 0; j < num_args; j++)
            wasm_component_value_destroy(&public_args[j]);
        if (public_args != stack_public_args)
            wasm_runtime_free(public_args);

        if (expected_result_count == 1) {
            WASMComponentCanonLiftValueInfo result_info_local;

            if (!lookup_component_canon_lift_value_type(
                    &inst->module->component, component_type->results->results,
                    "result", 0, false, false, false, &result_info_local, inst)
                || !decode_component_public_scalar_value(
                    inst, &public_result, &result_info_local, "result", 0,
                    &results[0])) {
                wasm_component_value_destroy(&public_result);
                return false;
            }
        }

        wasm_component_value_destroy(&public_result);
        return true;
    }

    if (function->kind != WASM_COMP_RUNTIME_FUNC_LIFT)
        return set_component_call_error(
            inst, "component call only supports canon lift functions");

    if (require_top_level_export && !function->is_top_level_export)
        return set_component_call_error(
            inst, "component call only supports top-level exported canon lift "
                  "functions");

    if (component_canon_lift_uses_lowered_core_func(function)) {
        if (function->core_func_ref.of.lowered_function->lowered_target == function)
            return set_component_call_error(
                inst, "component canon lift function forms an unsupported "
                      "lowered recursion cycle");
        return wasm_component_call_internal(
            inst, function->core_func_ref.of.lowered_function->lowered_target,
            num_results, results, num_args, args, false);
    }

    if (function->core_func_ref.type != WASM_COMP_CORE_RUNTIME_REF_FUNC
        || !function->core_func_ref.of.function)
        return set_component_call_error(
            inst, "component canon lift function is not bound to a core function");

    if (function->has_string_params || function->has_list_u8_params
        || function->memory_result_kind
               != WASM_COMP_RUNTIME_CANON_LIFT_MEMORY_RESULT_NONE)
        return set_component_call_error(
            inst, function->has_string_params || function->has_string_result
                       ? "component canon lift function uses string values; call "
                          "through the component value API"
                        : function->has_list_u8_params
                                  || function->has_list_u8_result
                              ? "component canon lift function uses memory-backed "
                                "values; call through the component value API"
                              : "component canon lift function uses memory-backed "
                                "values; call through the component value API");

    if (!resolve_component_canon_lift_type(inst, function, &component_type,
                                           &core_type))
        return false;

    if (!component_type || !component_type->params)
        return set_component_call_error(
            inst, "component canon lift function is missing parameter metadata");

    expected_result_count = component_type->results
                                    && component_type->results->tag
                                           == WASM_COMP_RESULT_LIST_WITH_TYPE
                                    && component_type->results->results
                                ? 1
                                : 0;

    if (core_type->param_count != component_type->params->count)
        return set_component_call_error(
            inst, "component canon lift function uses unsupported scalar "
                  "flattening for parameters");

    if (core_type->result_count != expected_result_count || core_type->result_count > 1)
        return set_component_call_error(
            inst, "component canon lift function only supports scalar signatures "
                  "with at most one result");

    if (num_args != component_type->params->count)
        return set_component_call_error_fmt(
            inst,
            "component canon lift function expects %u arguments but received %u",
            component_type->params->count, num_args);

    if (num_results != expected_result_count)
        return set_component_call_error_fmt(
            inst,
            "component canon lift function expects %u results but received %u",
            expected_result_count, num_results);

    if (num_args > 0 && !args)
        return set_component_call_error(
            inst, "component canon lift function arguments buffer is null");

    if (num_results > 0 && !results)
        return set_component_call_error(
            inst, "component canon lift function results buffer is null");

    for (i = 0; i < component_type->params->count; i++) {
        uint8 prim_type, expected_core_type;
        wasm_valkind_t expected_kind;

        if (!lookup_component_scalar_type(
                &inst->module->component, component_type->params->params[i].value_type,
                "parameter", i, &prim_type, &expected_core_type, &expected_kind,
                inst))
            return false;

        if (core_type->types[i] != expected_core_type)
            return set_component_call_error_fmt(
                inst, "component canon lift function parameter %u does not match "
                      "the core function signature",
                i);

        if (!validate_component_scalar_value(inst, &args[i], expected_kind, prim_type,
                                             "parameter", i))
            return false;
    }

    if (expected_result_count == 1) {
        uint8 prim_type, expected_core_type;
        wasm_valkind_t expected_kind;

        if (!lookup_component_scalar_type(&inst->module->component,
                                          component_type->results->results, "result",
                                          0, &prim_type, &expected_core_type,
                                          &expected_kind, inst))
            return false;

        if (core_type->types[core_type->param_count] != expected_core_type)
            return set_component_call_error(
                inst, "component canon lift function result does not match the "
                      "core function signature");
    }

    exec_env = wasm_runtime_get_exec_env_singleton(
        (WASMModuleInstanceCommon *)function->core_func_ref.owner_instance->module_inst);
    if (!exec_env)
        return set_component_call_error(
            inst, "component canon lift function could not acquire a core "
                  "execution environment");

    wasm_runtime_clear_exception(
        (WASMModuleInstanceCommon *)function->core_func_ref.owner_instance->module_inst);
    if (!wasm_runtime_call_wasm_a(exec_env, function->core_func_ref.of.function,
                                  num_results, results, num_args, args)) {
        const char *core_exception = wasm_runtime_get_exception(
            (WASMModuleInstanceCommon *)function->core_func_ref.owner_instance->module_inst);
        if (core_exception)
            wasm_runtime_set_exception((WASMModuleInstanceCommon *)inst,
                                       core_exception);
        else
            wasm_runtime_set_exception((WASMModuleInstanceCommon *)inst,
                                       "component canon lift call failed");
        return false;
    }

    if (expected_result_count == 1) {
        uint8 prim_type, ignored_core_type;
        wasm_valkind_t expected_kind;

        if (!lookup_component_scalar_type(&inst->module->component,
                                          component_type->results->results, "result",
                                          0, &prim_type, &ignored_core_type,
                                          &expected_kind, inst))
            return false;
        (void)ignored_core_type;

        if (!validate_component_scalar_value(inst, &results[0], expected_kind,
                                             prim_type, "result", 0))
            return false;
    }

    return true;
}

bool
wasm_component_call(WASMComponentInstance *inst,
                    const WASMComponentRuntimeFunc *function,
                    uint32 num_results, wasm_val_t *results,
                    uint32 num_args, wasm_val_t *args)
{
    return wasm_component_call_internal(inst, function, num_results, results,
                                        num_args, args, false);
}

bool
wasm_component_call_values(WASMComponentInstance *inst,
                           const WASMComponentRuntimeFunc *function,
                           uint32 num_results,
                           wasm_component_value_t *results,
                           uint32 num_args,
                           const wasm_component_value_t *args)
{
    return wasm_component_call_values_internal(inst, function, num_results,
                                               results, num_args, args, false);
}

static bool
set_component_start_error_from_exception(WASMComponentInstance *inst,
                                         char *error_buf, uint32 error_buf_size,
                                         const char *prefix)
{
    const char *exception =
        wasm_runtime_get_exception((WASMModuleInstanceCommon *)inst);
    static const char canon_lift_prefix[] = "component canon lift function ";
    static const char instantiate_prefix[] =
        "WASM component instantiate failed: ";

    if (!exception || !exception[0]) {
        if (error_buf && error_buf[0]) {
            exception = error_buf;
            if (!strncmp(exception, instantiate_prefix,
                         sizeof(instantiate_prefix) - 1))
                exception += sizeof(instantiate_prefix) - 1;
        }
        else
            exception = "component start call failed";
    }

    if (!strncmp(exception, canon_lift_prefix, sizeof(canon_lift_prefix) - 1))
        exception += sizeof(canon_lift_prefix) - 1;

    return set_component_runtime_error_fmt(
        error_buf, error_buf_size, "%s: %s", prefix, exception);
}

static bool
resolve_component_start_function_type(
    WASMComponentInstance *inst, const WASMComponentRuntimeFunc *function,
    WASMComponentFuncType **out_component_type)
{
    WASMFuncType *ignored_core_type = NULL;

    if (function->kind == WASM_COMP_RUNTIME_FUNC_HOST_IMPORT)
        return resolve_component_func_type(inst, function, "host component function",
                                           out_component_type);

    if (function->kind != WASM_COMP_RUNTIME_FUNC_LIFT)
        return set_component_call_error(
            inst, "component start section only supports canon lift and host "
                  "component functions");

    return resolve_component_canon_lift_type(inst, function, out_component_type,
                                             &ignored_core_type);
}

static void
destroy_component_public_values(wasm_component_value_t *values, uint32 count)
{
    uint32 i;

    if (!values)
        return;

    for (i = 0; i < count; i++)
        wasm_component_value_destroy(&values[i]);

    wasm_runtime_free(values);
}

static uint32
encode_component_unsigned_leb(uint64 value, uint8 *out_buf)
{
    uint32 len = 0;

    do {
        uint8 byte = (uint8)(value & 0x7F);
        value >>= 7;
        if (value != 0)
            byte |= 0x80;
        out_buf[len++] = byte;
    } while (value != 0);

    return len;
}

static uint32
encode_component_signed_leb(int64 value, uint8 *out_buf)
{
    uint32 len = 0;
    bool done = false;

    while (!done) {
        uint8 byte = (uint8)(value & 0x7F);
        bool sign_bit_set = (byte & 0x40) != 0;

        value >>= 7;
        done = (value == 0 && !sign_bit_set) || (value == -1 && sign_bit_set);
        if (!done)
            byte |= 0x80;

        out_buf[len++] = byte;
    }

    return len;
}

static bool
execute_component_start_section_with_public_values(
    WASMComponentInstance *inst, const WASMComponentRuntimeFunc *function,
    const WASMComponentStartSection *start_section,
    WASMComponentRuntimeValue *result_value, char *error_buf,
    uint32 error_buf_size, const char *error_prefix, uint32 num_args,
    wasm_component_value_t *public_args)
{
    WASMComponentFuncType *component_type = NULL;
    wasm_component_value_t public_result = { 0 };

    if (!wasm_component_call_values_internal(inst, function, start_section->result,
                                             start_section->result ? &public_result
                                                                   : NULL,
                                             num_args, public_args, false)) {
        wasm_component_value_destroy(&public_result);
        return set_component_start_error_from_exception(inst, error_buf,
                                                        error_buf_size,
                                                        error_prefix);
    }

    if (start_section->result > 0) {
        if (!resolve_component_start_function_type(inst, function, &component_type)) {
            wasm_component_value_destroy(&public_result);
            return set_component_start_error_from_exception(inst, error_buf,
                                                            error_buf_size,
                                                            error_prefix);
        }

        if (!component_type || !component_type->results
            || component_type->results->tag != WASM_COMP_RESULT_LIST_WITH_TYPE
            || !component_type->results->results) {
            wasm_component_value_destroy(&public_result);
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size, "%s result metadata is missing",
                error_prefix);
        }

        wasm_component_runtime_value_clear(result_value);
        if (!wasm_component_runtime_value_init_public(
                result_value, &inst->module->component,
                component_type->results->results, &public_result, error_buf,
                error_buf_size)) {
            wasm_component_runtime_value_clear(result_value);
            wasm_component_value_destroy(&public_result);
            return false;
        }
    }

    wasm_component_value_destroy(&public_result);
    return true;
}

static bool
instantiate_component_start_section(WASMComponentInstance *inst,
                                    const WASMComponentStartSection *start_section,
                                    char *error_buf, uint32 error_buf_size)
{
    const WASMComponentRuntimeFunc *function;
    wasm_component_value_t *public_args = NULL;
    uint32 i;

    if (!start_section)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component start section payload is missing");

    if (start_section->func_idx >= inst->component_func_count)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component start section function index %u is out of bounds",
            start_section->func_idx);

    function = &inst->component_funcs[start_section->func_idx];
    if (start_section->value_args_count > 0) {
        public_args = wasm_runtime_malloc(sizeof(wasm_component_value_t)
                                          * start_section->value_args_count);
        if (!public_args)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "allocate memory failed for component start arguments");
        memset(public_args, 0,
               sizeof(wasm_component_value_t) * start_section->value_args_count);
    }

    for (i = 0; i < start_section->value_args_count; i++) {
        uint32 value_idx = start_section->value_args[i];

        if (value_idx >= inst->component_value_count) {
            destroy_component_public_values(public_args,
                                            start_section->value_args_count);
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "component start section value index %u is out of bounds",
                value_idx);
        }

        if (!wasm_component_public_value_copy(&public_args[i],
                                              &inst->component_values[value_idx],
                                              error_buf, error_buf_size)) {
            destroy_component_public_values(public_args,
                                            start_section->value_args_count);
            return false;
        }
    }

    if (!execute_component_start_section_with_public_values(
            inst, function, start_section,
            start_section->result
                ? &inst->component_values[inst->component_value_count]
                : NULL,
            error_buf, error_buf_size, "component start section failed",
            start_section->value_args_count, public_args)) {
        if (start_section->result > 0)
            wasm_component_runtime_value_clear(
                &inst->component_values[inst->component_value_count]);
        destroy_component_public_values(public_args,
                                        start_section->value_args_count);
        return false;
    }

    if (start_section->result > 0)
        inst->component_value_count++;

    destroy_component_public_values(public_args, start_section->value_args_count);
    return true;
}

static bool
instantiate_nested_component_start_section(
    WASMComponentInstance *inst, WASMComponentRuntimeInstance *runtime_inst,
    WASMNestedComponentLocalBindings *bindings,
    const WASMComponentStartSection *start_section, char *error_buf,
    uint32 error_buf_size)
{
    const WASMComponentRuntimeFunc *function;
    wasm_component_value_t *public_args = NULL;
    uint32 i;

    if (!start_section)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "nested component start section payload is missing");

    if (start_section->func_idx >= bindings->func_count)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "nested component start section function index %u is out of bounds",
            start_section->func_idx);

    if (bindings->funcs[start_section->func_idx].type != WASM_COMP_RUNTIME_REF_FUNC
        || !bindings->funcs[start_section->func_idx].of.function)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "nested component start section function index %u does not resolve "
            "to a function",
            start_section->func_idx);

    function = bindings->funcs[start_section->func_idx].of.function;
    if (start_section->value_args_count > 0) {
        public_args = wasm_runtime_malloc(sizeof(wasm_component_value_t)
                                          * start_section->value_args_count);
        if (!public_args)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "allocate memory failed for nested component start arguments");
        memset(public_args, 0,
               sizeof(wasm_component_value_t) * start_section->value_args_count);
    }

    for (i = 0; i < start_section->value_args_count; i++) {
        uint32 value_idx = start_section->value_args[i];

        if (value_idx >= bindings->value_count) {
            destroy_component_public_values(public_args,
                                            start_section->value_args_count);
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "nested component start section value index %u is out of "
                "bounds",
                value_idx);
        }

        if (!wasm_component_public_value_copy(&public_args[i],
                                              bindings->values[value_idx],
                                              error_buf, error_buf_size)) {
            destroy_component_public_values(public_args,
                                            start_section->value_args_count);
            return false;
        }
    }

    if (!execute_component_start_section_with_public_values(
            inst, function, start_section,
            start_section->result
                ? &runtime_inst->owned_values[runtime_inst->owned_value_count]
                : NULL,
            error_buf, error_buf_size, "nested component start section failed",
            start_section->value_args_count, public_args)) {
        if (start_section->result > 0)
            wasm_component_runtime_value_clear(
                &runtime_inst->owned_values[runtime_inst->owned_value_count]);
        destroy_component_public_values(public_args,
                                        start_section->value_args_count);
        return false;
    }

    if (start_section->result > 0) {
        WASMComponentRuntimeValue *runtime_value =
            &runtime_inst->owned_values[runtime_inst->owned_value_count];

        if (!append_nested_component_local_value(bindings, runtime_value, error_buf,
                                                 error_buf_size)) {
            wasm_component_runtime_value_clear(runtime_value);
            destroy_component_public_values(public_args,
                                            start_section->value_args_count);
            return false;
        }

        runtime_inst->owned_value_count++;
    }

    destroy_component_public_values(public_args, start_section->value_args_count);
    return true;
}

static bool
lookup_component_instance_export(const WASMComponentRuntimeInstance *component_inst,
                                 const char *name,
                                 WASMComponentRuntimeRefType expected_type,
                                 WASMComponentRuntimeRef *out_ref,
                                 char *error_buf, uint32 error_buf_size)
{
    uint32 i;

    for (i = 0; i < component_inst->export_count; i++) {
        const WASMComponentNamedExport *export_item = &component_inst->exports[i];

        if (!strcmp(export_item->name, name)) {
            if (export_item->ref.type != expected_type)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component export \"%s\" resolved to an unexpected type",
                    name);
            *out_ref = export_item->ref;
            return true;
        }
    }

    return set_component_runtime_error_fmt(
        error_buf, error_buf_size,
        "component export \"%s\" was not found on component instance", name);
}

static bool
init_component_runtime_value_from_parsed(
    WASMComponentRuntimeValue *runtime_value, const WASMComponent *component,
    const WASMComponentValue *parsed_value, char *error_buf,
    uint32 error_buf_size)
{
    return wasm_component_runtime_value_init_borrowed(
        runtime_value, component, parsed_value->val_type, parsed_value->core_data,
        parsed_value->core_data_len, error_buf, error_buf_size);
}

static bool
clone_component_runtime_value_borrowed(
    WASMComponentRuntimeValue *dst, const WASMComponentRuntimeValue *src,
    char *error_buf, uint32 error_buf_size)
{
    return wasm_component_runtime_value_clone_borrowed(dst, src, error_buf,
                                                       error_buf_size);
}

static bool
lookup_core_instance_export(const WASMComponentCoreRuntimeInstance *core_instance,
                            const char *name,
                            WASMComponentCoreRuntimeRefType expected_type,
                            WASMComponentCoreRuntimeRef *out_ref,
                            char *error_buf, uint32 error_buf_size)
{
    uint32 i;

    if (core_instance->module_inst) {
        memset(out_ref, 0, sizeof(*out_ref));
        out_ref->type = expected_type;
        out_ref->owner_instance = (WASMComponentCoreRuntimeInstance *)core_instance;

        switch (expected_type) {
            case WASM_COMP_CORE_RUNTIME_REF_FUNC:
                out_ref->of.function = wasm_runtime_lookup_function(
                    core_instance->module_inst, name);
                if (out_ref->of.function)
                    return true;
                break;
            case WASM_COMP_CORE_RUNTIME_REF_TABLE:
                if (wasm_runtime_get_export_table_inst(core_instance->module_inst,
                                                      name,
                                                      &out_ref->of.table))
                    return true;
                break;
            case WASM_COMP_CORE_RUNTIME_REF_MEMORY:
                out_ref->of.memory =
                    wasm_runtime_lookup_memory(core_instance->module_inst, name);
                if (out_ref->of.memory)
                    return true;
                break;
            case WASM_COMP_CORE_RUNTIME_REF_GLOBAL:
                if (wasm_runtime_get_export_global_inst(
                        core_instance->module_inst, name,
                        &out_ref->of.global))
                    return true;
                break;
            default:
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "unsupported core export type for \"%s\"", name);
        }

        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "core export \"%s\" was not found on instantiated core instance",
            name);
    }

    for (i = 0; i < core_instance->export_count; i++) {
        const WASMComponentCoreNamedExport *export_item =
            &core_instance->exports[i];
        if (!strcmp(export_item->name, name)) {
            if (export_item->ref.type != expected_type
                && !(expected_type == WASM_COMP_CORE_RUNTIME_REF_FUNC
                     && export_item->ref.type
                            == WASM_COMP_CORE_RUNTIME_REF_LOWERED_FUNC))
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "core export \"%s\" resolved to an unexpected type", name);
            *out_ref = export_item->ref;
            return true;
        }
    }

    return set_component_runtime_error_fmt(
        error_buf, error_buf_size,
        "core export \"%s\" was not found on synthetic core instance", name);
}

static bool
append_core_alias(WASMComponentInstance *inst, const char *name,
                  const WASMComponentAliasDefinition *alias_def,
                  WASMComponentCoreRuntimeRef ref, char *error_buf,
                  uint32 error_buf_size)
{
    WASMComponentCoreRuntimeRef *sort_space = NULL;
    uint32 *sort_count = NULL;

    if (inst->resolved_alias_count > UINT32_MAX - 1)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size, "too many resolved aliases");

    switch (alias_def->sort->core_sort) {
        case WASM_COMP_CORE_SORT_FUNC:
            sort_space = inst->core_funcs;
            sort_count = &inst->core_func_count;
            break;
        case WASM_COMP_CORE_SORT_TABLE:
            sort_space = inst->core_tables;
            sort_count = &inst->core_table_count;
            break;
        case WASM_COMP_CORE_SORT_MEMORY:
            sort_space = inst->core_memories;
            sort_count = &inst->core_memory_count;
            break;
        case WASM_COMP_CORE_SORT_GLOBAL:
            sort_space = inst->core_globals;
            sort_count = &inst->core_global_count;
            break;
        default:
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "core alias sort 0x%02x is not supported yet",
                (unsigned)alias_def->sort->core_sort);
    }

    sort_space[*sort_count] = ref;
    (*sort_count)++;

    inst->resolved_aliases[inst->resolved_alias_count].name = name;
    inst->resolved_aliases[inst->resolved_alias_count].ref = ref;
    inst->resolved_alias_count++;
    return true;
}

static bool
append_component_alias(WASMComponentInstance *inst, const char *name,
                       const WASMComponentAliasDefinition *alias_def,
                       WASMComponentRuntimeRef ref, char *error_buf,
                       uint32 error_buf_size)
{
    switch (alias_def->sort->sort) {
        case WASM_COMP_SORT_FUNC:
            if (ref.type != WASM_COMP_RUNTIME_REF_FUNC)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component alias \"%s\" did not resolve to a function",
                    name);
            inst->component_funcs[inst->component_func_count++] =
                *ref.of.function;
            return true;
        case WASM_COMP_SORT_VALUE:
            if (ref.type != WASM_COMP_RUNTIME_REF_VALUE)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component alias \"%s\" did not resolve to a value", name);
            return clone_component_runtime_value_borrowed(
                &inst->component_values[inst->component_value_count++],
                ref.of.value, error_buf, error_buf_size);
        case WASM_COMP_SORT_INSTANCE: {
            WASMComponentRuntimeInstance *alias_inst =
                &inst->component_instances[inst->component_instance_count++];

            if (ref.type != WASM_COMP_RUNTIME_REF_INSTANCE)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component alias \"%s\" did not resolve to an instance",
                    name);

            memset(alias_inst, 0, sizeof(*alias_inst));
            alias_inst->owns_exports = false;
            alias_inst->export_count = ref.of.instance->export_count;
            alias_inst->exports = ref.of.instance->exports;
            return true;
        }
        case WASM_COMP_SORT_COMPONENT:
            if (ref.type != WASM_COMP_RUNTIME_REF_COMPONENT)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component alias \"%s\" did not resolve to a component",
                    name);
            inst->components[inst->component_count++] = *ref.of.component;
            inst->components[inst->component_count - 1].owns_scope = false;
            return true;
        default:
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "component alias sort 0x%02x is not supported yet",
                (unsigned)alias_def->sort->sort);
    }
}

static bool
instantiate_component_core_instance(WASMComponentInstance *inst,
                                    const WASMComponentCoreInst *core_inst,
                                    char *error_buf, uint32 error_buf_size)
{
    WASMComponentCoreRuntimeInstance *runtime_inst = NULL;

    runtime_inst = &inst->core_instances[inst->core_instance_count];
    memset(runtime_inst, 0, sizeof(*runtime_inst));

    if (core_inst->instance_expression_tag
        == WASM_COMP_INSTANCE_EXPRESSION_WITH_ARGS) {
        struct InstantiationArgs2 args;
        wasm_module_t module;

        if (core_inst->expression.with_args.idx >= inst->core_module_count)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "core module index %u is out of bounds",
                core_inst->expression.with_args.idx);

        module = inst->core_modules[core_inst->expression.with_args.idx];
        wasm_runtime_instantiation_args_set_defaults(&args);
        runtime_inst->module_inst = wasm_runtime_instantiate_internal(
            (WASMModuleCommon *)module, NULL, NULL, &args, error_buf,
            error_buf_size);
        if (!runtime_inst->module_inst)
            return false;
    }
    else if (core_inst->instance_expression_tag
             == WASM_COMP_INSTANCE_EXPRESSION_WITHOUT_ARGS) {
        uint32 i;
        uint32 export_count = core_inst->expression.without_args.inline_expr_len;

        if (!alloc_component_runtime_array((void **)&runtime_inst->exports,
                                           export_count,
                                           sizeof(*runtime_inst->exports),
                                           error_buf, error_buf_size))
            return false;

        runtime_inst->export_count = export_count;
        for (i = 0; i < export_count; i++) {
            const WASMComponentInlineExport *inline_export =
                &core_inst->expression.without_args.inline_expr[i];

            runtime_inst->exports[i].name = inline_export->name->name;
            if (!resolve_core_sort_idx(inst, inline_export->sort_idx,
                                       &runtime_inst->exports[i].ref, error_buf,
                                       error_buf_size))
                return false;
        }
    }
    else {
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "unsupported core instance expression tag 0x%02x",
            (unsigned)core_inst->instance_expression_tag);
    }

    inst->core_instance_count++;
    return true;
}

static bool
instantiate_nested_component_core_instance(
    WASMComponentRuntimeInstance *runtime_inst,
    WASMNestedComponentLocalBindings *bindings,
    const WASMComponentCoreInst *core_inst, char *error_buf,
    uint32 error_buf_size)
{
    WASMComponentCoreRuntimeInstance *child_inst =
        &runtime_inst->owned_core_instances[runtime_inst->owned_core_instance_count];
    struct InstantiationArgs2 args;
    wasm_module_t module;

    memset(child_inst, 0, sizeof(*child_inst));

    runtime_inst->owned_core_instance_count++;
    if (core_inst->instance_expression_tag
        == WASM_COMP_INSTANCE_EXPRESSION_WITH_ARGS) {
        if (core_inst->expression.with_args.idx >= bindings->core_module_count)
            goto fail_oob;

        module = bindings->core_modules[core_inst->expression.with_args.idx];
        wasm_runtime_instantiation_args_set_defaults(&args);
        child_inst->module_inst = wasm_runtime_instantiate_internal(
            (WASMModuleCommon *)module, NULL, NULL, &args, error_buf,
            error_buf_size);
        if (!child_inst->module_inst)
            goto fail;
    }
    else if (core_inst->instance_expression_tag
             == WASM_COMP_INSTANCE_EXPRESSION_WITHOUT_ARGS) {
        uint32 i;
        uint32 export_count = core_inst->expression.without_args.inline_expr_len;

        if (!alloc_component_runtime_array((void **)&child_inst->exports,
                                           export_count,
                                           sizeof(*child_inst->exports), error_buf,
                                           error_buf_size))
            goto fail;

        child_inst->export_count = export_count;
        for (i = 0; i < export_count; i++) {
            const WASMComponentInlineExport *inline_export =
                &core_inst->expression.without_args.inline_expr[i];

            child_inst->exports[i].name = inline_export->name->name;
            if (!resolve_nested_component_local_core_sort_idx(
                    bindings, inline_export->sort_idx, &child_inst->exports[i].ref,
                    error_buf, error_buf_size))
                goto fail;
        }
    }
    else {
        set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "unsupported nested core instance expression tag 0x%02x",
            (unsigned)core_inst->instance_expression_tag);
        goto fail;
    }

    if (!append_nested_component_local_core_instance(bindings, child_inst, error_buf,
                                                     error_buf_size))
        goto fail;

    return true;

fail:
    destroy_component_core_instance(child_inst);
    runtime_inst->owned_core_instance_count--;
    return false;

fail_oob:
    set_component_runtime_error_fmt(
        error_buf, error_buf_size,
        "nested component core module index %u is out of bounds",
        core_inst->expression.with_args.idx);
    goto fail;
}

static bool
resolve_component_alias_section(WASMComponentInstance *inst,
                                const WASMComponentAliasSection *alias_section,
                                char *error_buf, uint32 error_buf_size)
{
    uint32 i;

    for (i = 0; i < alias_section->count; i++) {
        const WASMComponentAliasDefinition *alias_def =
            &alias_section->aliases[i];
        if (alias_def->alias_target_type == WASM_COMP_ALIAS_TARGET_CORE_EXPORT) {
            const WASMComponentCoreRuntimeInstance *core_instance;
            WASMComponentCoreRuntimeRef ref;
            WASMComponentCoreRuntimeRefType expected_type;
            const char *name;

            if (!alias_def->sort
                || alias_def->sort->sort != WASM_COMP_SORT_CORE_SORT)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "non-core alias sorts are not supported yet");

            if (alias_def->target.core_exported.instance_idx
                >= inst->core_instance_count)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "core alias instance index %u is out of bounds",
                    alias_def->target.core_exported.instance_idx);

            switch (alias_def->sort->core_sort) {
                case WASM_COMP_CORE_SORT_FUNC:
                    expected_type = WASM_COMP_CORE_RUNTIME_REF_FUNC;
                    break;
                case WASM_COMP_CORE_SORT_TABLE:
                    expected_type = WASM_COMP_CORE_RUNTIME_REF_TABLE;
                    break;
                case WASM_COMP_CORE_SORT_MEMORY:
                    expected_type = WASM_COMP_CORE_RUNTIME_REF_MEMORY;
                    break;
                case WASM_COMP_CORE_SORT_GLOBAL:
                    expected_type = WASM_COMP_CORE_RUNTIME_REF_GLOBAL;
                    break;
                default:
                    return set_component_runtime_error_fmt(
                        error_buf, error_buf_size,
                        "core alias sort 0x%02x is not supported yet",
                        (unsigned)alias_def->sort->core_sort);
            }

            core_instance = &inst->core_instances
                                 [alias_def->target.core_exported.instance_idx];
            name = alias_def->target.core_exported.name->name;
            if (!lookup_core_instance_export(core_instance, name, expected_type,
                                             &ref, error_buf, error_buf_size)
                || !append_core_alias(inst, name, alias_def, ref, error_buf,
                                      error_buf_size))
                return false;
            continue;
        }

        if (alias_def->alias_target_type == WASM_COMP_ALIAS_TARGET_EXPORT) {
            const WASMComponentRuntimeInstance *component_instance;
            WASMComponentRuntimeRef ref;
            WASMComponentRuntimeRefType expected_type;
            const char *name;

            memset(&ref, 0, sizeof(ref));

            if (!alias_def->sort
                || alias_def->sort->sort == WASM_COMP_SORT_CORE_SORT)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component alias sorts must not use core sort encoding");

            if (alias_def->target.exported.instance_idx
                >= inst->component_instance_count)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component alias instance index %u is out of bounds",
                    alias_def->target.exported.instance_idx);

            switch (alias_def->sort->sort) {
                case WASM_COMP_SORT_FUNC:
                    expected_type = WASM_COMP_RUNTIME_REF_FUNC;
                    break;
                case WASM_COMP_SORT_VALUE:
                    expected_type = WASM_COMP_RUNTIME_REF_VALUE;
                    break;
                case WASM_COMP_SORT_INSTANCE:
                    expected_type = WASM_COMP_RUNTIME_REF_INSTANCE;
                    break;
                case WASM_COMP_SORT_COMPONENT:
                    expected_type = WASM_COMP_RUNTIME_REF_COMPONENT;
                    break;
                default:
                    return set_component_runtime_error_fmt(
                        error_buf, error_buf_size,
                        "component alias sort 0x%02x is not supported yet",
                        (unsigned)alias_def->sort->sort);
            }

            component_instance =
                &inst->component_instances[alias_def->target.exported.instance_idx];
            name = alias_def->target.exported.name->name;
            if (!lookup_component_instance_export(component_instance, name,
                                                  expected_type, &ref, error_buf,
                                                  error_buf_size)
                || !append_component_alias(inst, name, alias_def, ref, error_buf,
                                           error_buf_size))
                return false;
            continue;
        }

        return set_component_runtime_error_fmt(error_buf, error_buf_size,
                                               "outer aliases are not "
                                               "supported yet");
    }

    return true;
}

static bool
append_component_canon_function(WASMComponentInstance *inst,
                                const WASMComponentCanon *canon,
                                char *error_buf, uint32 error_buf_size)
{
    WASMComponentRuntimeFunc *func;

    if (canon->tag == WASM_COMP_CANON_LIFT) {
        if (inst->component_func_count >= UINT32_MAX)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size, "too many component functions");

        func = &inst->component_funcs[inst->component_func_count++];
        memset(func, 0, sizeof(*func));
        func->canon_tag = canon->tag;
        func->owner_instance = inst;
        if (canon->canon_data.lift.core_func_idx >= inst->core_func_count)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "canon lift core func index %u is out of bounds",
                canon->canon_data.lift.core_func_idx);

        if (inst->module->component.section_count == 0)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size, "component type space is unavailable");

        func->kind = WASM_COMP_RUNTIME_FUNC_LIFT;
        func->type_idx = canon->canon_data.lift.type_idx;
        func->canon_opts = canon->canon_data.lift.canon_opts;
        func->core_func_ref = inst->core_funcs[canon->canon_data.lift.core_func_idx];

        if (func->core_func_ref.type != WASM_COMP_CORE_RUNTIME_REF_FUNC
            && func->core_func_ref.type
                   != WASM_COMP_CORE_RUNTIME_REF_LOWERED_FUNC)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "canon lift core func index %u does not resolve to a function",
                canon->canon_data.lift.core_func_idx);

        if (!resolve_component_canon_lift_abi(inst, func, error_buf,
                                              error_buf_size))
            return false;
    }
    else if (canon->tag == WASM_COMP_CANON_LOWER) {
        WASMComponentRuntimeRef target_ref;

        if (canon->canon_data.lower.func_idx >= inst->component_func_count)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "canon lower func index %u is out of bounds",
                canon->canon_data.lower.func_idx);
        if (inst->lowered_func_count >= UINT32_MAX
            || inst->core_func_count >= UINT32_MAX)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size, "too many canon lower functions");

        target_ref.type = WASM_COMP_RUNTIME_REF_FUNC;
        target_ref.of.function =
            &inst->component_funcs[canon->canon_data.lower.func_idx];
        func = &inst->lowered_funcs[inst->lowered_func_count++];
        memset(func, 0, sizeof(*func));
        func->kind = WASM_COMP_RUNTIME_FUNC_LOWER;
        func->canon_tag = canon->tag;
        func->owner_instance = inst;
        func->canon_opts = canon->canon_data.lower.canon_opts;
        func->lowered_target = target_ref.of.function;

        memset(&inst->core_funcs[inst->core_func_count], 0,
               sizeof(inst->core_funcs[inst->core_func_count]));
        inst->core_funcs[inst->core_func_count].type =
            WASM_COMP_CORE_RUNTIME_REF_LOWERED_FUNC;
        inst->core_funcs[inst->core_func_count].of.lowered_function = func;
        inst->core_func_count++;
    }
    else if (canon->tag == WASM_COMP_CANON_RESOURCE_NEW
             || canon->tag == WASM_COMP_CANON_RESOURCE_DROP
             || canon->tag == WASM_COMP_CANON_RESOURCE_DROP_ASYNC
             || canon->tag == WASM_COMP_CANON_RESOURCE_REP) {
        memset(&inst->core_funcs[inst->core_func_count], 0,
               sizeof(inst->core_funcs[inst->core_func_count]));
        inst->core_funcs[inst->core_func_count].type =
            WASM_COMP_CORE_RUNTIME_REF_FUNC;
        inst->core_func_count++;
    }
    else {
        if (inst->component_func_count >= UINT32_MAX)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size, "too many component functions");

        func = &inst->component_funcs[inst->component_func_count++];
        memset(func, 0, sizeof(*func));
        func->canon_tag = canon->tag;
        func->owner_instance = inst;
        func->kind = WASM_COMP_RUNTIME_FUNC_UNSUPPORTED_CANON;
    }

    return true;
}

static bool
append_nested_component_canon(
    WASMComponentInstance *inst, WASMComponentRuntimeInstance *runtime_inst,
    WASMNestedComponentLocalBindings *bindings, const WASMComponentCanon *canon,
    char *error_buf, uint32 error_buf_size)
{
    WASMComponentRuntimeFunc *func;
    WASMComponentRuntimeRef component_ref;
    WASMComponentCoreRuntimeRef core_ref;

    switch (canon->tag) {
        case WASM_COMP_CANON_LIFT:
            if (canon->canon_data.lift.core_func_idx >= bindings->core_func_count)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "nested canon lift core func index %u is out of bounds",
                    canon->canon_data.lift.core_func_idx);

            if (runtime_inst->owned_func_count >= UINT32_MAX)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "too many nested canon lift functions");

            func = &runtime_inst->owned_funcs[runtime_inst->owned_func_count++];
            memset(func, 0, sizeof(*func));
            func->canon_tag = canon->tag;
            func->owner_instance = inst;
            func->kind = WASM_COMP_RUNTIME_FUNC_LIFT;
            func->type_idx = canon->canon_data.lift.type_idx;
            func->canon_opts = canon->canon_data.lift.canon_opts;
            func->core_func_ref = bindings->core_funcs[canon->canon_data.lift.core_func_idx];
            if (!resolve_component_canon_lift_abi(inst, func, error_buf,
                                                  error_buf_size))
                return false;

            memset(&component_ref, 0, sizeof(component_ref));
            component_ref.type = WASM_COMP_RUNTIME_REF_FUNC;
            component_ref.of.function = func;
            return append_nested_component_local_ref(bindings, component_ref, error_buf,
                                                     error_buf_size);
        case WASM_COMP_CANON_LOWER:
            if (canon->canon_data.lower.func_idx >= bindings->func_count)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "nested canon lower func index %u is out of bounds",
                    canon->canon_data.lower.func_idx);
            if (runtime_inst->owned_lowered_func_count >= UINT32_MAX)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "too many nested canon lower functions");

            func =
                &runtime_inst->owned_lowered_funcs[runtime_inst->owned_lowered_func_count++];
            memset(func, 0, sizeof(*func));
            func->kind = WASM_COMP_RUNTIME_FUNC_LOWER;
            func->canon_tag = canon->tag;
            func->owner_instance = inst;
            func->canon_opts = canon->canon_data.lower.canon_opts;
            func->lowered_target = bindings->funcs[canon->canon_data.lower.func_idx]
                                       .of.function;

            memset(&core_ref, 0, sizeof(core_ref));
            core_ref.type = WASM_COMP_CORE_RUNTIME_REF_LOWERED_FUNC;
            core_ref.of.lowered_function = func;
            return append_nested_component_local_core_func(bindings, core_ref, error_buf,
                                                           error_buf_size);
        case WASM_COMP_CANON_RESOURCE_NEW:
        case WASM_COMP_CANON_RESOURCE_DROP:
        case WASM_COMP_CANON_RESOURCE_DROP_ASYNC:
        case WASM_COMP_CANON_RESOURCE_REP:
            memset(&core_ref, 0, sizeof(core_ref));
            core_ref.type = WASM_COMP_CORE_RUNTIME_REF_FUNC;
            return append_nested_component_local_core_func(bindings, core_ref, error_buf,
                                                           error_buf_size);
        default:
            if (runtime_inst->owned_func_count >= UINT32_MAX)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "too many nested component canon functions");

            func = &runtime_inst->owned_funcs[runtime_inst->owned_func_count++];
            memset(func, 0, sizeof(*func));
            func->canon_tag = canon->tag;
            func->owner_instance = inst;
            func->kind = WASM_COMP_RUNTIME_FUNC_UNSUPPORTED_CANON;
            memset(&component_ref, 0, sizeof(component_ref));
            component_ref.type = WASM_COMP_RUNTIME_REF_FUNC;
            component_ref.of.function = func;
            return append_nested_component_local_ref(bindings, component_ref, error_buf,
                                                     error_buf_size);
    }
}

static bool
resolve_nested_component_exports(
    WASMComponentRuntimeInstance *runtime_inst,
    const WASMComponentExportSection *export_section,
    const WASMNestedComponentLocalBindings *bindings,
    char *error_buf, uint32 error_buf_size)
{
    uint32 j;

    for (j = 0; j < export_section->count; j++) {
        const WASMComponentExport *component_export = &export_section->exports[j];
        WASMComponentRuntimeRef ref;
        WASMComponentRuntimeRefType expected_type;
        const char *sort_name;

        if (!component_export->sort_idx || !component_export->sort_idx->sort)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "nested component export is missing a sort");

        switch (component_export->sort_idx->sort->sort) {
            case WASM_COMP_SORT_CORE_SORT:
                if (component_export->sort_idx->sort->core_sort
                    != WASM_COMP_CORE_SORT_MODULE)
                    return set_component_runtime_error_fmt(
                        error_buf, error_buf_size,
                        "only core module sort is supported in nested "
                        "component exports");
                expected_type = WASM_COMP_RUNTIME_REF_CORE_MODULE;
                sort_name = "core module";
                break;
            case WASM_COMP_SORT_FUNC:
                expected_type = WASM_COMP_RUNTIME_REF_FUNC;
                sort_name = "func";
                break;
            case WASM_COMP_SORT_VALUE:
                expected_type = WASM_COMP_RUNTIME_REF_VALUE;
                sort_name = "value";
                break;
            case WASM_COMP_SORT_INSTANCE:
                expected_type = WASM_COMP_RUNTIME_REF_INSTANCE;
                sort_name = "instance";
                break;
            case WASM_COMP_SORT_COMPONENT:
                expected_type = WASM_COMP_RUNTIME_REF_COMPONENT;
                sort_name = "component";
                break;
            default:
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "nested component export sort 0x%02x is not supported yet",
                    (unsigned)component_export->sort_idx->sort->sort);
        }

        if (!resolve_nested_component_local_sort_idx(
                bindings, component_export->sort_idx, &ref, error_buf,
                error_buf_size))
            return false;

        if (ref.type != expected_type)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "nested component export \"%s\" does not resolve to a %s",
                get_component_export_name(component_export->export_name),
                sort_name);

        runtime_inst->exports[runtime_inst->export_count].name =
            get_component_export_name(component_export->export_name);
        runtime_inst->exports[runtime_inst->export_count].ref = ref;
        runtime_inst->export_count++;
    }

    runtime_inst->owns_exports = true;
    return true;
}

static bool
count_nested_component_local_bindings(const WASMComponent *nested_component,
                                      uint32 *import_count,
                                      uint32 *core_module_count,
                                      uint32 *core_instance_count,
                                      uint32 *core_func_count,
                                      uint32 *func_count,
                                      uint32 *value_count,
                                      uint32 *instance_count,
                                      uint32 *component_count,
                                      uint32 *owned_func_count,
                                      uint32 *owned_value_count,
                                      uint32 *owned_component_count,
                                      uint32 *owned_core_instance_count,
                                      uint32 *owned_lowered_func_count,
                                      uint32 *owned_instance_count,
                                      uint32 *export_count, char *error_buf,
                                      uint32 error_buf_size)
{
    uint32 i;

    *import_count = *core_module_count = *core_instance_count = *core_func_count
        = *func_count = *value_count = *instance_count = *component_count
        = *owned_func_count = *owned_value_count = *owned_component_count
        = *owned_core_instance_count = *owned_lowered_func_count
        = *owned_instance_count = *export_count = 0;

    for (i = 0; i < nested_component->section_count; i++) {
        const WASMComponentSection *section = &nested_component->sections[i];

        switch (section->id) {
            case WASM_COMP_SECTION_CORE_CUSTOM:
            case WASM_COMP_SECTION_TYPE:
                break;
            case WASM_COMP_SECTION_IMPORTS:
            {
                uint32 j;
                const WASMComponentImportSection *import_section =
                    section->parsed.import_section;

                for (j = 0; j < import_section->count; j++) {
                    const WASMComponentImport *component_import =
                        &import_section->imports[j];

                    if (!component_import->extern_desc)
                        return set_component_runtime_error_fmt(
                            error_buf, error_buf_size,
                            "nested component import is missing an external "
                            "descriptor");

                    switch (component_import->extern_desc->type) {
                        case WASM_COMP_EXTERN_FUNC:
                            (*import_count)++;
                            (*func_count)++;
                            break;
                        case WASM_COMP_EXTERN_VALUE:
                            (*import_count)++;
                            (*value_count)++;
                            break;
                        case WASM_COMP_EXTERN_CORE_MODULE:
                            (*import_count)++;
                            (*core_module_count)++;
                            break;
                        case WASM_COMP_EXTERN_INSTANCE:
                            (*import_count)++;
                            (*instance_count)++;
                            break;
                        case WASM_COMP_EXTERN_COMPONENT:
                            (*import_count)++;
                            (*component_count)++;
                            break;
                        default:
                            return set_component_runtime_error_fmt(
                                error_buf, error_buf_size,
                                "nested component imports other than "
                                "core_module/func/value/instance/component are "
                                "not supported yet");
                    }
                }
                break;
            }
            case WASM_COMP_SECTION_CORE_MODULE:
                (*core_module_count)++;
                break;
            case WASM_COMP_SECTION_CORE_INSTANCE:
                (*core_instance_count) +=
                    section->parsed.core_instance_section->count;
                (*owned_core_instance_count) +=
                    section->parsed.core_instance_section->count;
                break;
            case WASM_COMP_SECTION_CANONS:
            {
                uint32 j;
                const WASMComponentCanonSection *canon_section =
                    section->parsed.canon_section;

                for (j = 0; j < canon_section->count; j++) {
                    switch (canon_section->canons[j].tag) {
                        case WASM_COMP_CANON_LIFT:
                            (*func_count)++;
                            (*owned_func_count)++;
                            break;
                        case WASM_COMP_CANON_LOWER:
                            (*core_func_count)++;
                            (*owned_lowered_func_count)++;
                            break;
                        case WASM_COMP_CANON_RESOURCE_NEW:
                        case WASM_COMP_CANON_RESOURCE_DROP:
                        case WASM_COMP_CANON_RESOURCE_DROP_ASYNC:
                        case WASM_COMP_CANON_RESOURCE_REP:
                            (*core_func_count)++;
                            break;
                        default:
                            (*func_count)++;
                            (*owned_func_count)++;
                            break;
                    }
                }
                break;
            }
            case WASM_COMP_SECTION_CORE_TYPE:
                break;
            case WASM_COMP_SECTION_ALIASES:
            {
                uint32 j;
                const WASMComponentAliasSection *alias_section =
                    section->parsed.alias_section;

                for (j = 0; j < alias_section->count; j++) {
                    const WASMComponentAliasDefinition *alias_def =
                        &alias_section->aliases[j];

                    if (!alias_def->sort
                        || alias_def->sort->sort == WASM_COMP_SORT_CORE_SORT)
                        return set_component_runtime_error_fmt(
                            error_buf, error_buf_size,
                            "nested component aliases must use component "
                            "func/value/instance/component sorts");

                    switch (alias_def->sort->sort) {
                        case WASM_COMP_SORT_FUNC:
                            (*func_count)++;
                            break;
                        case WASM_COMP_SORT_VALUE:
                            (*value_count)++;
                            break;
                        case WASM_COMP_SORT_INSTANCE:
                            (*instance_count)++;
                            break;
                        case WASM_COMP_SORT_COMPONENT:
                            (*component_count)++;
                            break;
                        default:
                            return set_component_runtime_error_fmt(
                                error_buf, error_buf_size,
                                "nested component alias sort 0x%02x is not "
                                "supported yet",
                                (unsigned)alias_def->sort->sort);
                    }
                }
                break;
            }
            case WASM_COMP_SECTION_COMPONENT:
                (*component_count)++;
                (*owned_component_count)++;
                break;
            case WASM_COMP_SECTION_VALUES:
                (*value_count) += section->parsed.value_section->count;
                (*owned_value_count) += section->parsed.value_section->count;
                break;
            case WASM_COMP_SECTION_EXPORTS:
                *export_count += section->parsed.export_section->count;
                break;
            case WASM_COMP_SECTION_INSTANCES:
            {
                uint32 j;
                const WASMComponentInstSection *inst_section =
                    section->parsed.instance_section;

                for (j = 0; j < inst_section->count; j++) {
                    const WASMComponentInst *component_inst =
                        &inst_section->instances[j];

                    if (component_inst->instance_expression_tag
                            != WASM_COMP_INSTANCE_EXPRESSION_WITHOUT_ARGS
                        && component_inst->instance_expression_tag
                               != WASM_COMP_INSTANCE_EXPRESSION_WITH_ARGS)
                        return set_component_runtime_error_fmt(
                            error_buf, error_buf_size,
                            "unsupported nested component instance "
                            "expression tag 0x%02x",
                            (unsigned)component_inst->instance_expression_tag);

                    (*instance_count)++;
                    (*owned_instance_count)++;
                }
                break;
            }
            case WASM_COMP_SECTION_START:
                if (!section->parsed.start_section)
                    return set_component_runtime_error_fmt(
                        error_buf, error_buf_size,
                        "nested component start section payload is missing");
                (*value_count) += section->parsed.start_section->result;
                (*owned_value_count) += section->parsed.start_section->result;
                break;
            default:
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "nested component section 0x%02x is not supported yet",
                    (unsigned)section->id);
        }
    }

    return true;
}

static bool
instantiate_component_value_section(WASMComponentInstance *inst,
                                    const WASMComponentValueSection *value_section,
                                    char *error_buf, uint32 error_buf_size)
{
    uint32 i;

    for (i = 0; i < value_section->count; i++) {
        if (!init_component_runtime_value_from_parsed(
                &inst->component_values[inst->component_value_count],
                &inst->module->component, &value_section->values[i], error_buf,
                error_buf_size))
            return false;
        inst->component_value_count++;
    }

    return true;
}

static bool
instantiate_nested_component_value_section(
    WASMComponentRuntimeInstance *runtime_inst,
    WASMNestedComponentLocalBindings *bindings, const WASMComponent *component,
    const WASMComponentValueSection *value_section, char *error_buf,
    uint32 error_buf_size)
{
    uint32 i;

    for (i = 0; i < value_section->count; i++) {
        WASMComponentRuntimeValue *runtime_value =
            &runtime_inst->owned_values[runtime_inst->owned_value_count];

        if (!init_component_runtime_value_from_parsed(
                runtime_value, component, &value_section->values[i], error_buf,
                error_buf_size)
            || !append_nested_component_local_value(bindings, runtime_value,
                                                    error_buf, error_buf_size))
            return false;

        runtime_inst->owned_value_count++;
    }

    return true;
}

static uint32
find_component_core_type_count(const WASMComponent *component)
{
    uint32 i, core_type_count = 0;

    for (i = 0; i < component->section_count; i++) {
        const WASMComponentSection *section = &component->sections[i];

        if (section->id == WASM_COMP_SECTION_CORE_TYPE
            && section->parsed.core_type_section)
            core_type_count += section->parsed.core_type_section->count;
    }

    return core_type_count;
}

static uint32
find_component_type_count(const WASMComponent *component)
{
    uint32 i, type_count = 0;

    for (i = 0; i < component->section_count; i++) {
        const WASMComponentSection *section = &component->sections[i];

        if (section->id == WASM_COMP_SECTION_TYPE && section->parsed.type_section)
            type_count += section->parsed.type_section->count;
    }

    return type_count;
}

static bool
nullable_strings_equal(const char *lhs, const char *rhs)
{
    if (!lhs || !rhs)
        return lhs == rhs;
    return strcmp(lhs, rhs) == 0;
}

static const WASMComponentCoreType *
lookup_component_core_type(const WASMComponent *component, uint32 type_idx)
{
    uint32 i;

    for (i = 0; i < component->section_count; i++) {
        const WASMComponentSection *section = &component->sections[i];

        if (section->id != WASM_COMP_SECTION_CORE_TYPE
            || !section->parsed.core_type_section)
            continue;

        if (type_idx < section->parsed.core_type_section->count)
            return &section->parsed.core_type_section->types[type_idx];

        type_idx -= section->parsed.core_type_section->count;
    }

    return NULL;
}

static const WASMComponentCoreFuncType *
resolve_component_core_func_type(const WASMComponentCoreType *type)
{
    if (!type || !type->deftype)
        return NULL;

    switch (type->deftype->tag) {
        case WASM_CORE_DEFTYPE_SUBTYPE:
            if (type->deftype->type.subtype
                && type->deftype->type.subtype->comptype
                && type->deftype->type.subtype->comptype->tag
                       == WASM_CORE_COMPTYPE_FUNC)
                return &type->deftype->type.subtype->comptype->type.func_type;
            break;
        case WASM_CORE_DEFTYPE_RECTYPE:
            if (type->deftype->type.rectype
                && type->deftype->type.rectype->subtype_count == 1
                && type->deftype->type.rectype->subtypes
                && type->deftype->type.rectype->subtypes[0].comptype.tag
                       == WASM_CORE_COMPTYPE_FUNC)
                return &type->deftype->type.rectype->subtypes[0].comptype.type
                            .func_type;
            break;
        default:
            break;
    }

    return NULL;
}

static const WASMComponentCoreFuncType *
resolve_core_moduletype_func_type(const WASMComponentCoreModuleType *module_type,
                                  uint32 type_idx)
{
    uint32 i;

    if (!module_type)
        return NULL;

    for (i = 0; i < module_type->decl_count; i++) {
        const WASMComponentCoreModuleDecl *decl = &module_type->declarations[i];

        if (decl->tag != WASM_CORE_MODULEDECL_TYPE)
            continue;

        if (type_idx == 0)
            return resolve_component_core_func_type(decl->decl.type_decl.type);

        type_idx--;
    }

    return NULL;
}

static bool
core_valtype_matches_runtime_type(const WASMComponentCoreValType *expected,
                                  uint8 actual_type)
{
    if (!expected)
        return false;

    switch (expected->tag) {
        case WASM_CORE_VALTYPE_NUM:
            return (uint8)expected->type.num_type == actual_type;
        case WASM_CORE_VALTYPE_VECTOR:
            return (uint8)expected->type.vector_type == actual_type;
        case WASM_CORE_VALTYPE_REF:
            return (uint8)expected->type.ref_type == actual_type;
        default:
            return false;
    }
}

static bool
core_func_type_matches_runtime_type(const WASMComponentCoreFuncType *expected,
                                    const WASMFuncType *actual)
{
    uint32 i;

    if (!expected || !actual || expected->params.count != actual->param_count
        || expected->results.count != actual->result_count)
        return false;

    for (i = 0; i < expected->params.count; i++) {
        if (!core_valtype_matches_runtime_type(&expected->params.val_types[i],
                                               actual->types[i]))
            return false;
    }

    for (i = 0; i < expected->results.count; i++) {
        if (!core_valtype_matches_runtime_type(
                &expected->results.val_types[i],
                actual->types[expected->params.count + i]))
            return false;
    }

    return true;
}

static bool
core_limits_match_runtime_table(const WASMComponentCoreLimits *expected,
                                const WASMTableType *actual)
{
    if (!expected || !actual || expected->lim.limits.min != actual->init_size)
        return false;

    if (expected->tag == WASM_CORE_LIMITS_MAX)
        return expected->lim.limits_max.max == actual->max_size;

    return true;
}

static bool
core_limits_match_runtime_memory(const WASMComponentCoreLimits *expected,
                                 const WASMMemoryType *actual)
{
    if (!expected || !actual
        || (expected->tag == WASM_CORE_LIMITS_MIN
            && expected->lim.limits.min != actual->init_page_count)
        || (expected->tag == WASM_CORE_LIMITS_MAX
            && expected->lim.limits_max.min != actual->init_page_count))
        return false;

    if (expected->tag == WASM_CORE_LIMITS_MAX)
        return expected->lim.limits_max.max == actual->max_page_count;

    return true;
}

static bool
validate_core_desc_against_runtime_type(
    const WASMComponentCoreModuleType *module_type,
    const WASMComponentCoreImportDesc *expected, const char *member_name,
    bool is_import, const wasm_import_t *actual_import,
    const wasm_export_t *actual_export, char *error_buf, uint32 error_buf_size)
{
    wasm_import_export_kind_t actual_kind =
        is_import ? actual_import->kind : actual_export->kind;

    switch (expected->type) {
        case WASM_CORE_IMPORTDESC_FUNC:
        {
            const WASMComponentCoreFuncType *expected_func_type =
                resolve_core_moduletype_func_type(module_type,
                                                  expected->desc.func_type_idx);
            const WASMFuncType *actual_func_type =
                is_import ? actual_import->u.func_type : actual_export->u.func_type;

            if (actual_kind != WASM_IMPORT_EXPORT_KIND_FUNC)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component import expects core module %s \"%s\" to be a "
                    "function",
                    is_import ? "import" : "export", member_name);

            if (!expected_func_type)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component import core module uses unresolved function "
                    "type index %u",
                    expected->desc.func_type_idx);

            if (!core_func_type_matches_runtime_type(expected_func_type,
                                                     actual_func_type))
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component import expects core module %s \"%s\" to match "
                    "the declared function type",
                    is_import ? "import" : "export", member_name);
            return true;
        }
        case WASM_CORE_IMPORTDESC_TABLE:
            if (actual_kind != WASM_IMPORT_EXPORT_KIND_TABLE)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component import expects core module %s \"%s\" to be a "
                    "table",
                    is_import ? "import" : "export", member_name);
            if (!core_limits_match_runtime_table(
                    expected->desc.table_type.limits,
                    is_import ? actual_import->u.table_type
                              : actual_export->u.table_type)
                || (uint8)expected->desc.table_type.ref_type
                       != (is_import ? actual_import->u.table_type->elem_type
                                     : actual_export->u.table_type->elem_type))
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component import expects core module %s \"%s\" to match "
                    "the declared table type",
                    is_import ? "import" : "export", member_name);
            return true;
        case WASM_CORE_IMPORTDESC_MEMORY:
            if (actual_kind != WASM_IMPORT_EXPORT_KIND_MEMORY)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component import expects core module %s \"%s\" to be a "
                    "memory",
                    is_import ? "import" : "export", member_name);
            if (!core_limits_match_runtime_memory(
                    expected->desc.memory_type.limits,
                    is_import ? actual_import->u.memory_type
                              : actual_export->u.memory_type))
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component import expects core module %s \"%s\" to match "
                    "the declared memory type",
                    is_import ? "import" : "export", member_name);
            return true;
        case WASM_CORE_IMPORTDESC_GLOBAL:
            if (actual_kind != WASM_IMPORT_EXPORT_KIND_GLOBAL)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component import expects core module %s \"%s\" to be a "
                    "global",
                    is_import ? "import" : "export", member_name);
            if (!core_valtype_matches_runtime_type(
                    &expected->desc.global_type.val_type,
                    is_import ? actual_import->u.global_type->val_type
                              : actual_export->u.global_type->val_type)
                || expected->desc.global_type.is_mutable
                       != (is_import ? actual_import->u.global_type->is_mutable
                                     : actual_export->u.global_type->is_mutable))
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component import expects core module %s \"%s\" to match "
                    "the declared global type",
                    is_import ? "import" : "export", member_name);
            return true;
        default:
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "component import uses an unsupported core module member type");
    }
}

static bool
validate_core_module_against_type(const wasm_module_t module,
                                  const WASMComponentCoreModuleType *module_type,
                                  const char *import_name, char *error_buf,
                                  uint32 error_buf_size)
{
    uint32 i;

    for (i = 0; i < module_type->decl_count; i++) {
        const WASMComponentCoreModuleDecl *decl = &module_type->declarations[i];

        switch (decl->tag) {
            case WASM_CORE_MODULEDECL_IMPORT:
            {
                int32 import_count = wasm_runtime_get_import_count(module);
                int32 j;
                bool matched = false;

                for (j = 0; j < import_count; j++) {
                    wasm_import_t actual_import;
                    const char *expected_module_name;
                    const char *expected_name;

                    wasm_runtime_get_import_type(module, j, &actual_import);
                    expected_module_name = decl->decl.import_decl.import->mod_name
                                               ? decl->decl.import_decl.import
                                                     ->mod_name->name
                                               : NULL;
                    expected_name = decl->decl.import_decl.import->nm
                                        ? decl->decl.import_decl.import->nm->name
                                        : NULL;

                    if (!nullable_strings_equal(expected_module_name,
                                                actual_import.module_name)
                        || !nullable_strings_equal(expected_name,
                                                   actual_import.name))
                        continue;

                    if (!validate_core_desc_against_runtime_type(
                            module_type,
                            decl->decl.import_decl.import->import_desc,
                            expected_name ? expected_name : "<unnamed>", true,
                            &actual_import, NULL, error_buf, error_buf_size))
                        return false;

                    matched = true;
                    break;
                }

                if (!matched)
                    return set_component_runtime_error_fmt(
                        error_buf, error_buf_size,
                        "component import \"%s\" expects core module import "
                        "\"%s\".\"%s\"",
                        import_name,
                        decl->decl.import_decl.import->mod_name
                                && decl->decl.import_decl.import->mod_name->name
                            ? decl->decl.import_decl.import->mod_name->name
                            : "",
                        decl->decl.import_decl.import->nm
                                && decl->decl.import_decl.import->nm->name
                            ? decl->decl.import_decl.import->nm->name
                            : "");
                break;
            }
            case WASM_CORE_MODULEDECL_EXPORT:
            {
                int32 export_count = wasm_runtime_get_export_count(module);
                int32 j;
                bool matched = false;
                const char *expected_name = decl->decl.export_decl.export_decl->name
                                                ? decl->decl.export_decl.export_decl
                                                      ->name->name
                                                : NULL;

                for (j = 0; j < export_count; j++) {
                    wasm_export_t actual_export;

                    wasm_runtime_get_export_type(module, j, &actual_export);
                    if (!nullable_strings_equal(expected_name, actual_export.name))
                        continue;

                    if (!validate_core_desc_against_runtime_type(
                            module_type,
                            decl->decl.export_decl.export_decl->export_desc,
                            expected_name ? expected_name : "<unnamed>", false,
                            NULL, &actual_export, error_buf, error_buf_size))
                        return false;

                    matched = true;
                    break;
                }

                if (!matched)
                    return set_component_runtime_error_fmt(
                        error_buf, error_buf_size,
                        "component import \"%s\" expects core module export "
                        "\"%s\"",
                        import_name, expected_name ? expected_name : "");
                break;
            }
            case WASM_CORE_MODULEDECL_TYPE:
            case WASM_CORE_MODULEDECL_ALIAS:
                break;
            default:
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component import \"%s\" uses an unsupported core module "
                    "type declaration",
                    import_name);
        }
    }

    return true;
}

static bool
resolve_component_instance_type(const WASMComponent *component, uint32 type_idx,
                                const char *import_name,
                                const WASMComponentInstType **out_type,
                                char *error_buf, uint32 error_buf_size)
{
    const WASMComponentTypes *type_entry =
        wasm_component_lookup_type(component, type_idx);

    if (!type_entry)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component import \"%s\" uses unresolved instance type index %u",
            import_name, type_idx);

    if (type_entry->tag != WASM_COMP_INSTANCE_TYPE || !type_entry->type.instance_type)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component import \"%s\" instance type index %u is not an "
            "instance type",
            import_name, type_idx);

    *out_type = type_entry->type.instance_type;
    return true;
}

static bool
resolve_component_component_type(const WASMComponent *component, uint32 type_idx,
                                 const char *import_name,
                                 const char *member_name,
                                 const char *role,
                                 const WASMComponentComponentType **out_type,
                                 char *error_buf, uint32 error_buf_size)
{
    const WASMComponentTypes *type_entry =
        wasm_component_lookup_type(component, type_idx);

    if (!type_entry)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            member_name
                ? "component import \"%s\" instance export \"%s\" uses "
                  "unresolved %s component type index %u"
                : "component import \"%s\" uses unresolved %s component type "
                  "index %u",
            import_name ? import_name : "<unnamed>",
            member_name ? member_name : "", role, type_idx);

    if (type_entry->tag != WASM_COMP_COMPONENT_TYPE
        || !type_entry->type.component_type)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            member_name
                ? "component import \"%s\" instance export \"%s\" %s component "
                  "type index %u is not a component type"
                : "component import \"%s\" %s component type index %u is not a "
                  "component type",
            import_name ? import_name : "<unnamed>",
            member_name ? member_name : "", role, type_idx);

    *out_type = type_entry->type.component_type;
    return true;
}

static const WASMComponentExport *
lookup_component_export_by_name(const WASMComponent *component,
                                const char *export_name,
                                WASMComponentSortValues sort)
{
    uint32 i, j;

    if (!component || !export_name)
        return NULL;

    for (i = 0; i < component->section_count; i++) {
        const WASMComponentSection *section = &component->sections[i];

        if (section->id != WASM_COMP_SECTION_EXPORTS || !section->parsed.export_section)
            continue;

        for (j = 0; j < section->parsed.export_section->count; j++) {
            const WASMComponentExport *component_export =
                &section->parsed.export_section->exports[j];
            const char *actual_name =
                get_component_export_name(component_export->export_name);

            if (!component_export->sort_idx || !component_export->sort_idx->sort
                || component_export->sort_idx->sort->sort != sort
                || !nullable_strings_equal(actual_name, export_name))
                continue;

            return component_export;
        }
    }

    return NULL;
}

static const WASMComponentComponentDeclExport *
lookup_component_type_export_decl(
    const WASMComponentComponentType *component_type, const char *export_name,
    WASMComponentExternDescType extern_type)
{
    uint32 i;

    if (!component_type || !export_name)
        return NULL;

    for (i = 0; i < component_type->count; i++) {
        const WASMComponentComponentDecl *decl = &component_type->component_decls[i];
        const WASMComponentInstDecl *instance_decl;
        const WASMComponentComponentDeclExport *export_decl;
        const char *actual_name;

        if (decl->tag != WASM_COMP_COMPONENT_DECL_EXPORT
            || !decl->decl.instance_decl)
            continue;

        instance_decl = decl->decl.instance_decl;
        if (instance_decl->tag != WASM_COMP_COMPONENT_DECL_INSTANCE_EXPORTDECL)
            continue;

        export_decl = instance_decl->decl.export_decl;
        actual_name =
            export_decl ? get_component_export_name(export_decl->export_name) : NULL;
        if (!export_decl || !export_decl->extern_desc
            || export_decl->extern_desc->type != extern_type
            || !nullable_strings_equal(actual_name, export_name))
            continue;

        return export_decl;
    }

    return NULL;
}

static bool
validate_component_type_has_no_imports(
    const WASMComponentComponentType *component_type, const char *import_name,
    const char *member_name, const char *role, char *error_buf,
    uint32 error_buf_size)
{
    uint32 i;

    if (!component_type)
        return false;

    for (i = 0; i < component_type->count; i++) {
        const WASMComponentComponentDecl *decl = &component_type->component_decls[i];

        if (decl->tag == WASM_COMP_COMPONENT_DECL_IMPORT)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                member_name
                    ? "component import \"%s\" instance export \"%s\" uses "
                      "unsupported %s component imports"
                    : "component import \"%s\" uses unsupported %s component "
                      "imports",
                import_name ? import_name : "<unnamed>",
                member_name ? member_name : "", role);
    }

    return true;
}

static bool
validate_component_type_against_type(
    const WASMComponent *expected_component,
    const WASMComponentComponentType *expected_type,
    const WASMComponent *actual_component,
    const WASMComponentComponentType *actual_type, const char *import_name,
    const char *member_name, uint32 remaining_depth, char *error_buf,
    uint32 error_buf_size);

static bool
count_component_imports(const WASMComponent *component, uint32 *import_count);

static bool
validate_runtime_component_against_type(
    const WASMComponent *expected_component, const WASMComponent *actual_component,
    const WASMComponentComponentType *expected_type, const char *import_name,
    const char *member_name, uint32 remaining_depth, char *error_buf,
    uint32 error_buf_size)
{
    uint32 i, import_count = 0;

    if (!expected_component || !actual_component || !expected_type)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            member_name ? "component import \"%s\" instance export \"%s\" is "
                          "missing component type metadata"
                        : "component import \"%s\" is missing component type "
                          "metadata",
            import_name ? import_name : "<unnamed>",
            member_name ? member_name : "");

    if (remaining_depth == 0)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            member_name ? "component import \"%s\" instance export \"%s\" "
                          "component type recursion is unsupported"
                        : "component import \"%s\" component type recursion is "
                          "unsupported",
            import_name ? import_name : "<unnamed>",
            member_name ? member_name : "");

    if (!validate_component_type_has_no_imports(expected_type, import_name,
                                                member_name, "expected", error_buf,
                                                error_buf_size)
        || !count_component_imports(actual_component, &import_count))
        return false;

    if (import_count != 0)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            member_name ? "component import \"%s\" instance export \"%s\" "
                          "uses unsupported actual component imports"
                        : "component import \"%s\" uses unsupported actual "
                          "component imports",
            import_name ? import_name : "<unnamed>",
            member_name ? member_name : "");

    for (i = 0; i < expected_type->count; i++) {
        const WASMComponentComponentDecl *decl = &expected_type->component_decls[i];

        switch (decl->tag) {
            case WASM_COMP_COMPONENT_DECL_TYPE:
            case WASM_COMP_COMPONENT_DECL_ALIAS:
                break;
            case WASM_COMP_COMPONENT_DECL_IMPORT:
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    member_name
                        ? "component import \"%s\" instance export \"%s\" uses "
                          "unsupported expected component imports"
                        : "component import \"%s\" uses unsupported expected "
                          "component imports",
                    import_name ? import_name : "<unnamed>",
                    member_name ? member_name : "");
            case WASM_COMP_COMPONENT_DECL_EXPORT:
            {
                const WASMComponentInstDecl *instance_decl = decl->decl.instance_decl;
                const WASMComponentComponentDeclExport *export_decl =
                    instance_decl ? instance_decl->decl.export_decl : NULL;
                const char *expected_export_name =
                    export_decl ? get_component_export_name(export_decl->export_name)
                                : NULL;
                const WASMComponentExport *actual_export;
                const WASMComponentComponentType *expected_nested_type;
                const WASMComponentComponentType *actual_nested_type;

                if (!instance_decl
                    || instance_decl->tag
                           != WASM_COMP_COMPONENT_DECL_INSTANCE_EXPORTDECL
                    || !export_decl || !export_decl->extern_desc
                    || export_decl->extern_desc->type != WASM_COMP_EXTERN_COMPONENT
                    || !expected_export_name)
                    return member_name
                               ? set_component_runtime_error_fmt(
                                     error_buf, error_buf_size,
                                     "component import \"%s\" instance export "
                                     "\"%s\" uses unsupported typed component "
                                     "export matching",
                                     import_name ? import_name : "<unnamed>",
                                     member_name)
                               : set_component_runtime_error_fmt(
                                     error_buf, error_buf_size,
                                     "component import \"%s\" uses unsupported "
                                     "typed component export matching",
                                     import_name ? import_name : "<unnamed>");

                actual_export = lookup_component_export_by_name(
                    actual_component, expected_export_name, WASM_COMP_SORT_COMPONENT);
                if (!actual_export)
                    return member_name
                               ? set_component_runtime_error_fmt(
                                     error_buf, error_buf_size,
                                     "component import \"%s\" instance export "
                                     "\"%s\" expects component export \"%s\"",
                                     import_name ? import_name : "<unnamed>",
                                     member_name, expected_export_name)
                               : set_component_runtime_error_fmt(
                                     error_buf, error_buf_size,
                                     "component import \"%s\" expects component "
                                     "export \"%s\"",
                                     import_name ? import_name : "<unnamed>",
                                     expected_export_name);

                if (!actual_export->extern_desc
                    || actual_export->extern_desc->type != WASM_COMP_EXTERN_COMPONENT)
                    return member_name
                               ? set_component_runtime_error_fmt(
                                     error_buf, error_buf_size,
                                     "component import \"%s\" instance export "
                                     "\"%s\" component export \"%s\" is missing "
                                     "component type metadata",
                                     import_name ? import_name : "<unnamed>",
                                     member_name, expected_export_name)
                               : set_component_runtime_error_fmt(
                                     error_buf, error_buf_size,
                                     "component import \"%s\" component export "
                                     "\"%s\" is missing component type metadata",
                                     import_name ? import_name : "<unnamed>",
                                     expected_export_name);

                if (!resolve_component_component_type(
                        expected_component,
                        export_decl->extern_desc->extern_desc.component.type_idx,
                        import_name, member_name, "expected", &expected_nested_type,
                        error_buf, error_buf_size)
                    || !resolve_component_component_type(
                        actual_component,
                        actual_export->extern_desc->extern_desc.component.type_idx,
                        import_name, member_name, "actual", &actual_nested_type,
                        error_buf, error_buf_size)
                    || !validate_component_type_against_type(
                        expected_component, expected_nested_type, actual_component,
                        actual_nested_type, import_name, member_name,
                        remaining_depth - 1, error_buf, error_buf_size))
                    return false;
                break;
            }
            default:
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    member_name
                        ? "component import \"%s\" instance export \"%s\" uses "
                          "an unsupported component type declaration"
                        : "component import \"%s\" uses an unsupported component "
                          "type declaration",
                    import_name ? import_name : "<unnamed>",
                    member_name ? member_name : "");
        }
    }

    return true;
}

static bool
validate_component_type_against_type(
    const WASMComponent *expected_component,
    const WASMComponentComponentType *expected_type,
    const WASMComponent *actual_component,
    const WASMComponentComponentType *actual_type, const char *import_name,
    const char *member_name, uint32 remaining_depth, char *error_buf,
    uint32 error_buf_size)
{
    uint32 i;

    if (!expected_component || !expected_type || !actual_component || !actual_type)
        return false;

    if (remaining_depth == 0)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            member_name ? "component import \"%s\" instance export \"%s\" "
                          "component type recursion is unsupported"
                        : "component import \"%s\" component type recursion is "
                          "unsupported",
            import_name ? import_name : "<unnamed>",
            member_name ? member_name : "");

    if (!validate_component_type_has_no_imports(expected_type, import_name,
                                                member_name, "expected", error_buf,
                                                error_buf_size)
        || !validate_component_type_has_no_imports(actual_type, import_name,
                                                   member_name, "actual",
                                                   error_buf, error_buf_size))
        return false;

    for (i = 0; i < expected_type->count; i++) {
        const WASMComponentComponentDecl *decl = &expected_type->component_decls[i];

        switch (decl->tag) {
            case WASM_COMP_COMPONENT_DECL_TYPE:
            case WASM_COMP_COMPONENT_DECL_ALIAS:
            case WASM_COMP_COMPONENT_DECL_IMPORT:
                break;
            case WASM_COMP_COMPONENT_DECL_EXPORT:
            {
                const WASMComponentInstDecl *expected_instance_decl =
                    decl->decl.instance_decl;
                const WASMComponentComponentDeclExport *expected_export_decl =
                    expected_instance_decl
                        ? expected_instance_decl->decl.export_decl
                        : NULL;
                const char *expected_export_name = expected_export_decl
                                                       ? get_component_export_name(
                                                             expected_export_decl
                                                                 ->export_name)
                                                       : NULL;
                const WASMComponentComponentDeclExport *actual_export_decl;
                const WASMComponentComponentType *expected_nested_type;
                const WASMComponentComponentType *actual_nested_type;

                if (!expected_instance_decl
                    || expected_instance_decl->tag
                           != WASM_COMP_COMPONENT_DECL_INSTANCE_EXPORTDECL
                    || !expected_export_decl
                    || !expected_export_decl->extern_desc
                    || expected_export_decl->extern_desc->type
                           != WASM_COMP_EXTERN_COMPONENT
                    || !expected_export_name)
                    return member_name
                               ? set_component_runtime_error_fmt(
                                     error_buf, error_buf_size,
                                     "component import \"%s\" instance export "
                                     "\"%s\" uses unsupported typed component "
                                     "export matching",
                                     import_name ? import_name : "<unnamed>",
                                     member_name)
                               : set_component_runtime_error_fmt(
                                     error_buf, error_buf_size,
                                     "component import \"%s\" uses unsupported "
                                     "typed component export matching",
                                     import_name ? import_name : "<unnamed>");

                actual_export_decl = lookup_component_type_export_decl(
                    actual_type, expected_export_name, WASM_COMP_EXTERN_COMPONENT);
                if (!actual_export_decl)
                    return member_name
                               ? set_component_runtime_error_fmt(
                                     error_buf, error_buf_size,
                                     "component import \"%s\" instance export "
                                     "\"%s\" expects component export \"%s\"",
                                     import_name ? import_name : "<unnamed>",
                                     member_name, expected_export_name)
                               : set_component_runtime_error_fmt(
                                     error_buf, error_buf_size,
                                     "component import \"%s\" expects component "
                                     "export \"%s\"",
                                     import_name ? import_name : "<unnamed>",
                                     expected_export_name);

                if (!resolve_component_component_type(
                        expected_component,
                        expected_export_decl->extern_desc->extern_desc.component
                            .type_idx,
                        import_name, member_name, "expected", &expected_nested_type,
                        error_buf, error_buf_size)
                    || !resolve_component_component_type(
                        actual_component,
                        actual_export_decl->extern_desc->extern_desc.component
                            .type_idx,
                        import_name, member_name, "actual", &actual_nested_type,
                        error_buf, error_buf_size)
                    || !validate_component_type_against_type(
                        expected_component, expected_nested_type, actual_component,
                        actual_nested_type, import_name, member_name,
                        remaining_depth - 1, error_buf, error_buf_size))
                    return false;
                break;
            }
            default:
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    member_name
                        ? "component import \"%s\" instance export \"%s\" uses "
                          "an unsupported component type declaration"
                        : "component import \"%s\" uses an unsupported component "
                          "type declaration",
                    import_name ? import_name : "<unnamed>",
                    member_name ? member_name : "");
        }
    }

    return true;
}

static bool
resolve_component_declared_func_type(const WASMComponent *component,
                                     uint32 type_idx, const char *import_name,
                                     const char *member_name,
                                     const char *role,
                                     const WASMComponentFuncType **out_type,
                                     char *error_buf, uint32 error_buf_size)
{
    const WASMComponentTypes *type_entry =
        wasm_component_lookup_type(component, type_idx);

    if (!type_entry)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            member_name
                ? "component import \"%s\" instance export \"%s\" uses "
                  "unresolved %s function type index %u"
                : "component import \"%s\" uses unresolved %s function type "
                  "index %u",
            import_name ? import_name : "<unnamed>",
            member_name ? member_name : "", role, type_idx);

    if (type_entry->tag != WASM_COMP_FUNC_TYPE || !type_entry->type.func_type)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            member_name
                ? "component import \"%s\" instance export \"%s\" %s function "
                  "type index %u is not a function"
                : "component import \"%s\" %s function type index %u is not a "
                  "function",
            import_name ? import_name : "<unnamed>",
            member_name ? member_name : "", role, type_idx);

    *out_type = type_entry->type.func_type;
    return true;
}

static bool
resolve_component_scalar_primitive_type(
    const WASMComponent *component, const WASMComponentValueType *value_type,
    const char *import_name, const char *member_name, const char *position,
    uint32 index, WASMComponentPrimValType *out_primitive_type, bool *supported,
    char *error_buf, uint32 error_buf_size)
{
    WASMComponentRuntimeValueType resolved_type;
    uint8 ignored_core_type = 0;
    wasm_valkind_t ignored_public_kind = WASM_I32;
    WASMComponentPrimValType primitive_type;

    if (supported)
        *supported = false;

    if (!component || !value_type || !out_primitive_type)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            member_name ? "component import \"%s\" instance export \"%s\" uses "
                          "unsupported typed function matching"
                        : "component import \"%s\" uses unsupported typed "
                          "function matching",
            import_name ? import_name : "<unnamed>",
            member_name ? member_name : "");

    memset(&resolved_type, 0, sizeof(resolved_type));
    if (!wasm_component_runtime_value_resolve_type(component, value_type,
                                                   &resolved_type, error_buf,
                                                   error_buf_size))
        return false;

    if (resolved_type.kind == WASM_COMP_RUNTIME_VALUE_TYPE_PRIMITIVE)
        primitive_type = resolved_type.type.primitive_type;
    else if (resolved_type.kind == WASM_COMP_RUNTIME_VALUE_TYPE_DEFINED
             && resolved_type.type.defined_type
             && resolved_type.type.defined_type->tag == WASM_COMP_DEF_VAL_PRIMVAL)
        primitive_type = resolved_type.type.defined_type->def_val.primval;
    else
        return true;

    if (!component_scalar_prim_to_core(primitive_type, &ignored_core_type,
                                       &ignored_public_kind))
        return true;

    *out_primitive_type = primitive_type;
    if (supported)
        *supported = true;
    return true;
}

static bool
is_supported_value_match_primitive(WASMComponentPrimValType primitive_type);

static bool
validate_component_value_types_equal(
    const WASMComponent *expected_component,
    const WASMComponentValueType *expected_value_type,
    const WASMComponent *actual_component,
    const WASMComponentValueType *actual_value_type, const char *import_name,
    const char *member_name, char *error_buf, uint32 error_buf_size);

static bool
component_value_type_is_u8_primitive(const WASMComponent *component,
                                     const WASMComponentValueType *value_type)
{
    const WASMComponentTypes *type_entry;
    const WASMComponentDefValType *def_type;

    if (!component || !value_type)
        return false;

    if (value_type->type == WASM_COMP_VAL_TYPE_PRIMVAL)
        return value_type->type_specific.primval_type == WASM_COMP_PRIMVAL_U8;

    type_entry =
        wasm_component_lookup_type(component, value_type->type_specific.type_idx);
    if (!type_entry || type_entry->tag != WASM_COMP_DEF_TYPE
        || !(def_type = type_entry->type.def_val_type))
        return false;

    return def_type->tag == WASM_COMP_DEF_VAL_PRIMVAL
           && def_type->def_val.primval == WASM_COMP_PRIMVAL_U8;
}

static bool
component_value_type_uses_supported_matching_subset(
    const WASMComponent *component, const WASMComponentValueType *value_type)
{
    const WASMComponentTypes *type_entry;
    const WASMComponentDefValType *def_type;

    if (!component || !value_type)
        return false;

    if (value_type->type == WASM_COMP_VAL_TYPE_PRIMVAL)
        return is_supported_value_match_primitive(
            value_type->type_specific.primval_type);

    type_entry =
        wasm_component_lookup_type(component, value_type->type_specific.type_idx);
    if (!type_entry || type_entry->tag != WASM_COMP_DEF_TYPE
        || !(def_type = type_entry->type.def_val_type))
        return false;

    switch (def_type->tag) {
        case WASM_COMP_DEF_VAL_PRIMVAL:
            return is_supported_value_match_primitive(def_type->def_val.primval);
        case WASM_COMP_DEF_VAL_LIST:
            return def_type->def_val.list && def_type->def_val.list->element_type
                   && component_value_type_is_u8_primitive(
                       component, def_type->def_val.list->element_type);
        case WASM_COMP_DEF_VAL_RECORD:
            if (!def_type->def_val.record)
                return false;
            for (uint32 i = 0; i < def_type->def_val.record->count; i++) {
                if (!component_value_type_uses_supported_matching_subset(
                        component,
                        def_type->def_val.record->fields[i].value_type))
                    return false;
            }
            return true;
        case WASM_COMP_DEF_VAL_TUPLE:
            if (!def_type->def_val.tuple)
                return false;
            for (uint32 i = 0; i < def_type->def_val.tuple->count; i++) {
                if (!component_value_type_uses_supported_matching_subset(
                        component,
                        &def_type->def_val.tuple->element_types[i]))
                    return false;
            }
            return true;
        default:
            return false;
    }
}

static bool
component_contains_nested_component(const WASMComponent *component,
                                    const WASMComponent *candidate)
{
    if (!component || !candidate)
        return false;

    for (uint32 i = 0; i < component->section_count; i++) {
        const WASMComponentSection *section = &component->sections[i];

        if (section->id != WASM_COMP_SECTION_COMPONENT || !section->parsed.component)
            continue;

        if (section->parsed.component == candidate
            || component_contains_nested_component(section->parsed.component,
                                                   candidate))
            return true;
    }

    return false;
}

static bool
component_func_type_uses_supported_scalar_matching(
    const WASMComponent *component, const WASMComponentFuncType *func_type,
    const char *import_name, const char *member_name, bool *supported,
    char *error_buf, uint32 error_buf_size)
{
    WASMComponentPrimValType ignored_primitive_type;
    bool value_supported = false;
    uint32 param_count;
    uint32 result_count;

    if (supported)
        *supported = false;

    if (!component || !func_type || !supported)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            member_name ? "component import \"%s\" instance export \"%s\" is "
                          "missing function type metadata"
                        : "component import \"%s\" is missing function type "
                          "metadata",
            import_name ? import_name : "<unnamed>",
            member_name ? member_name : "");

    param_count = func_type->params ? func_type->params->count : 0;
    result_count = get_component_func_result_count(func_type);

    if (result_count > 1)
        return true;

    for (uint32 i = 0; i < param_count; i++) {
        if (!resolve_component_scalar_primitive_type(
                component, func_type->params->params[i].value_type, import_name,
                member_name, "parameter", i, &ignored_primitive_type,
                &value_supported, error_buf, error_buf_size))
            return false;
        if (!value_supported)
            return true;
    }

    if (result_count == 1) {
        if (!resolve_component_scalar_primitive_type(
                component, func_type->results->results, import_name,
                member_name, "result", 0, &ignored_primitive_type,
                &value_supported, error_buf, error_buf_size))
            return false;
        if (!value_supported)
            return true;
    }

    *supported = true;
    return true;
}

static bool
component_func_type_uses_supported_value_matching(
    const WASMComponent *component, const WASMComponentFuncType *func_type,
    const char *import_name, const char *member_name, bool *supported,
    char *error_buf, uint32 error_buf_size)
{
    uint32 param_count;
    uint32 result_count;

    if (supported)
        *supported = false;

    if (!component || !func_type || !supported)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            member_name ? "component import \"%s\" instance export \"%s\" is "
                          "missing function type metadata"
                        : "component import \"%s\" is missing function type "
                          "metadata",
            import_name ? import_name : "<unnamed>",
            member_name ? member_name : "");

    param_count = func_type->params ? func_type->params->count : 0;
    result_count = get_component_func_result_count(func_type);

    if (result_count > 1)
        return true;

    for (uint32 i = 0; i < param_count; i++) {
        if (!component_value_type_uses_supported_matching_subset(
                component, func_type->params->params[i].value_type))
            return true;
    }

    if (result_count == 1
        && !component_value_type_uses_supported_matching_subset(
            component, func_type->results->results))
        return true;

    *supported = true;
    return true;
}

static bool
validate_component_func_types_equal(
    const WASMComponent *expected_component,
    const WASMComponentFuncType *expected_type,
    const WASMComponent *actual_component,
    const WASMComponentFuncType *actual_type, const char *import_name,
    const char *member_name, char *error_buf, uint32 error_buf_size)
{
    uint32 expected_param_count, actual_param_count;
    uint32 expected_result_count, actual_result_count;
    bool expected_supported = false, actual_supported = false;

    if (!expected_component || !expected_type || !actual_component || !actual_type)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            member_name ? "component import \"%s\" instance export \"%s\" is "
                          "missing function type metadata"
                        : "component import \"%s\" is missing function type "
                          "metadata",
            import_name ? import_name : "<unnamed>",
            member_name ? member_name : "");

    if (!component_func_type_uses_supported_value_matching(
            expected_component, expected_type, import_name, member_name,
            &expected_supported, error_buf, error_buf_size)
        || !component_func_type_uses_supported_value_matching(
            actual_component, actual_type, import_name, member_name,
            &actual_supported, error_buf, error_buf_size))
        return false;

    if (!expected_supported || !actual_supported)
        return true;

    expected_param_count =
        expected_type->params ? expected_type->params->count : 0;
    actual_param_count = actual_type->params ? actual_type->params->count : 0;
    expected_result_count = get_component_func_result_count(expected_type);
    actual_result_count = get_component_func_result_count(actual_type);

    if (expected_param_count != actual_param_count
        || expected_result_count != actual_result_count)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            member_name ? "component import \"%s\" instance export \"%s\" "
                          "function type mismatch"
                        : "component import \"%s\" function type mismatch",
            import_name ? import_name : "<unnamed>",
            member_name ? member_name : "");

    for (uint32 i = 0; i < expected_param_count; i++) {
        const char *expected_label =
            expected_type->params->params[i].label
                ? expected_type->params->params[i].label->name
                : NULL;
        const char *actual_label =
            actual_type->params->params[i].label
                ? actual_type->params->params[i].label->name
                : NULL;

        if (((expected_label || actual_label)
             && (!expected_label || !actual_label
                 || strcmp(expected_label, actual_label) != 0))
            || !validate_component_value_types_equal(
                expected_component, expected_type->params->params[i].value_type,
                actual_component, actual_type->params->params[i].value_type,
                import_name, member_name, error_buf, error_buf_size))
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                member_name ? "component import \"%s\" instance export \"%s\" "
                              "function type mismatch"
                            : "component import \"%s\" function type mismatch",
                import_name ? import_name : "<unnamed>",
                member_name ? member_name : "");
    }

    if (expected_result_count == 1) {
        if (!validate_component_value_types_equal(
                expected_component, expected_type->results->results,
                actual_component, actual_type->results->results, import_name,
                member_name, error_buf, error_buf_size))
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                member_name ? "component import \"%s\" instance export \"%s\" "
                              "function type mismatch"
                            : "component import \"%s\" function type mismatch",
                import_name ? import_name : "<unnamed>",
                member_name ? member_name : "");
    }

    return true;
}

static bool
validate_component_runtime_func_against_type(
    const WASMComponent *expected_component,
    const WASMComponentRuntimeFunc *runtime_func, uint32 expected_type_idx,
    const char *import_name, const char *member_name, char *error_buf,
    uint32 error_buf_size)
{
    const WASMComponentFuncType *expected_type, *actual_type;
    const WASMComponent *actual_component;
    bool internal_same_module_scope;

    if (!runtime_func || !runtime_func->owner_instance)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            member_name ? "component import \"%s\" instance export \"%s\" is "
                          "missing runtime function ownership"
                        : "component import \"%s\" is missing runtime function "
                          "ownership",
            import_name ? import_name : "<unnamed>",
            member_name ? member_name : "");

    actual_component = &runtime_func->owner_instance->module->component;

    if (!resolve_component_declared_func_type(expected_component, expected_type_idx,
                                              import_name, member_name,
                                              "expected", &expected_type,
                                              error_buf, error_buf_size)
        || !resolve_component_declared_func_type(actual_component,
                                                 runtime_func->type_idx,
                                                 import_name, member_name,
                                                  "actual", &actual_type, error_buf,
                                                  error_buf_size))
        return false;

    internal_same_module_scope =
        actual_component == expected_component
        || component_contains_nested_component(actual_component, expected_component)
        || component_contains_nested_component(expected_component, actual_component);

    if (internal_same_module_scope) {
        bool expected_supported = false, actual_supported = false;

        if (!component_func_type_uses_supported_value_matching(
                expected_component, expected_type, import_name, member_name,
                &expected_supported, error_buf, error_buf_size)
            || !component_func_type_uses_supported_value_matching(
                actual_component, actual_type, import_name, member_name,
                &actual_supported, error_buf, error_buf_size))
            return false;

        if (!expected_supported || !actual_supported)
            return true;
    }

    if (runtime_func->kind == WASM_COMP_RUNTIME_FUNC_LIFT
        && actual_component != expected_component) {
        bool expected_supported = false, actual_supported = false;

        if (!component_func_type_uses_supported_value_matching(
                expected_component, expected_type, import_name, member_name,
                &expected_supported, error_buf, error_buf_size)
            || !component_func_type_uses_supported_value_matching(
                actual_component, actual_type, import_name, member_name,
                &actual_supported, error_buf, error_buf_size))
            return false;

        if (!expected_supported || !actual_supported)
            return true;
    }

    return validate_component_func_types_equal(
        expected_component, expected_type, actual_component, actual_type,
        import_name, member_name, error_buf, error_buf_size);
}

static bool
resolve_component_runtime_value_primitive_type(
    const WASMComponentRuntimeValue *runtime_value,
    WASMComponentPrimValType *out_primitive_type)
{
    if (!runtime_value || !out_primitive_type)
        return false;

    if (runtime_value->type.kind == WASM_COMP_RUNTIME_VALUE_TYPE_PRIMITIVE) {
        *out_primitive_type = runtime_value->type.type.primitive_type;
        return true;
    }

    if (runtime_value->type.kind == WASM_COMP_RUNTIME_VALUE_TYPE_DEFINED
        && runtime_value->type.type.defined_type
        && runtime_value->type.type.defined_type->tag == WASM_COMP_DEF_VAL_PRIMVAL) {
        *out_primitive_type = runtime_value->type.type.defined_type->def_val.primval;
        return true;
    }

    return false;
}

static bool
is_supported_value_match_primitive(WASMComponentPrimValType primitive_type)
{
    uint8 ignored_core_type = 0;
    wasm_valkind_t ignored_public_kind = WASM_I32;

    return component_scalar_prim_to_core(primitive_type, &ignored_core_type,
                                         &ignored_public_kind)
           || primitive_type == WASM_COMP_PRIMVAL_STRING;
}

static bool
resolve_component_value_bound_type(
    const WASMComponentValueBound *value_bound,
    const WASMComponentValueType **out_value_type, const char *import_name,
    const char *member_name, char *error_buf, uint32 error_buf_size)
{
    if (out_value_type)
        *out_value_type = NULL;

    if (!value_bound || !out_value_type)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            member_name ? "component import \"%s\" instance export \"%s\" is "
                          "missing value type metadata"
                        : "component import \"%s\" is missing value type metadata",
            import_name ? import_name : "<unnamed>",
            member_name ? member_name : "");

    if (value_bound->tag != WASM_COMP_VALUEBOUND_TYPE
        || !value_bound->bound.value_type)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            member_name ? "component import \"%s\" instance export \"%s\" uses "
                          "unsupported typed value matching"
                        : "component import \"%s\" uses unsupported typed value "
                          "matching",
            import_name ? import_name : "<unnamed>",
            member_name ? member_name : "");

    *out_value_type = value_bound->bound.value_type;
    return true;
}

static bool
resolve_component_value_match_primitive(
    const WASMComponentRuntimeValueType *resolved_type,
    WASMComponentPrimValType *out_primitive_type)
{
    WASMComponentPrimValType primitive_type;

    if (!resolved_type || !out_primitive_type)
        return false;

    if (resolved_type->kind == WASM_COMP_RUNTIME_VALUE_TYPE_PRIMITIVE)
        primitive_type = resolved_type->type.primitive_type;
    else if (resolved_type->kind == WASM_COMP_RUNTIME_VALUE_TYPE_DEFINED
             && resolved_type->type.defined_type
             && resolved_type->type.defined_type->tag == WASM_COMP_DEF_VAL_PRIMVAL)
        primitive_type = resolved_type->type.defined_type->def_val.primval;
    else
        return false;

    if (!is_supported_value_match_primitive(primitive_type))
        return false;

    *out_primitive_type = primitive_type;
    return true;
}

static bool
validate_component_value_types_equal(
    const WASMComponent *expected_component,
    const WASMComponentValueType *expected_value_type,
    const WASMComponent *actual_component,
    const WASMComponentValueType *actual_value_type, const char *import_name,
    const char *member_name, char *error_buf, uint32 error_buf_size)
{
    WASMComponentRuntimeValueType expected_resolved_type, actual_resolved_type;
    WASMComponentPrimValType expected_primitive_type, actual_primitive_type;

    if (!expected_component || !expected_value_type || !actual_component
        || !actual_value_type)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            member_name ? "component import \"%s\" instance export \"%s\" is "
                          "missing value type metadata"
                        : "component import \"%s\" is missing value type metadata",
            import_name ? import_name : "<unnamed>",
            member_name ? member_name : "");

    memset(&expected_resolved_type, 0, sizeof(expected_resolved_type));
    memset(&actual_resolved_type, 0, sizeof(actual_resolved_type));

    if (!wasm_component_runtime_value_resolve_type(
            expected_component, expected_value_type, &expected_resolved_type,
            error_buf, error_buf_size)
        || !wasm_component_runtime_value_resolve_type(
            actual_component, actual_value_type, &actual_resolved_type, error_buf,
            error_buf_size))
        return false;

    if (resolve_component_value_match_primitive(&expected_resolved_type,
                                                &expected_primitive_type)
        || resolve_component_value_match_primitive(&actual_resolved_type,
                                                   &actual_primitive_type)) {
        if (!resolve_component_value_match_primitive(&expected_resolved_type,
                                                     &expected_primitive_type)
            || !resolve_component_value_match_primitive(&actual_resolved_type,
                                                        &actual_primitive_type))
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                member_name ? "component import \"%s\" instance export \"%s\" "
                              "value type mismatch"
                            : "component import \"%s\" value type mismatch",
                import_name ? import_name : "<unnamed>",
                member_name ? member_name : "");

        if (expected_primitive_type != actual_primitive_type)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                member_name ? "component import \"%s\" instance export \"%s\" "
                              "value type mismatch: expected %s but received %s"
                            : "component import \"%s\" value type mismatch: "
                              "expected %s but received %s",
                import_name ? import_name : "<unnamed>",
                member_name ? member_name : "",
                component_prim_type_name(expected_primitive_type),
                component_prim_type_name(actual_primitive_type));

        return true;
    }

    if (expected_resolved_type.kind != WASM_COMP_RUNTIME_VALUE_TYPE_DEFINED
        || actual_resolved_type.kind != WASM_COMP_RUNTIME_VALUE_TYPE_DEFINED
        || !expected_resolved_type.type.defined_type
        || !actual_resolved_type.type.defined_type)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            member_name ? "component import \"%s\" instance export \"%s\" uses "
                          "unsupported typed value matching"
                        : "component import \"%s\" uses unsupported typed value "
                          "matching",
            import_name ? import_name : "<unnamed>",
            member_name ? member_name : "");

    if (expected_resolved_type.type.defined_type->tag
        != actual_resolved_type.type.defined_type->tag)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            member_name ? "component import \"%s\" instance export \"%s\" value "
                          "type mismatch"
                        : "component import \"%s\" value type mismatch",
            import_name ? import_name : "<unnamed>",
            member_name ? member_name : "");

    switch (expected_resolved_type.type.defined_type->tag) {
        case WASM_COMP_DEF_VAL_LIST:
        {
            const WASMComponentValueType *expected_element_type;
            const WASMComponentValueType *actual_element_type;
            WASMComponentRuntimeValueType expected_element_resolved,
                actual_element_resolved;

            if (!expected_resolved_type.type.defined_type->def_val.list
                || !actual_resolved_type.type.defined_type->def_val.list)
                break;

            expected_element_type =
                expected_resolved_type.type.defined_type->def_val.list->element_type;
            actual_element_type =
                actual_resolved_type.type.defined_type->def_val.list->element_type;
            if (!expected_element_type || !actual_element_type)
                break;

            memset(&expected_element_resolved, 0, sizeof(expected_element_resolved));
            memset(&actual_element_resolved, 0, sizeof(actual_element_resolved));
            if (!wasm_component_runtime_value_resolve_type(
                    expected_component, expected_element_type,
                    &expected_element_resolved, error_buf, error_buf_size)
                || !wasm_component_runtime_value_resolve_type(
                    actual_component, actual_element_type, &actual_element_resolved,
                    error_buf, error_buf_size))
                return false;

            if (!resolve_component_value_match_primitive(&expected_element_resolved,
                                                         &expected_primitive_type)
                || !resolve_component_value_match_primitive(&actual_element_resolved,
                                                            &actual_primitive_type)
                || expected_primitive_type != WASM_COMP_PRIMVAL_U8
                || actual_primitive_type != WASM_COMP_PRIMVAL_U8)
                break;

            return true;
        }
        case WASM_COMP_DEF_VAL_RECORD:
        {
            const WASMComponentRecordType *expected_record =
                expected_resolved_type.type.defined_type->def_val.record;
            const WASMComponentRecordType *actual_record =
                actual_resolved_type.type.defined_type->def_val.record;

            if (!expected_record || !actual_record
                || expected_record->count != actual_record->count)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    member_name ? "component import \"%s\" instance export \"%s\" "
                                  "value type mismatch"
                                : "component import \"%s\" value type mismatch",
                    import_name ? import_name : "<unnamed>",
                    member_name ? member_name : "");

            for (uint32 i = 0; i < expected_record->count; i++) {
                const char *expected_name =
                    expected_record->fields[i].label
                    && expected_record->fields[i].label->name
                        ? expected_record->fields[i].label->name
                        : NULL;
                const char *actual_name =
                    actual_record->fields[i].label
                    && actual_record->fields[i].label->name
                        ? actual_record->fields[i].label->name
                        : NULL;

                if (!expected_name || !actual_name
                    || strcmp(expected_name, actual_name) != 0)
                    return set_component_runtime_error_fmt(
                        error_buf, error_buf_size,
                        member_name ? "component import \"%s\" instance export "
                                      "\"%s\" value type mismatch"
                                    : "component import \"%s\" value type mismatch",
                        import_name ? import_name : "<unnamed>",
                        member_name ? member_name : "");

                if (!validate_component_value_types_equal(
                        expected_component, expected_record->fields[i].value_type,
                        actual_component, actual_record->fields[i].value_type,
                        import_name, member_name, error_buf, error_buf_size))
                    return false;
            }
            return true;
        }
        case WASM_COMP_DEF_VAL_TUPLE:
        {
            const WASMComponentTupleType *expected_tuple =
                expected_resolved_type.type.defined_type->def_val.tuple;
            const WASMComponentTupleType *actual_tuple =
                actual_resolved_type.type.defined_type->def_val.tuple;

            if (!expected_tuple || !actual_tuple
                || expected_tuple->count != actual_tuple->count)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    member_name ? "component import \"%s\" instance export \"%s\" "
                                  "value type mismatch"
                                : "component import \"%s\" value type mismatch",
                    import_name ? import_name : "<unnamed>",
                    member_name ? member_name : "");

            for (uint32 i = 0; i < expected_tuple->count; i++) {
                if (!validate_component_value_types_equal(
                        expected_component, &expected_tuple->element_types[i],
                        actual_component, &actual_tuple->element_types[i],
                        import_name, member_name, error_buf, error_buf_size))
                    return false;
            }
            return true;
        }
        default:
            break;
    }

    return set_component_runtime_error_fmt(
        error_buf, error_buf_size,
        member_name ? "component import \"%s\" instance export \"%s\" uses "
                      "unsupported typed value matching"
                    : "component import \"%s\" uses unsupported typed value "
                      "matching",
        import_name ? import_name : "<unnamed>",
        member_name ? member_name : "");
}

static bool
validate_component_runtime_value_against_bound(
    const WASMComponent *component, const WASMComponentRuntimeValue *runtime_value,
    const WASMComponentValueBound *value_bound, const char *import_name,
    const char *member_name, char *error_buf, uint32 error_buf_size)
{
    const WASMComponentValueType *expected_value_type;

    if (!runtime_value || !runtime_value->owner_component
        || !runtime_value->type.declared_type)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            member_name ? "component import \"%s\" instance export \"%s\" is "
                          "missing value type metadata"
                        : "component import \"%s\" is missing value type metadata",
            import_name ? import_name : "<unnamed>",
            member_name ? member_name : "");

    if (!resolve_component_value_bound_type(value_bound, &expected_value_type,
                                            import_name, member_name, error_buf,
                                            error_buf_size))
        return false;

    return validate_component_value_types_equal(
        component, expected_value_type, runtime_value->owner_component,
        runtime_value->type.declared_type, import_name, member_name, error_buf,
        error_buf_size);
}

static bool
validate_component_instance_against_type(
    const WASMComponent *component,
    const WASMComponentRuntimeInstance *component_inst,
    const WASMComponentInstType *instance_type, const char *import_name,
    uint32 remaining_depth, char *error_buf, uint32 error_buf_size)
{
    uint32 i;

    if (!component || !component_inst || !instance_type)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component import \"%s\" is missing instance type metadata",
            import_name ? import_name : "<unnamed>");

    if (remaining_depth == 0)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component import \"%s\" instance type recursion is unsupported",
            import_name ? import_name : "<unnamed>");

    for (i = 0; i < instance_type->count; i++) {
        const WASMComponentInstDecl *decl = &instance_type->instance_decls[i];

        switch (decl->tag) {
            case WASM_COMP_COMPONENT_DECL_INSTANCE_CORE_TYPE:
            case WASM_COMP_COMPONENT_DECL_INSTANCE_TYPE:
            case WASM_COMP_COMPONENT_DECL_INSTANCE_ALIAS:
                break;
            case WASM_COMP_COMPONENT_DECL_INSTANCE_EXPORTDECL:
            {
                const WASMComponentComponentDeclExport *export_decl =
                    decl->decl.export_decl;
                const char *member_name =
                    export_decl && export_decl->export_name
                        ? export_decl->export_name->exported.simple.name->name
                        : NULL;
                WASMComponentRuntimeRef ref;

                if (!export_decl || !export_decl->extern_desc || !member_name)
                    return set_component_runtime_error_fmt(
                        error_buf, error_buf_size,
                        "component import \"%s\" has an invalid instance "
                        "export declaration",
                        import_name ? import_name : "<unnamed>");

                switch (export_decl->extern_desc->type) {
                    case WASM_COMP_EXTERN_FUNC:
                        if (!lookup_component_instance_export(
                                component_inst, member_name,
                                WASM_COMP_RUNTIME_REF_FUNC, &ref, error_buf,
                                error_buf_size))
                            return false;
                        if (!validate_component_runtime_func_against_type(
                                component, ref.of.function,
                                export_decl->extern_desc->extern_desc.func.type_idx,
                                import_name, member_name, error_buf,
                                error_buf_size))
                            return false;
                        break;
                    case WASM_COMP_EXTERN_CORE_MODULE:
                    {
                        uint32 core_type_idx =
                            export_decl->extern_desc->extern_desc.core_module
                                .type_idx;
                        const WASMComponentCoreType *core_type =
                            lookup_component_core_type(component, core_type_idx);
                        const WASMComponentCoreModuleType *module_type;

                        if (!lookup_component_instance_export(
                                component_inst, member_name,
                                WASM_COMP_RUNTIME_REF_CORE_MODULE, &ref, error_buf,
                                error_buf_size))
                            return false;

                        if (!core_type)
                            return set_component_runtime_error_fmt(
                                error_buf, error_buf_size,
                                "component import \"%s\" instance export \"%s\" "
                                "uses unresolved core module type index %u",
                                import_name, member_name, core_type_idx);

                        if (!core_type->deftype
                            || core_type->deftype->tag
                                   != WASM_CORE_DEFTYPE_MODULETYPE
                            || !(module_type = core_type->deftype->type.moduletype))
                            return set_component_runtime_error_fmt(
                                error_buf, error_buf_size,
                                "component import \"%s\" instance export \"%s\" "
                                "core module type index %u is not a module type",
                                import_name, member_name, core_type_idx);

                        if (!validate_core_module_against_type(
                                ref.of.core_module, module_type, import_name,
                                error_buf, error_buf_size))
                            return false;
                        break;
                    }
                    case WASM_COMP_EXTERN_INSTANCE:
                    {
                        const WASMComponentInstType *nested_instance_type;

                        if (!lookup_component_instance_export(
                                component_inst, member_name,
                                WASM_COMP_RUNTIME_REF_INSTANCE, &ref, error_buf,
                                error_buf_size)
                            || !resolve_component_instance_type(
                                component,
                                export_decl->extern_desc->extern_desc.instance
                                    .type_idx,
                                import_name, &nested_instance_type, error_buf,
                                error_buf_size)
                            || !validate_component_instance_against_type(
                                component, ref.of.instance, nested_instance_type,
                                import_name, remaining_depth - 1, error_buf,
                                error_buf_size))
                            return false;
                        break;
                    }
                    case WASM_COMP_EXTERN_VALUE:
                        if (!lookup_component_instance_export(
                                component_inst, member_name,
                                WASM_COMP_RUNTIME_REF_VALUE, &ref, error_buf,
                                error_buf_size))
                            return false;
                        if (!validate_component_runtime_value_against_bound(
                                component, ref.of.value,
                                export_decl->extern_desc->extern_desc.value.value_bound,
                                import_name, member_name, error_buf,
                                error_buf_size))
                            return false;
                        break;
                    case WASM_COMP_EXTERN_COMPONENT:
                    {
                        const WASMComponentComponentType *nested_component_type;

                        if (!lookup_component_instance_export(
                                component_inst, member_name,
                                WASM_COMP_RUNTIME_REF_COMPONENT, &ref, error_buf,
                                error_buf_size)
                            || !resolve_component_component_type(
                                component,
                                export_decl->extern_desc->extern_desc.component
                                    .type_idx,
                                import_name, member_name, "expected",
                                &nested_component_type, error_buf,
                                error_buf_size)
                            || !validate_runtime_component_against_type(
                                component, ref.of.component->component,
                                nested_component_type, import_name, member_name,
                                remaining_depth - 1, error_buf, error_buf_size))
                            return false;
                        break;
                    }
                    default:
                        return set_component_runtime_error_fmt(
                            error_buf, error_buf_size,
                            "component import \"%s\" instance export \"%s\" "
                            "uses an unsupported extern desc",
                            import_name, member_name);
                }
                break;
            }
            default:
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component import \"%s\" uses an unsupported instance "
                    "type declaration",
                    import_name ? import_name : "<unnamed>");
        }
    }

    return true;
}

static bool
validate_component_import_binding_type(const WASMComponent *component,
                                       const WASMComponentImport *component_import,
                                       WASMComponentRuntimeRef ref,
                                       char *error_buf,
                                       uint32 error_buf_size)
{
    const char *import_name =
        get_component_import_name(component_import->import_name);

    if (!component_import->extern_desc)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component import \"%s\" is missing an external descriptor",
            import_name);

    switch (component_import->extern_desc->type) {
        case WASM_COMP_EXTERN_FUNC:
            if (ref.type != WASM_COMP_RUNTIME_REF_FUNC)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component import \"%s\" bound to the wrong runtime sort",
                    import_name);
            if (find_component_type_count(component) == 0)
                return true;
            return validate_component_runtime_func_against_type(
                component, ref.of.function,
                component_import->extern_desc->extern_desc.func.type_idx,
                import_name, NULL, error_buf, error_buf_size);
        case WASM_COMP_EXTERN_VALUE:
            if (ref.type != WASM_COMP_RUNTIME_REF_VALUE)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component import \"%s\" bound to the wrong runtime sort",
                    import_name);
            return validate_component_runtime_value_against_bound(
                component, ref.of.value,
                component_import->extern_desc->extern_desc.value.value_bound,
                import_name, NULL, error_buf, error_buf_size);
        case WASM_COMP_EXTERN_CORE_MODULE:
            if (ref.type != WASM_COMP_RUNTIME_REF_CORE_MODULE)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component import \"%s\" bound to the wrong runtime sort",
                    import_name);

            if (find_component_core_type_count(component) == 0)
                return true;

            {
                uint32 type_idx =
                    component_import->extern_desc->extern_desc.core_module.type_idx;
                const WASMComponentCoreType *core_type =
                    lookup_component_core_type(component, type_idx);
                const WASMComponentCoreModuleType *module_type;

                if (!core_type)
                    return set_component_runtime_error_fmt(
                        error_buf, error_buf_size,
                        "component import \"%s\" uses unresolved core module "
                        "type index %u",
                        import_name, type_idx);

                if (!core_type->deftype
                    || core_type->deftype->tag != WASM_CORE_DEFTYPE_MODULETYPE
                    || !(module_type = core_type->deftype->type.moduletype))
                    return set_component_runtime_error_fmt(
                        error_buf, error_buf_size,
                        "component import \"%s\" core module type index %u is "
                        "not a module type",
                        import_name, type_idx);

                return validate_core_module_against_type(ref.of.core_module,
                                                         module_type, import_name,
                                                         error_buf,
                                                         error_buf_size);
            }
        case WASM_COMP_EXTERN_INSTANCE:
            if (ref.type != WASM_COMP_RUNTIME_REF_INSTANCE)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component import \"%s\" bound to the wrong runtime sort",
                    import_name);

            if (find_component_type_count(component) == 0)
                return true;

            {
                const WASMComponentInstType *instance_type;
                uint32 type_idx =
                    component_import->extern_desc->extern_desc.instance.type_idx;

                if (!resolve_component_instance_type(component, type_idx,
                                                     import_name, &instance_type,
                                                     error_buf, error_buf_size))
                    return false;

                return validate_component_instance_against_type(
                    component, ref.of.instance, instance_type,
                    import_name, find_component_type_count(component) + 1,
                    error_buf, error_buf_size);
            }
        case WASM_COMP_EXTERN_COMPONENT:
            if (ref.type != WASM_COMP_RUNTIME_REF_COMPONENT)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component import \"%s\" bound to the wrong runtime sort",
                    import_name);

            if (find_component_type_count(component) == 0)
                return true;

            {
                const WASMComponentComponentType *component_type;
                const WASMComponentTypes *type_entry;
                uint32 type_idx =
                    component_import->extern_desc->extern_desc.component.type_idx;

                type_entry = wasm_component_lookup_type(component, type_idx);
                if (!type_entry || type_entry->tag != WASM_COMP_COMPONENT_TYPE
                    || !type_entry->type.component_type)
                    return true;

                component_type = type_entry->type.component_type;

                return validate_runtime_component_against_type(
                    component, ref.of.component->component, component_type,
                    import_name, NULL, find_component_type_count(component) + 1,
                    error_buf, error_buf_size);
            }
        default:
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "nested component imports other than "
                "core_module/func/value/instance/component are not supported "
                "yet");
    }
}

static bool
count_component_imports(const WASMComponent *component, uint32 *import_count)
{
    uint32 i;

    *import_count = 0;
    for (i = 0; i < component->section_count; i++) {
        if (component->sections[i].id == WASM_COMP_SECTION_IMPORTS)
            *import_count += component->sections[i].parsed.import_section->count;
    }
    return true;
}

static const WASMComponentValueType *
resolve_component_import_value_type(const WASMComponentInstance *inst,
                                    const WASMComponentImport *component_import,
                                    char *error_buf, uint32 error_buf_size)
{
    const WASMComponentValueBound *value_bound;
    const char *import_name =
        get_component_import_name(component_import->import_name);

    if (!component_import->extern_desc
        || component_import->extern_desc->type != WASM_COMP_EXTERN_VALUE)
        return NULL;

    value_bound = component_import->extern_desc->extern_desc.value.value_bound;
    if (!value_bound) {
        set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component import \"%s\" is missing a value type", import_name);
        return NULL;
    }

    if (value_bound->tag == WASM_COMP_VALUEBOUND_TYPE)
        return value_bound->bound.value_type;

    if (value_bound->tag == WASM_COMP_VALUEBOUND_EQ) {
        if (value_bound->bound.value_idx >= inst->component_value_count) {
            set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "component import \"%s\" value eq index %u is out of bounds",
                import_name, value_bound->bound.value_idx);
            return NULL;
        }

        if (!inst->component_values[value_bound->bound.value_idx].type.declared_type) {
            set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "component import \"%s\" value eq index %u has no declared type",
                import_name, value_bound->bound.value_idx);
            return NULL;
        }

        return inst->component_values[value_bound->bound.value_idx]
            .type.declared_type;
    }

    set_component_runtime_error_fmt(
        error_buf, error_buf_size,
        "component import \"%s\" uses an unsupported value bound",
        import_name);
    return NULL;
}

static bool
component_import_binding_to_ref(const wasm_component_import_binding_t *binding,
                                WASMComponentRuntimeRef *out_ref,
                                char *error_buf, uint32 error_buf_size)
{
    memset(out_ref, 0, sizeof(*out_ref));

    switch (binding->kind) {
        case WASM_COMPONENT_EXTERN_KIND_FUNC:
            if (!binding->value.function)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component import binding \"%s\" is missing a function "
                    "handle",
                    binding->name ? binding->name : "<unnamed>");
            out_ref->type = WASM_COMP_RUNTIME_REF_FUNC;
            out_ref->of.function = binding->value.function;
            return true;
        case WASM_COMPONENT_EXTERN_KIND_INSTANCE:
            if (!binding->value.instance)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component import binding \"%s\" is missing an instance "
                    "handle",
                    binding->name ? binding->name : "<unnamed>");
            out_ref->type = WASM_COMP_RUNTIME_REF_INSTANCE;
            out_ref->of.instance = binding->value.instance;
            return true;
        case WASM_COMPONENT_EXTERN_KIND_COMPONENT:
            if (!binding->value.component)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component import binding \"%s\" is missing a component "
                    "handle",
                    binding->name ? binding->name : "<unnamed>");
            out_ref->type = WASM_COMP_RUNTIME_REF_COMPONENT;
            out_ref->of.component = binding->value.component;
            return true;
        case WASM_COMPONENT_EXTERN_KIND_CORE_MODULE:
            if (!binding->value.core_module)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component import binding \"%s\" is missing a core module "
                    "handle",
                    binding->name ? binding->name : "<unnamed>");
            out_ref->type = WASM_COMP_RUNTIME_REF_CORE_MODULE;
            out_ref->of.core_module = binding->value.core_module;
            return true;
        default:
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "component import binding \"%s\" uses an unsupported kind",
                binding->name ? binding->name : "<unnamed>");
    }
}

static bool
append_top_level_component_import(WASMComponentInstance *inst,
                                  WASMComponentRuntimeRef ref, char *error_buf,
                                  uint32 error_buf_size)
{
    switch (ref.type) {
        case WASM_COMP_RUNTIME_REF_FUNC:
            inst->component_funcs[inst->component_func_count++] =
                *ref.of.function;
            return true;
        case WASM_COMP_RUNTIME_REF_VALUE:
            return clone_component_runtime_value_borrowed(
                &inst->component_values[inst->component_value_count++],
                ref.of.value, error_buf, error_buf_size);
        case WASM_COMP_RUNTIME_REF_INSTANCE:
        {
            WASMComponentRuntimeInstance *runtime_inst =
                &inst->component_instances[inst->component_instance_count++];

            memset(runtime_inst, 0, sizeof(*runtime_inst));
            runtime_inst->owns_exports = false;
            runtime_inst->owns_resource_state = false;
            runtime_inst->export_count = ref.of.instance->export_count;
            runtime_inst->exports = ref.of.instance->exports;
            runtime_inst->resource_state = ref.of.instance->resource_state;
            return true;
        }
        case WASM_COMP_RUNTIME_REF_COMPONENT:
            inst->components[inst->component_count++] = *ref.of.component;
            inst->components[inst->component_count - 1].owns_scope = false;
            return true;
        case WASM_COMP_RUNTIME_REF_CORE_MODULE:
            inst->core_modules[inst->core_module_count++] = ref.of.core_module;
            return true;
        default:
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "unsupported top-level component import runtime sort");
    }
}

static bool
append_top_level_component_import_value(
    WASMComponentInstance *inst, const WASMComponentImport *component_import,
    const wasm_component_import_binding_t *binding, char *error_buf,
    uint32 error_buf_size)
{
    const WASMComponentValueType *value_type;

    if (!binding->value.value)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component import binding \"%s\" is missing a value handle",
            binding->name ? binding->name : "<unnamed>");

    value_type = resolve_component_import_value_type(inst, component_import,
                                                     error_buf, error_buf_size);
    if (!value_type)
        return false;

    if (!wasm_component_runtime_value_init_public(
            &inst->component_values[inst->component_value_count],
            &inst->module->component, value_type, binding->value.value,
            error_buf, error_buf_size))
        return false;

    inst->component_value_count++;
    return true;
}

static bool
append_top_level_component_host_import(
    WASMComponentInstance *inst, const WASMComponentImport *component_import,
    const wasm_component_func_import_binding_t *binding, char *error_buf,
    uint32 error_buf_size)
{
    WASMComponentRuntimeFunc *function;
    const char *import_name =
        get_component_import_name(component_import->import_name);

    if (!binding->callback)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component function import binding \"%s\" is missing a callback",
            binding->name ? binding->name : "<unnamed>");

    if (!component_import->extern_desc
        || component_import->extern_desc->type != WASM_COMP_EXTERN_FUNC)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component import \"%s\" is not a function import", import_name);

    function = &inst->component_funcs[inst->component_func_count];
    memset(function, 0, sizeof(*function));
    function->kind = WASM_COMP_RUNTIME_FUNC_HOST_IMPORT;
    function->type_idx = component_import->extern_desc->extern_desc.func.type_idx;
    function->owner_instance = inst;
    function->host_callback = binding->callback;
    function->host_user_data = binding->user_data;

    if (!validate_component_host_import_func_type(inst, function, import_name,
                                                  error_buf, error_buf_size))
        return false;

    inst->component_func_count++;
    return true;
}

static bool
resolve_top_level_component_imports(
    WASMComponentInstance *inst, const WASMComponentImportSection *import_section,
    const struct InstantiationArgs2 *args, char *error_buf,
    uint32 error_buf_size)
{
    uint32 j;

    for (j = 0; j < import_section->count; j++) {
        const WASMComponentImport *component_import = &import_section->imports[j];
        const char *import_name =
            get_component_import_name(component_import->import_name);
        WASMComponentRuntimeRef ref;
        bool matched = false;
        uint32 k;

        if (args && args->component_imports) {
            for (k = 0; k < args->component_import_count; k++) {
                const wasm_component_import_binding_t *binding =
                    &args->component_imports[k];

                if (!binding->name || strcmp(binding->name, import_name))
                    continue;

                if (binding->kind == WASM_COMPONENT_EXTERN_KIND_VALUE) {
                    if (!component_import->extern_desc
                        || component_import->extern_desc->type
                               != WASM_COMP_EXTERN_VALUE)
                        return set_component_runtime_error_fmt(
                            error_buf, error_buf_size,
                            "component import \"%s\" bound to the wrong runtime "
                            "sort",
                            import_name);
                    if (!append_top_level_component_import_value(
                            inst, component_import, binding, error_buf,
                            error_buf_size))
                        return false;
                }
                else if (!component_import_binding_to_ref(binding, &ref, error_buf,
                                                          error_buf_size)
                         || !validate_component_import_binding_type(
                             &inst->module->component, component_import, ref,
                             error_buf, error_buf_size)
                         || !append_top_level_component_import(
                             inst, ref, error_buf, error_buf_size))
                    return false;

                matched = true;
                break;
            }
        }

        if (!matched && component_import->extern_desc
            && component_import->extern_desc->type == WASM_COMP_EXTERN_FUNC
            && args && args->component_func_imports) {
            for (k = 0; k < args->component_func_import_count; k++) {
                const wasm_component_func_import_binding_t *binding =
                    &args->component_func_imports[k];

                if (!binding->name || strcmp(binding->name, import_name))
                    continue;

                if (!append_top_level_component_host_import(
                        inst, component_import, binding, error_buf,
                        error_buf_size))
                    return false;

                matched = true;
                break;
            }
        }

        if (!matched)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "top-level component import \"%s\" is missing a binding",
                import_name);
    }

    return true;
}

static bool
resolve_component_import_bindings_from_top_level(
    const WASMComponentInstance *inst, const WASMComponent *component,
    const WASMComponentInst *component_inst, WASMComponentRuntimeRef **out_refs,
    uint32 *out_ref_count, char *error_buf, uint32 error_buf_size)
{
    uint32 i, import_index = 0, import_count = 0;
    WASMComponentRuntimeRef *resolved_imports = NULL;

    if (!count_component_imports(component, &import_count))
        return false;

    if (component_inst->expression.with_args.arg_len != import_count)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "nested component import binding count mismatch");

    if (!alloc_component_runtime_array((void **)&resolved_imports, import_count,
                                       sizeof(*resolved_imports), error_buf,
                                       error_buf_size))
        return false;

    for (i = 0; i < component->section_count; i++) {
        const WASMComponentSection *section = &component->sections[i];
        uint32 j;

        if (section->id != WASM_COMP_SECTION_IMPORTS)
            continue;

        for (j = 0; j < section->parsed.import_section->count; j++) {
            const WASMComponentImport *component_import =
                &section->parsed.import_section->imports[j];
            const char *import_name =
                get_component_import_name(component_import->import_name);
            bool matched = false;
            uint32 k;

            for (k = 0; k < component_inst->expression.with_args.arg_len; k++) {
                const WASMComponentInstArg *arg =
                    &component_inst->expression.with_args.args[k];

                if (strcmp(arg->name->name, import_name))
                    continue;

                if (!resolve_component_sort_idx(inst, arg->idx.sort_idx,
                                                &resolved_imports[import_index],
                                                error_buf, error_buf_size)
                    || !validate_component_import_binding_type(
                        component, component_import, resolved_imports[import_index],
                        error_buf, error_buf_size)) {
                    wasm_runtime_free(resolved_imports);
                    return false;
                }

                matched = true;
                break;
            }

            if (!matched) {
                wasm_runtime_free(resolved_imports);
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "nested component import \"%s\" is missing a binding",
                    import_name);
            }

            import_index++;
        }
    }

    *out_refs = resolved_imports;
    *out_ref_count = import_count;
    return true;
}

static bool
resolve_component_import_bindings_from_nested(
    const WASMNestedComponentLocalBindings *bindings,
    const WASMComponent *component, const WASMComponentInst *component_inst,
    WASMComponentRuntimeRef **out_refs, uint32 *out_ref_count, char *error_buf,
    uint32 error_buf_size)
{
    uint32 i, import_index = 0, import_count = 0;
    WASMComponentRuntimeRef *resolved_imports = NULL;

    if (!count_component_imports(component, &import_count))
        return false;

    if (component_inst->expression.with_args.arg_len != import_count)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "nested component import binding count mismatch");

    if (!alloc_component_runtime_array((void **)&resolved_imports, import_count,
                                       sizeof(*resolved_imports), error_buf,
                                       error_buf_size))
        return false;

    for (i = 0; i < component->section_count; i++) {
        const WASMComponentSection *section = &component->sections[i];
        uint32 j;

        if (section->id != WASM_COMP_SECTION_IMPORTS)
            continue;

        for (j = 0; j < section->parsed.import_section->count; j++) {
            const WASMComponentImport *component_import =
                &section->parsed.import_section->imports[j];
            const char *import_name =
                get_component_import_name(component_import->import_name);
            bool matched = false;
            uint32 k;

            for (k = 0; k < component_inst->expression.with_args.arg_len; k++) {
                const WASMComponentInstArg *arg =
                    &component_inst->expression.with_args.args[k];

                if (strcmp(arg->name->name, import_name))
                    continue;

                if (!resolve_nested_component_local_sort_idx(
                        bindings, arg->idx.sort_idx,
                        &resolved_imports[import_index], error_buf,
                        error_buf_size)
                    || !validate_component_import_binding_type(
                        component, component_import, resolved_imports[import_index],
                        error_buf, error_buf_size)) {
                    wasm_runtime_free(resolved_imports);
                    return false;
                }

                matched = true;
                break;
            }

            if (!matched) {
                wasm_runtime_free(resolved_imports);
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "nested component import \"%s\" is missing a binding",
                    import_name);
            }

            import_index++;
        }
    }

    *out_refs = resolved_imports;
    *out_ref_count = import_count;
    return true;
}

static bool
build_component_runtime_instance_from_component(
    WASMComponentInstance *inst, WASMComponentRuntimeInstance *runtime_inst,
    const WASMComponent *component, WASMComponentRuntimeScope *parent_scope,
    const WASMComponentRuntimeRef *resolved_imports, uint32 resolved_import_count,
    char *error_buf, uint32 error_buf_size);

static bool
instantiate_nested_component_inline_instance(
    WASMComponentRuntimeInstance *runtime_inst,
    WASMNestedComponentLocalBindings *bindings,
    const WASMComponentInst *component_inst, char *error_buf,
    uint32 error_buf_size)
{
    uint32 i;
    WASMComponentRuntimeRef ref;
    WASMComponentRuntimeInstance *child_inst =
        &runtime_inst->owned_instances[runtime_inst->owned_instance_count];

    if (component_inst->instance_expression_tag
        != WASM_COMP_INSTANCE_EXPRESSION_WITHOUT_ARGS)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "nested component instance expressions other than inline exports "
            "are not supported yet");

    memset(child_inst, 0, sizeof(*child_inst));
    runtime_inst->owned_instance_count++;
    child_inst->resource_state =
        wasm_component_resource_state_create(NULL, error_buf, error_buf_size);
    if (!child_inst->resource_state)
        goto fail;
    child_inst->owns_resource_state = true;

    if (!alloc_component_runtime_array(
            (void **)&child_inst->exports,
            component_inst->expression.without_args.inline_expr_len,
            sizeof(*child_inst->exports), error_buf, error_buf_size))
        goto fail;

    child_inst->owns_exports = true;
    child_inst->export_count =
        component_inst->expression.without_args.inline_expr_len;
    for (i = 0; i < child_inst->export_count; i++) {
        const WASMComponentInlineExport *inline_export =
            &component_inst->expression.without_args.inline_expr[i];

        child_inst->exports[i].name = inline_export->name->name;
        if (!resolve_nested_component_local_sort_idx(
                bindings, inline_export->sort_idx, &child_inst->exports[i].ref,
                error_buf, error_buf_size))
            goto fail;
    }

    memset(&ref, 0, sizeof(ref));
    ref.type = WASM_COMP_RUNTIME_REF_INSTANCE;
    ref.of.instance = child_inst;
    return append_nested_component_local_ref(bindings, ref, error_buf,
                                             error_buf_size);

fail:
    destroy_component_runtime_instance(child_inst);
    runtime_inst->owned_instance_count--;
    return false;
}

static bool
resolve_nested_component_alias_section(
    WASMNestedComponentLocalBindings *bindings,
    const WASMComponentAliasSection *alias_section, char *error_buf,
    uint32 error_buf_size)
{
    uint32 i;

    for (i = 0; i < alias_section->count; i++) {
        const WASMComponentAliasDefinition *alias_def =
            &alias_section->aliases[i];
        const WASMComponentRuntimeInstance *component_instance;
        WASMComponentRuntimeComponent *component_ref = NULL;
        WASMComponentRuntimeRef ref;
        WASMComponentRuntimeRefType expected_type;
        const char *name;

        memset(&ref, 0, sizeof(ref));

        if (alias_def->alias_target_type == WASM_COMP_ALIAS_TARGET_OUTER) {
            if (!alias_def->sort
                || alias_def->sort->sort != WASM_COMP_SORT_COMPONENT)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "nested outer aliases currently support only component "
                    "sort");

            if (!resolve_outer_component_alias(
                    bindings->parent_scope, alias_def->target.outer.ct,
                    alias_def->target.outer.idx, &component_ref, error_buf,
                    error_buf_size)
                || !append_nested_component_local_component(
                    bindings, component_ref, error_buf, error_buf_size))
                return false;
            continue;
        }

        if (alias_def->alias_target_type != WASM_COMP_ALIAS_TARGET_EXPORT)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "nested component aliases other than export are not "
                "supported yet");

        if (!alias_def->sort || alias_def->sort->sort == WASM_COMP_SORT_CORE_SORT)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "nested component aliases must use component "
                "func/value/instance/component sorts");

        if (alias_def->target.exported.instance_idx >= bindings->instance_count)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "nested component alias instance index %u is out of bounds",
                alias_def->target.exported.instance_idx);

        switch (alias_def->sort->sort) {
            case WASM_COMP_SORT_FUNC:
                expected_type = WASM_COMP_RUNTIME_REF_FUNC;
                break;
            case WASM_COMP_SORT_VALUE:
                expected_type = WASM_COMP_RUNTIME_REF_VALUE;
                break;
            case WASM_COMP_SORT_INSTANCE:
                expected_type = WASM_COMP_RUNTIME_REF_INSTANCE;
                break;
            case WASM_COMP_SORT_COMPONENT:
                expected_type = WASM_COMP_RUNTIME_REF_COMPONENT;
                break;
            default:
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "nested component alias sort 0x%02x is not supported yet",
                    (unsigned)alias_def->sort->sort);
        }

        component_instance =
            bindings->instances[alias_def->target.exported.instance_idx]
                .of.instance;
        name = alias_def->target.exported.name->name;
        if (!lookup_component_instance_export(component_instance, name,
                                              expected_type, &ref, error_buf,
                                              error_buf_size))
            return false;

        if (alias_def->sort->sort == WASM_COMP_SORT_COMPONENT) {
            if (!append_nested_component_local_component(bindings, ref.of.component,
                                                         error_buf,
                                                         error_buf_size))
                return false;
        }
        else if (!append_nested_component_local_ref(bindings, ref, error_buf,
                                                    error_buf_size))
            return false;
    }

    return true;
}

static bool
instantiate_nested_component_component_instance(
    WASMComponentInstance *inst, WASMComponentRuntimeInstance *runtime_inst,
    WASMNestedComponentLocalBindings *bindings,
    const WASMComponentInst *component_inst, char *error_buf,
    uint32 error_buf_size)
{
    WASMComponentRuntimeComponent *nested_component = NULL;
    WASMComponentRuntimeRef *resolved_imports = NULL;
    WASMComponentRuntimeRef ref;
    uint32 import_count = 0;
    WASMComponentRuntimeInstance *child_inst =
        &runtime_inst->owned_instances[runtime_inst->owned_instance_count];

    if (component_inst->instance_expression_tag
        != WASM_COMP_INSTANCE_EXPRESSION_WITH_ARGS)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "nested component instance expressions other than instantiate are "
            "not supported here");

    if (!resolve_nested_component_local_component_idx(
            bindings, component_inst->expression.with_args.idx, &nested_component,
            error_buf, error_buf_size))
        return false;

    memset(child_inst, 0, sizeof(*child_inst));
    runtime_inst->owned_instance_count++;
    if (!resolve_component_import_bindings_from_nested(
            bindings, nested_component->component, component_inst, &resolved_imports,
            &import_count, error_buf, error_buf_size)
        || !build_component_runtime_instance_from_component(
            inst, child_inst, nested_component->component, nested_component->scope,
            resolved_imports, import_count, error_buf, error_buf_size))
        goto fail;

    wasm_runtime_free(resolved_imports);
    memset(&ref, 0, sizeof(ref));
    ref.type = WASM_COMP_RUNTIME_REF_INSTANCE;
    ref.of.instance = child_inst;
    return append_nested_component_local_ref(bindings, ref, error_buf,
                                             error_buf_size);

fail:
    if (resolved_imports)
        wasm_runtime_free(resolved_imports);
    destroy_component_runtime_instance(child_inst);
    runtime_inst->owned_instance_count--;
    return false;
}

static bool
build_component_runtime_instance_from_component(
    WASMComponentInstance *inst, WASMComponentRuntimeInstance *runtime_inst,
    const WASMComponent *component, WASMComponentRuntimeScope *parent_scope,
    const WASMComponentRuntimeRef *resolved_imports, uint32 resolved_import_count,
    char *error_buf, uint32 error_buf_size)
{
    WASMNestedComponentLocalBindings bindings;
    uint32 i, import_index = 0, import_count = 0, owned_component_count = 0,
              owned_func_count = 0, owned_value_count = 0,
              owned_core_instance_count = 0,
              owned_lowered_func_count = 0, owned_instance_count = 0,
              export_count = 0;

    memset(&bindings, 0, sizeof(bindings));
    runtime_inst->resource_state = wasm_component_resource_state_create(
        component, error_buf, error_buf_size);
    if (!runtime_inst->resource_state)
        return false;
    runtime_inst->owns_resource_state = true;

    if (!count_nested_component_local_bindings(
            component, &import_count, &bindings.core_module_capacity,
            &bindings.core_instance_capacity, &bindings.core_func_capacity,
            &bindings.func_capacity, &bindings.value_capacity,
            &bindings.instance_capacity, &bindings.component_capacity,
            &owned_func_count, &owned_value_count, &owned_component_count,
            &owned_core_instance_count, &owned_lowered_func_count,
            &owned_instance_count, &export_count,
            error_buf, error_buf_size))
        return false;

    if (resolved_import_count != import_count)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "nested component import binding count mismatch");

    if (!alloc_nested_component_local_bindings(
            &bindings, bindings.core_module_capacity,
            bindings.core_instance_capacity, bindings.core_func_capacity,
            bindings.func_capacity, bindings.value_capacity,
            bindings.instance_capacity, bindings.component_capacity, error_buf,
            error_buf_size)
        || !alloc_component_runtime_array(
            (void **)&runtime_inst->owned_funcs, owned_func_count,
            sizeof(*runtime_inst->owned_funcs), error_buf, error_buf_size)
        || !alloc_component_runtime_array(
            (void **)&runtime_inst->owned_values, owned_value_count,
            sizeof(*runtime_inst->owned_values), error_buf, error_buf_size)
        || !alloc_component_runtime_array(
            (void **)&runtime_inst->owned_core_instances,
            owned_core_instance_count,
            sizeof(*runtime_inst->owned_core_instances), error_buf,
            error_buf_size)
        || !alloc_component_runtime_array(
            (void **)&runtime_inst->owned_lowered_funcs,
            owned_lowered_func_count,
            sizeof(*runtime_inst->owned_lowered_funcs), error_buf,
            error_buf_size)
        || !alloc_component_runtime_array(
            (void **)&runtime_inst->owned_components, owned_component_count,
            sizeof(*runtime_inst->owned_components), error_buf, error_buf_size)
        || !alloc_component_runtime_array(
            (void **)&runtime_inst->owned_instances, owned_instance_count,
            sizeof(*runtime_inst->owned_instances), error_buf, error_buf_size)
        || !alloc_component_runtime_array((void **)&runtime_inst->exports,
                                          export_count,
                                          sizeof(*runtime_inst->exports),
                                          error_buf, error_buf_size)) {
        free_nested_component_local_bindings(&bindings);
        return false;
    }
    bindings.parent_scope = parent_scope;

    for (i = 0; i < component->section_count; i++) {
        const WASMComponentSection *section = &component->sections[i];

        switch (section->id) {
            case WASM_COMP_SECTION_CORE_CUSTOM:
            case WASM_COMP_SECTION_TYPE:
            case WASM_COMP_SECTION_CORE_TYPE:
                break;
            case WASM_COMP_SECTION_CORE_MODULE:
                if (!section->parsed.core_module
                    || !section->parsed.core_module->module_handle
                    || !append_nested_component_local_core_module(
                        &bindings,
                        (wasm_module_t)section->parsed.core_module->module_handle,
                        error_buf, error_buf_size))
                    goto fail;
                break;
            case WASM_COMP_SECTION_CORE_INSTANCE:
            {
                uint32 j;
                const WASMComponentCoreInstSection *core_inst_section =
                    section->parsed.core_instance_section;

                for (j = 0; j < core_inst_section->count; j++) {
                    if (!instantiate_nested_component_core_instance(
                            runtime_inst, &bindings,
                            &core_inst_section->instances[j], error_buf,
                            error_buf_size))
                        goto fail;
                }
                break;
            }
            case WASM_COMP_SECTION_CANONS:
            {
                uint32 j;
                const WASMComponentCanonSection *canon_section =
                    section->parsed.canon_section;

                for (j = 0; j < canon_section->count; j++) {
                    if (!append_nested_component_canon(
                            inst, runtime_inst, &bindings,
                            &canon_section->canons[j], error_buf,
                            error_buf_size))
                        goto fail;
                }
                break;
            }
            case WASM_COMP_SECTION_COMPONENT:
                if (!append_nested_component_definition(
                        runtime_inst, &bindings, section->parsed.component,
                        error_buf,
                        error_buf_size))
                    goto fail;
                break;
            case WASM_COMP_SECTION_IMPORTS:
            {
                uint32 j;
                const WASMComponentImportSection *import_section =
                    section->parsed.import_section;

                for (j = 0; j < import_section->count; j++) {
                    const WASMComponentImport *component_import =
                        &import_section->imports[j];

                    if (import_index >= resolved_import_count
                        || !validate_component_import_binding_type(
                            component, component_import,
                            resolved_imports[import_index],
                            error_buf, error_buf_size)
                        || !append_nested_component_local_ref(
                            &bindings, resolved_imports[import_index],
                            error_buf, error_buf_size))
                        goto fail;

                    import_index++;
                }
                break;
            }
            case WASM_COMP_SECTION_ALIASES:
                if (!resolve_nested_component_alias_section(
                        &bindings, section->parsed.alias_section, error_buf,
                        error_buf_size))
                    goto fail;
                break;
            case WASM_COMP_SECTION_INSTANCES:
            {
                uint32 j;
                const WASMComponentInstSection *inst_section =
                    section->parsed.instance_section;

                for (j = 0; j < inst_section->count; j++) {
                    const WASMComponentInst *nested_inst =
                        &inst_section->instances[j];

                    if (nested_inst->instance_expression_tag
                        == WASM_COMP_INSTANCE_EXPRESSION_WITHOUT_ARGS) {
                        if (!instantiate_nested_component_inline_instance(
                                runtime_inst, &bindings, nested_inst, error_buf,
                                error_buf_size))
                            goto fail;
                    }
                    else if (nested_inst->instance_expression_tag
                             == WASM_COMP_INSTANCE_EXPRESSION_WITH_ARGS) {
                        if (!instantiate_nested_component_component_instance(
                                inst, runtime_inst, &bindings, nested_inst,
                                error_buf, error_buf_size))
                            goto fail;
                    }
                    else {
                        goto fail;
                    }
                }
                break;
            }
            case WASM_COMP_SECTION_EXPORTS:
                if (!resolve_nested_component_exports(
                        runtime_inst, section->parsed.export_section, &bindings,
                        error_buf, error_buf_size))
                    goto fail;
                break;
            case WASM_COMP_SECTION_START:
                if (!instantiate_nested_component_start_section(
                        inst, runtime_inst, &bindings, section->parsed.start_section,
                        error_buf,
                        error_buf_size))
                    goto fail;
                break;
            case WASM_COMP_SECTION_VALUES:
                if (!instantiate_nested_component_value_section(
                        runtime_inst, &bindings, component,
                        section->parsed.value_section, error_buf,
                        error_buf_size))
                    goto fail;
                break;
            default:
                goto fail;
        }
    }

    free_nested_component_local_bindings(&bindings);
    return true;

fail:
    free_nested_component_local_bindings(&bindings);
    destroy_component_runtime_instance(runtime_inst);
    return false;
}

static bool
instantiate_nested_component_instance(WASMComponentInstance *inst,
                                      const WASMComponentInst *component_inst,
                                      char *error_buf, uint32 error_buf_size)
{
    WASMComponentRuntimeInstance *runtime_inst =
        &inst->component_instances[inst->component_instance_count];
    WASMComponentRuntimeComponent *nested_component;
    WASMComponentRuntimeRef *resolved_imports = NULL;
    uint32 import_count = 0;

    if (component_inst->expression.with_args.idx >= inst->component_count)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "nested component index %u is out of bounds",
            component_inst->expression.with_args.idx);

    nested_component = &inst->components[component_inst->expression.with_args.idx];
    memset(runtime_inst, 0, sizeof(*runtime_inst));
    if (!resolve_component_import_bindings_from_top_level(
            inst, nested_component->component, component_inst, &resolved_imports,
            &import_count, error_buf, error_buf_size)
        || !build_component_runtime_instance_from_component(
            inst, runtime_inst, nested_component->component,
            nested_component->scope,
            resolved_imports, import_count, error_buf, error_buf_size)) {
        if (resolved_imports)
            wasm_runtime_free(resolved_imports);
        return false;
    }

    wasm_runtime_free(resolved_imports);
    inst->component_instance_count++;
    return true;
}

static bool
instantiate_component_runtime_instance(WASMComponentInstance *inst,
                                       const WASMComponentInst *component_inst,
                                       char *error_buf, uint32 error_buf_size)
{
    WASMComponentRuntimeInstance *runtime_inst =
        &inst->component_instances[inst->component_instance_count];

    memset(runtime_inst, 0, sizeof(*runtime_inst));

    if (component_inst->instance_expression_tag
        == WASM_COMP_INSTANCE_EXPRESSION_WITH_ARGS)
        return instantiate_nested_component_instance(inst, component_inst,
                                                     error_buf, error_buf_size);

    if (component_inst->instance_expression_tag
        == WASM_COMP_INSTANCE_EXPRESSION_WITHOUT_ARGS) {
        uint32 i;
        uint32 export_count =
            component_inst->expression.without_args.inline_expr_len;

        if (!alloc_component_runtime_array((void **)&runtime_inst->exports,
                                           export_count,
                                           sizeof(*runtime_inst->exports),
                                           error_buf, error_buf_size))
            return false;

        runtime_inst->owns_exports = true;
        runtime_inst->resource_state =
            wasm_component_resource_state_create(NULL, error_buf, error_buf_size);
        if (!runtime_inst->resource_state) {
            wasm_runtime_free(runtime_inst->exports);
            runtime_inst->exports = NULL;
            runtime_inst->owns_exports = false;
            return false;
        }
        runtime_inst->owns_resource_state = true;
        runtime_inst->export_count = export_count;
        for (i = 0; i < export_count; i++) {
            const WASMComponentInlineExport *inline_export =
                &component_inst->expression.without_args.inline_expr[i];

            runtime_inst->exports[i].name = inline_export->name->name;
            if (!resolve_component_sort_idx(inst, inline_export->sort_idx,
                                            &runtime_inst->exports[i].ref,
                                            error_buf, error_buf_size)) {
                wasm_runtime_free(runtime_inst->exports);
                runtime_inst->exports = NULL;
                wasm_component_resource_state_destroy(runtime_inst->resource_state);
                runtime_inst->resource_state = NULL;
                runtime_inst->owns_resource_state = false;
                runtime_inst->export_count = 0;
                return false;
            }
        }

        inst->component_instance_count++;
        return true;
    }

    return set_component_runtime_error_fmt(
        error_buf, error_buf_size,
        "unsupported component instance expression tag 0x%02x",
        (unsigned)component_inst->instance_expression_tag);
}

static bool
resolve_component_exports(WASMComponentInstance *inst,
                          const WASMComponentExportSection *export_section,
                          char *error_buf, uint32 error_buf_size)
{
    uint32 i;

    for (i = 0; i < export_section->count; i++) {
        const WASMComponentExport *component_export = &export_section->exports[i];

        if (!resolve_component_sort_idx(inst, component_export->sort_idx,
                                        &inst->component_exports
                                             [inst->component_export_count]
                                                 .ref,
                                        error_buf, error_buf_size))
            return false;

        inst->component_exports[inst->component_export_count].name =
            get_component_export_name(component_export->export_name);
        if (inst->component_exports[inst->component_export_count].ref.type
            == WASM_COMP_RUNTIME_REF_FUNC)
            inst->component_exports[inst->component_export_count]
                .ref.of.function->is_top_level_export = true;
        inst->component_export_count++;
    }

    return true;
}

static bool
build_component_instance_graph(WASMComponentInstance *inst,
                               const struct InstantiationArgs2 *args,
                               char *error_buf, uint32 error_buf_size)
{
    const WASMComponent *component = &inst->module->component;
    uint32 i;

    for (i = 0; i < component->section_count; i++) {
        const WASMComponentSection *section = &component->sections[i];

        switch (section->id) {
            case WASM_COMP_SECTION_CORE_MODULE:
                if (!section->parsed.core_module
                    || !section->parsed.core_module->module_handle)
                    return set_component_runtime_error_fmt(
                        error_buf, error_buf_size,
                        "embedded core module is missing a runtime handle");

                inst->core_modules[inst->core_module_count++] =
                    (wasm_module_t)section->parsed.core_module->module_handle;
                break;
            case WASM_COMP_SECTION_CORE_INSTANCE: {
                uint32 j;
                const WASMComponentCoreInstSection *core_inst_section =
                    section->parsed.core_instance_section;

                for (j = 0; j < core_inst_section->count; j++) {
                    if (!instantiate_component_core_instance(
                            inst, &core_inst_section->instances[j], error_buf,
                            error_buf_size))
                        return false;
                }
                break;
            }
            case WASM_COMP_SECTION_ALIASES:
                if (!resolve_component_alias_section(
                        inst, section->parsed.alias_section, error_buf,
                        error_buf_size))
                    return false;
                break;
            case WASM_COMP_SECTION_COMPONENT:
                if (!append_top_level_component_definition(
                        inst, section->parsed.component, error_buf,
                        error_buf_size))
                    return false;
                break;
            case WASM_COMP_SECTION_IMPORTS:
                if (!resolve_top_level_component_imports(
                        inst, section->parsed.import_section, args, error_buf,
                        error_buf_size))
                    return false;
                break;
            case WASM_COMP_SECTION_CANONS: {
                uint32 j;
                const WASMComponentCanonSection *canon_section =
                    section->parsed.canon_section;

                for (j = 0; j < canon_section->count; j++) {
                    if (!append_component_canon_function(
                            inst, &canon_section->canons[j], error_buf,
                            error_buf_size))
                        return false;
                }
                break;
            }
            case WASM_COMP_SECTION_INSTANCES: {
                uint32 j;
                const WASMComponentInstSection *inst_section =
                    section->parsed.instance_section;

                for (j = 0; j < inst_section->count; j++) {
                    if (!instantiate_component_runtime_instance(
                            inst, &inst_section->instances[j], error_buf,
                            error_buf_size))
                        return false;
                }
                break;
            }
            case WASM_COMP_SECTION_EXPORTS:
                if (!resolve_component_exports(inst, section->parsed.export_section,
                                               error_buf, error_buf_size))
                    return false;
                break;
            case WASM_COMP_SECTION_START:
                if (!instantiate_component_start_section(
                        inst, section->parsed.start_section, error_buf,
                        error_buf_size))
                    return false;
                break;
            case WASM_COMP_SECTION_VALUES:
                if (!instantiate_component_value_section(
                        inst, section->parsed.value_section, error_buf,
                        error_buf_size))
                    return false;
                break;
            default:
                break;
        }
    }

    return true;
}

WASMComponentModule *
wasm_component_module_load(uint8 *buf, uint32 size, const LoadArgs *args,
                           char *error_buf, uint32 error_buf_size)
{
    WASMComponentModule *module = NULL;
    LoadArgs component_args = *args;
    WASMHeader header = { 0 };

    if (!buf || size < 8) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "WASM component load failed: unexpected end");
        return NULL;
    }

    if (!wasm_decode_header(buf, size, &header) || !is_wasm_component(header)) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "WASM component load failed: invalid component header");
        return NULL;
    }

    module = wasm_runtime_malloc(sizeof(WASMComponentModule));
    if (!module) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "WASM component load failed: allocate memory failed");
        return NULL;
    }
    memset(module, 0, sizeof(WASMComponentModule));

    module->module_type = Wasm_Module_Component;
    module->binary = buf;
    module->binary_size = size;
    module->is_binary_freeable = args->wasm_binary_freeable;

    component_args.is_component = true;
    if (!wasm_component_parse_sections(buf, size, &module->component,
                                       &component_args, 0)) {
        wasm_runtime_free(module);
        set_error_buf_ex(error_buf, error_buf_size,
                         "WASM component load failed: failed to parse "
                         "component sections");
        return NULL;
    }

    return module;
}

void
wasm_component_module_unload(WASMComponentModule *module)
{
    if (!module)
        return;

    wasm_component_free(&module->component);

    if (module->is_binary_freeable && module->binary)
        wasm_runtime_free(module->binary);

    wasm_runtime_free(module);
}

WASMComponentInstance *
wasm_component_module_instantiate(WASMComponentModule *module,
                                  const struct InstantiationArgs2 *args,
                                  char *error_buf, uint32 error_buf_size)
{
    WASMComponentInstance *inst = NULL;
    WASMComponentRuntimeAllocCounts counts;

    if (!module) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "Instantiate module failed, null component module");
        return NULL;
    }

    inst = wasm_runtime_malloc(sizeof(WASMComponentInstance));
    if (!inst) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "Instantiate module failed, allocate memory failed");
        return NULL;
    }

    memset(inst, 0, sizeof(WASMComponentInstance));
    inst->module_type = Wasm_Module_Component;
    inst->module = module;

    collect_component_runtime_alloc_counts(&module->component, &counts);
    if (!alloc_component_instance_graph(inst, &counts, error_buf,
                                        error_buf_size)
        || !(inst->resource_state = wasm_component_resource_state_create(
                 &module->component, error_buf, error_buf_size))
        || !build_component_instance_graph(inst, args, error_buf,
                                           error_buf_size)) {
        wasm_component_module_deinstantiate(inst);
        return NULL;
    }

    return inst;
}

void
wasm_component_module_deinstantiate(WASMComponentInstance *inst)
{
    if (!inst)
        return;

    destroy_component_instance_graph(inst);
    wasm_runtime_free(inst);
}
