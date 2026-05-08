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
#include "wasm_memory.h"
#include "wasm_runtime_common.h"

typedef struct WASMComponentRuntimeAllocCounts {
    uint32 core_module_count;
    uint32 core_instance_count;
    uint32 alias_count;
    uint32 component_count;
    uint32 component_instance_count;
    uint32 component_func_count;
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
                counts->component_func_count +=
                    section->parsed.canon_section->count;
                break;
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

    if (component_inst->owned_values) {
        for (i = 0; i < component_inst->owned_value_count; i++)
            wasm_component_runtime_value_clear(&component_inst->owned_values[i]);
        wasm_runtime_free(component_inst->owned_values);
        component_inst->owned_values = NULL;
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
    component_inst->owned_instance_count = 0;
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
    uint32 func_capacity, uint32 value_capacity, uint32 instance_capacity,
    uint32 component_capacity, char *error_buf,
    uint32 error_buf_size)
{
    memset(bindings, 0, sizeof(*bindings));

    if (!alloc_component_runtime_array((void **)&bindings->core_modules,
                                       core_module_capacity,
                                       sizeof(*bindings->core_modules),
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
        return false;
    }

    bindings->core_module_capacity = core_module_capacity;
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
                                          counts->alias_count,
                                          sizeof(*inst->core_funcs), error_buf,
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
    WASM_COMP_CANON_LIFT_VALUE_STRING
} WASMComponentCanonLiftValueKind;

typedef struct WASMComponentCanonLiftValueInfo {
    WASMComponentCanonLiftValueKind kind;
    bool declared_as_defined;
    uint8 prim_type;
    uint8 core_type;
    wasm_valkind_t public_kind;
} WASMComponentCanonLiftValueInfo;

static bool
lookup_component_canon_lift_value_type(const WASMComponent *component,
                                       const WASMComponentValueType *value_type,
                                       const char *position, uint32 index,
                                       bool allow_string,
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

    if (def_type->tag == WASM_COMP_DEF_VAL_LIST
        || def_type->tag == WASM_COMP_DEF_VAL_LIST_LEN)
        return set_component_call_error_fmt(
            inst, "component canon lift function %s %u requires memory-backed "
                  "Canonical ABI for %s",
            position, index, component_def_type_name(def_type->tag));

    return set_component_call_error_fmt(
        inst,
        "component canon lift function %s %u uses unsupported non-scalar %s type",
        position, index, component_def_type_name(def_type->tag));
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
                                                index, false, &info, inst))
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
validate_component_scalar_value(WASMComponentInstance *inst, wasm_val_t *value,
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
resolve_component_canon_lift_abi(WASMComponentInstance *inst,
                                 WASMComponentRuntimeFunc *function,
                                 char *error_buf, uint32 error_buf_size)
{
    const WASMComponentTypes *type_entry;
    const WASMComponentFuncType *func_type;
    uint32 i;
    bool needs_string_abi;

    function->string_encoding = WASM_COMP_RUNTIME_STRING_ENCODING_NONE;
    memset(&function->canon_memory_ref, 0, sizeof(function->canon_memory_ref));
    memset(&function->canon_realloc_ref, 0, sizeof(function->canon_realloc_ref));
    memset(&function->canon_post_return_ref, 0,
           sizeof(function->canon_post_return_ref));
    function->has_string_params = false;
    function->has_string_result = false;

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
            bool is_string;
            if (!resolve_component_lift_string_usage(
                    &inst->module->component, func_type->params->params[i].value_type,
                    &is_string, error_buf, error_buf_size))
                return false;
            if (is_string)
                function->has_string_params = true;
        }
    }

    if (func_type->results
        && func_type->results->tag == WASM_COMP_RESULT_LIST_WITH_TYPE
        && func_type->results->results) {
        if (!resolve_component_lift_string_usage(&inst->module->component,
                                                 func_type->results->results,
                                                 &function->has_string_result,
                                                 error_buf, error_buf_size))
            return false;
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
    needs_string_abi =
        function->has_string_params || function->has_string_result;
    if (!needs_string_abi)
        return true;

    if (function->string_encoding != WASM_COMP_RUNTIME_STRING_ENCODING_UTF8)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component canon lift function only supports UTF-8 strings");

    if (function->canon_memory_ref.type != WASM_COMP_CORE_RUNTIME_REF_MEMORY
        || !function->canon_memory_ref.of.memory)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component canon lift function requires memory for string Canonical "
            "ABI");

    if (function->canon_memory_ref.of.memory->is_memory64)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component canon lift function does not support memory64 string "
            "Canonical ABI");

    if (!function->core_func_ref.owner_instance
        || !function->core_func_ref.owner_instance->module_inst
        || !function->canon_memory_ref.owner_instance
        || function->canon_memory_ref.owner_instance->module_inst
               != function->core_func_ref.owner_instance->module_inst)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component canon lift function requires canon memory in the same "
            "core instance");

    if (function->has_string_params) {
        static const uint8 realloc_sig[] = { VALUE_TYPE_I32, VALUE_TYPE_I32,
                                             VALUE_TYPE_I32, VALUE_TYPE_I32,
                                             VALUE_TYPE_I32 };

        if (function->canon_realloc_ref.type != WASM_COMP_CORE_RUNTIME_REF_FUNC
            || !function->canon_realloc_ref.of.function)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "component canon lift function requires realloc for string "
                "parameters");

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
    const WASMComponentTypes *type_entry;
    WASMModuleInstanceCommon *core_module_inst;

    type_entry =
        wasm_component_lookup_type(&inst->module->component, function->type_idx);
    if (!type_entry)
        return set_component_call_error_fmt(
            inst,
            "component canon lift function uses unresolved type index %u",
            function->type_idx);

    if (type_entry->tag != WASM_COMP_FUNC_TYPE)
        return set_component_call_error_fmt(
            inst, "component canon lift function type index %u is not a function",
            function->type_idx);

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

    *out_component_type = type_entry->type.func_type;
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
call_component_canon_realloc(WASMComponentInstance *inst,
                             const WASMComponentRuntimeFunc *function,
                             uint32 new_size, uint32 *ptr_out)
{
    WASMExecEnv *exec_env;
    wasm_val_t args[4] = { 0 };
    wasm_val_t result = { 0 };

    if (new_size == 0) {
        *ptr_out = 0;
        return true;
    }

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
    args[2].of.i32 = 1;
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
    WASMComponentCanonLiftValueInfo result_info;
    wasm_val_t core_result = { 0 };
    wasm_val_t stack_args[16];
    wasm_val_t *core_args = stack_args;
    uint32 expected_result_count;
    uint32 expected_core_param_count = 0;
    uint32 i, core_arg_index = 0;
    bool call_succeeded = false;
    bool have_string_result_ptr = false;
    uint32 string_result_retptr = 0;

    if (!inst)
        return false;

    if (!function) {
        wasm_runtime_set_exception((WASMModuleInstanceCommon *)inst,
                                   "component function handle is null");
        return false;
    }

    wasm_runtime_clear_exception((WASMModuleInstanceCommon *)inst);

    if (function->kind != WASM_COMP_RUNTIME_FUNC_LIFT)
        return set_component_call_error(
            inst, "component call only supports canon lift functions");

    if (require_top_level_export && !function->is_top_level_export)
        return set_component_call_error(
            inst, "component call only supports top-level exported canon lift "
                  "functions");

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

    for (i = 0; i < component_type->params->count; i++) {
        WASMComponentCanonLiftValueInfo type_info;

        if (!lookup_component_canon_lift_value_type(
                &inst->module->component, component_type->params->params[i].value_type,
                "parameter", i, true, &type_info, inst))
            return false;

        expected_core_param_count +=
            type_info.kind == WASM_COMP_CANON_LIFT_VALUE_STRING ? 2 : 1;
    }

    if (expected_core_param_count != core_type->param_count)
        return set_component_call_error(
            inst, "component canon lift function uses unsupported Canonical ABI "
                  "flattening for parameters");

    if (expected_result_count == 1) {
        if (!lookup_component_canon_lift_value_type(
                &inst->module->component, component_type->results->results, "result",
                0, true, &result_info, inst))
            return false;

        if (result_info.kind == WASM_COMP_CANON_LIFT_VALUE_STRING) {
            if (core_type->result_count != 1
                || core_type->types[core_type->param_count] != VALUE_TYPE_I32)
                return set_component_call_error(
                    inst, "component canon lift function only supports a single "
                          "string result returned through memory");
        }
        else if (core_type->result_count != 1
                 || core_type->types[core_type->param_count]
                        != result_info.core_type)
            return set_component_call_error(
                inst, "component canon lift function result does not match the "
                      "core function signature");
    }
    else if (core_type->result_count != 0)
        return set_component_call_error(
            inst, "component canon lift function only supports at most one "
                  "result");

    if (core_type->param_count > sizeof(stack_args) / sizeof(stack_args[0])) {
        core_args = wasm_runtime_malloc(sizeof(wasm_val_t) * core_type->param_count);
        if (!core_args)
            return set_component_call_error(
                inst, "component canon lift function could not allocate argument "
                      "storage");
    }
    memset(core_args, 0, sizeof(wasm_val_t) * core_type->param_count);

    for (i = 0; i < component_type->params->count; i++) {
        WASMComponentCanonLiftValueInfo type_info;

        if (!lookup_component_canon_lift_value_type(
                &inst->module->component, component_type->params->params[i].value_type,
                "parameter", i, true, &type_info, inst)) {
            call_succeeded = false;
            goto cleanup;
        }

        if (type_info.kind == WASM_COMP_CANON_LIFT_VALUE_SCALAR) {
            if (!decode_component_public_scalar_value(inst, &args[i], &type_info,
                                                      "parameter", i,
                                                      &core_args[core_arg_index])) {
                call_succeeded = false;
                goto cleanup;
            }
            if (core_type->types[core_arg_index] != type_info.core_type) {
                set_component_call_error_fmt(
                    inst, "component canon lift function parameter %u does not "
                          "match the core function signature",
                    i);
                call_succeeded = false;
                goto cleanup;
            }
            core_arg_index++;
        }
        else {
            const uint8 *payload;
            uint32 payload_len;
            uint32 guest_ptr = 0;
            uint8 *guest_bytes = NULL;

            if (function->string_encoding
                != WASM_COMP_RUNTIME_STRING_ENCODING_UTF8) {
                set_component_call_error(
                    inst, "component canon lift function only supports UTF-8 "
                          "string encoding");
                call_succeeded = false;
                goto cleanup;
            }

            if (!decode_component_public_string_value(inst, &args[i], &type_info,
                                                      "parameter", i, &payload,
                                                      &payload_len)
                || !call_component_canon_realloc(inst, function, payload_len,
                                                 &guest_ptr)) {
                call_succeeded = false;
                goto cleanup;
            }

            if (payload_len > 0) {
                if (!get_component_canon_memory_bytes(inst, function, guest_ptr,
                                                      payload_len,
                                                      "string parameter buffer",
                                                      &guest_bytes)) {
                    call_succeeded = false;
                    goto cleanup;
                }
                memcpy(guest_bytes, payload, payload_len);
            }

            if (core_type->types[core_arg_index] != VALUE_TYPE_I32
                || core_type->types[core_arg_index + 1] != VALUE_TYPE_I32) {
                set_component_call_error_fmt(
                    inst, "component canon lift function parameter %u does not "
                          "match the core function signature",
                    i);
                call_succeeded = false;
                goto cleanup;
            }

            core_args[core_arg_index].kind = WASM_I32;
            core_args[core_arg_index].of.i32 = (int32)guest_ptr;
            core_args[core_arg_index + 1].kind = WASM_I32;
            core_args[core_arg_index + 1].of.i32 = (int32)payload_len;
            core_arg_index += 2;
        }
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
    if (!wasm_runtime_call_wasm_a(exec_env, function->core_func_ref.of.function,
                                  core_type->result_count,
                                  core_type->result_count ? &core_result : NULL,
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
        if (result_info.kind == WASM_COMP_CANON_LIFT_VALUE_SCALAR) {
            if (!validate_component_scalar_value(inst, &core_result,
                                                 result_info.public_kind,
                                                 result_info.prim_type, "result",
                                                 0)
                || !init_component_public_scalar_result(inst, &result_info,
                                                        &core_result,
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
            bool copied = false;

            if (core_result.kind != WASM_I32) {
                set_component_call_error(
                    inst, "component canon lift function string result returned "
                          "an unexpected result kind");
                call_succeeded = false;
                goto cleanup;
            }

            string_result_retptr = (uint32)core_result.of.i32;
            have_string_result_ptr = true;
            if (!get_component_canon_memory_bytes(inst, function, string_result_retptr,
                                                  8, "string result area",
                                                  &ret_area_bytes)) {
                call_succeeded = false;
                goto cleanup;
            }

            memcpy(&payload_ptr, ret_area_bytes, sizeof(payload_ptr));
            memcpy(&payload_len, ret_area_bytes + sizeof(payload_ptr),
                   sizeof(payload_len));

            if (payload_len > 0) {
                if (!get_component_canon_memory_bytes(inst, function, payload_ptr,
                                                      payload_len,
                                                      "string result payload",
                                                      &payload_bytes)) {
                    call_succeeded = false;
                    goto cleanup;
                }
                if (!wasm_component_validate_utf8(payload_bytes, payload_len)) {
                    set_component_call_error(
                        inst, "component canon lift function result does not "
                              "contain valid UTF-8");
                    call_succeeded = false;
                    goto cleanup;
                }

                payload_copy = wasm_runtime_malloc(payload_len);
                if (!payload_copy) {
                    set_component_call_error(
                        inst, "component canon lift function could not allocate "
                              "result storage");
                    call_succeeded = false;
                    goto cleanup;
                }
                memcpy(payload_copy, payload_bytes, payload_len);
                copied = true;
            }

            if (!init_component_public_string_result(
                    inst, &result_info, copied ? payload_copy : NULL, payload_len,
                    &results[0])) {
                if (payload_copy)
                    wasm_runtime_free(payload_copy);
                call_succeeded = false;
                goto cleanup;
            }
            if (payload_copy)
                wasm_runtime_free(payload_copy);
        }
    }

    call_succeeded = true;

cleanup:
    if (have_string_result_ptr) {
        bool post_return_ok =
            call_component_canon_post_return(inst, function, string_result_retptr);
        if (call_succeeded && !post_return_ok)
            call_succeeded = false;
    }

    if (core_args != stack_args)
        wasm_runtime_free(core_args);
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

    if (function->kind != WASM_COMP_RUNTIME_FUNC_LIFT)
        return set_component_call_error(
            inst, "component call only supports canon lift functions");

    if (require_top_level_export && !function->is_top_level_export)
        return set_component_call_error(
            inst, "component call only supports top-level exported canon lift "
                  "functions");

    if (function->core_func_ref.type != WASM_COMP_CORE_RUNTIME_REF_FUNC
        || !function->core_func_ref.of.function)
        return set_component_call_error(
            inst, "component canon lift function is not bound to a core function");

    if (function->has_string_params || function->has_string_result)
        return set_component_call_error(
            inst, "component canon lift function uses string values; call "
                  "through the component value API");

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
                                        num_args, args, true);
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
                                               results, num_args, args, true);
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
resolve_component_start_scalar_type(const WASMComponent *component,
                                    const WASMComponentValueType *value_type,
                                    const char *position, uint32 index,
                                    uint8 *prim_type_out,
                                    wasm_valkind_t *public_kind_out,
                                    char *error_buf,
                                    uint32 error_buf_size)
{
    const WASMComponentTypes *type_entry;
    WASMComponentDefValType *def_type;
    uint8 prim_type;
    uint8 ignored_core_type;

    if (!value_type)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component start section %s %u is missing a type", position, index);

    if (value_type->type == WASM_COMP_VAL_TYPE_PRIMVAL) {
        prim_type = value_type->type_specific.primval_type;
        if (!component_scalar_prim_to_core(prim_type, &ignored_core_type,
                                           public_kind_out)) {
            if (prim_type == WASM_COMP_PRIMVAL_STRING)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component start section %s %u requires memory-backed "
                    "Canonical ABI for string",
                    position, index);
            if (prim_type == WASM_COMP_PRIMVAL_ERROR_CONTEXT)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component start section %s %u uses unsupported component "
                    "scalar type error-context",
                    position, index);
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "component start section %s %u uses unsupported component "
                "scalar type %s",
                position, index, component_prim_type_name(prim_type));
        }
        *prim_type_out = prim_type;
        return true;
    }

    type_entry =
        wasm_component_lookup_type(component, value_type->type_specific.type_idx);
    if (!type_entry)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component start section %s %u uses unresolved type index %u",
            position, index, value_type->type_specific.type_idx);

    if (type_entry->tag != WASM_COMP_DEF_TYPE)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component start section %s %u uses non-value type index %u",
            position, index, value_type->type_specific.type_idx);

    def_type = type_entry->type.def_val_type;
    if (def_type->tag == WASM_COMP_DEF_VAL_PRIMVAL) {
        prim_type = def_type->def_val.primval;
        if (!component_scalar_prim_to_core(prim_type, &ignored_core_type,
                                           public_kind_out)) {
            if (prim_type == WASM_COMP_PRIMVAL_STRING)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component start section %s %u requires memory-backed "
                    "Canonical ABI for string",
                    position, index);
            if (prim_type == WASM_COMP_PRIMVAL_ERROR_CONTEXT)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component start section %s %u uses unsupported component "
                    "scalar type error-context",
                    position, index);
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "component start section %s %u uses unsupported component "
                "scalar type %s",
                position, index, component_prim_type_name(prim_type));
        }
        *prim_type_out = prim_type;
        return true;
    }

    if (def_type->tag == WASM_COMP_DEF_VAL_LIST
        || def_type->tag == WASM_COMP_DEF_VAL_LIST_LEN)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component start section %s %u requires memory-backed Canonical "
            "ABI for %s",
            position, index, component_def_type_name(def_type->tag));

    return set_component_runtime_error_fmt(
        error_buf, error_buf_size,
        "component start section %s %u uses unsupported non-scalar %s type",
        position, index, component_def_type_name(def_type->tag));
}

static bool
decode_component_start_leb(const uint8 *data, uint32 byte_size, uint32 maxbits,
                           bool sign, const char *type_name,
                           const char *position, uint32 index, uint64 *out_value,
                           char *error_buf, uint32 error_buf_size)
{
    size_t offset = 0;
    bh_leb_read_status_t status;

    if (!data || byte_size == 0)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component start section %s %u is missing %s bytes", position,
            index, type_name);

    status = bh_leb_read(data, data + byte_size, maxbits, sign, out_value, &offset);
    if (status != BH_LEB_READ_SUCCESS || offset != byte_size)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component start section %s %u does not contain a valid %s value",
            position, index, type_name);

    return true;
}

static bool
decode_component_start_char(const uint8 *data, uint32 byte_size,
                            const char *position, uint32 index,
                            uint32 *code_point_out, char *error_buf,
                            uint32 error_buf_size)
{
    uint32 code_point;

    if (!data || !wasm_component_validate_single_utf8_scalar(data, byte_size))
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component start section %s %u does not contain a valid char value",
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
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "component start section %s %u does not contain a valid char "
                "value",
                position, index);
    }

    *code_point_out = code_point;
    return true;
}

static bool
decode_component_start_argument(const WASMComponent *component,
                                const WASMComponentRuntimeValue *runtime_value,
                                uint32 index, wasm_val_t *out_arg,
                                char *error_buf, uint32 error_buf_size)
{
    const uint8 *data =
        wasm_component_runtime_value_get_data(runtime_value);
    uint8 prim_type;
    wasm_valkind_t expected_kind;
    uint64 decoded_u64;
    uint32 code_point;

    if (!runtime_value || !out_arg)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component start section argument %u is null", index);

    if (!resolve_component_start_scalar_type(component,
                                             runtime_value->type.declared_type,
                                             "argument", index, &prim_type,
                                             &expected_kind, error_buf,
                                             error_buf_size))
        return false;

    memset(out_arg, 0, sizeof(*out_arg));
    out_arg->kind = expected_kind;

    switch (prim_type) {
        case WASM_COMP_PRIMVAL_BOOL:
            if (!data || runtime_value->byte_size != 1
                || (data[0] != 0 && data[0] != 1))
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component start section argument %u does not contain a "
                    "valid bool value",
                    index);
            out_arg->of.i32 = data[0];
            return true;
        case WASM_COMP_PRIMVAL_S8:
            if (!data || runtime_value->byte_size != 1)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component start section argument %u does not contain a "
                    "valid s8 value",
                    index);
            out_arg->of.i32 = (int8)data[0];
            return true;
        case WASM_COMP_PRIMVAL_U8:
            if (!data || runtime_value->byte_size != 1)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component start section argument %u does not contain a "
                    "valid u8 value",
                    index);
            out_arg->of.i32 = data[0];
            return true;
        case WASM_COMP_PRIMVAL_S16:
            if (!decode_component_start_leb(data, runtime_value->byte_size, 16,
                                            true, "s16", "argument", index,
                                            &decoded_u64, error_buf,
                                            error_buf_size))
                return false;
            out_arg->of.i32 = (int16)(uint16)decoded_u64;
            return true;
        case WASM_COMP_PRIMVAL_U16:
            if (!decode_component_start_leb(data, runtime_value->byte_size, 16,
                                            false, "u16", "argument", index,
                                            &decoded_u64, error_buf,
                                            error_buf_size))
                return false;
            out_arg->of.i32 = (uint16)decoded_u64;
            return true;
        case WASM_COMP_PRIMVAL_S32:
            if (!decode_component_start_leb(data, runtime_value->byte_size, 32,
                                            true, "s32", "argument", index,
                                            &decoded_u64, error_buf,
                                            error_buf_size))
                return false;
            out_arg->of.i32 = (int32)decoded_u64;
            return true;
        case WASM_COMP_PRIMVAL_U32:
            if (!decode_component_start_leb(data, runtime_value->byte_size, 32,
                                            false, "u32", "argument", index,
                                            &decoded_u64, error_buf,
                                            error_buf_size))
                return false;
            out_arg->of.i32 = (int32)(uint32)decoded_u64;
            return true;
        case WASM_COMP_PRIMVAL_S64:
            if (!decode_component_start_leb(data, runtime_value->byte_size, 64,
                                            true, "s64", "argument", index,
                                            &decoded_u64, error_buf,
                                            error_buf_size))
                return false;
            out_arg->of.i64 = (int64)decoded_u64;
            return true;
        case WASM_COMP_PRIMVAL_U64:
            if (!decode_component_start_leb(data, runtime_value->byte_size, 64,
                                            false, "u64", "argument", index,
                                            &decoded_u64, error_buf,
                                            error_buf_size))
                return false;
            out_arg->of.i64 = (int64)decoded_u64;
            return true;
        case WASM_COMP_PRIMVAL_F32:
            if (!data || runtime_value->byte_size != sizeof(out_arg->of.f32))
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component start section argument %u does not contain a "
                    "valid f32 value",
                    index);
            memcpy(&out_arg->of.f32, data, sizeof(out_arg->of.f32));
            return true;
        case WASM_COMP_PRIMVAL_F64:
            if (!data || runtime_value->byte_size != sizeof(out_arg->of.f64))
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component start section argument %u does not contain a "
                    "valid f64 value",
                    index);
            memcpy(&out_arg->of.f64, data, sizeof(out_arg->of.f64));
            return true;
        case WASM_COMP_PRIMVAL_CHAR:
            if (!decode_component_start_char(data, runtime_value->byte_size,
                                             "argument", index, &code_point,
                                             error_buf, error_buf_size))
                return false;
            out_arg->of.i32 = (int32)code_point;
            return true;
        default:
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "component start section argument %u uses unsupported component "
                "scalar type %s",
                index, component_prim_type_name(prim_type));
    }
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
encode_component_start_char(uint32 code_point, uint8 *out_buf,
                            uint32 *out_len, char *error_buf,
                            uint32 error_buf_size)
{
    if (!is_valid_unicode_scalar(code_point))
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component start section result is not a valid Unicode scalar "
            "value");

    if (code_point <= 0x7F) {
        out_buf[0] = (uint8)code_point;
        *out_len = 1;
    }
    else if (code_point <= 0x7FF) {
        out_buf[0] = (uint8)(0xC0 | (code_point >> 6));
        out_buf[1] = (uint8)(0x80 | (code_point & 0x3F));
        *out_len = 2;
    }
    else if (code_point <= 0xFFFF) {
        out_buf[0] = (uint8)(0xE0 | (code_point >> 12));
        out_buf[1] = (uint8)(0x80 | ((code_point >> 6) & 0x3F));
        out_buf[2] = (uint8)(0x80 | (code_point & 0x3F));
        *out_len = 3;
    }
    else {
        out_buf[0] = (uint8)(0xF0 | (code_point >> 18));
        out_buf[1] = (uint8)(0x80 | ((code_point >> 12) & 0x3F));
        out_buf[2] = (uint8)(0x80 | ((code_point >> 6) & 0x3F));
        out_buf[3] = (uint8)(0x80 | (code_point & 0x3F));
        *out_len = 4;
    }

    return true;
}

static bool
init_component_start_result_value(WASMComponentInstance *inst,
                                  const WASMComponentValueType *value_type,
                                  const wasm_val_t *result,
                                  WASMComponentRuntimeValue *runtime_value,
                                  char *error_buf, uint32 error_buf_size)
{
    uint8 storage[16];
    uint8 prim_type;
    wasm_valkind_t expected_kind;
    uint32 storage_len = 0;

    if (!resolve_component_start_scalar_type(&inst->module->component, value_type,
                                             "result", 0, &prim_type,
                                             &expected_kind, error_buf,
                                             error_buf_size))
        return false;

    if (!result || result->kind != expected_kind)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "component start section result kind %u does not match expected "
            "kind %u",
            result ? (unsigned)result->kind : UINT_MAX, (unsigned)expected_kind);

    switch (prim_type) {
        case WASM_COMP_PRIMVAL_BOOL:
            if (result->of.i32 != 0 && result->of.i32 != 1)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component start section result is not a valid bool value");
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
            storage_len = encode_component_signed_leb((int64)result->of.i32,
                                                      storage);
            break;
        case WASM_COMP_PRIMVAL_U16:
        case WASM_COMP_PRIMVAL_U32:
            storage_len = encode_component_unsigned_leb((uint32)result->of.i32,
                                                        storage);
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
            if (!encode_component_start_char((uint32)result->of.i32, storage,
                                             &storage_len, error_buf,
                                             error_buf_size))
                return false;
            break;
        default:
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "component start section result uses unsupported component "
                "scalar type %s",
                component_prim_type_name(prim_type));
    }

    return wasm_component_runtime_value_init_inline(
        runtime_value, &inst->module->component, value_type, storage, storage_len,
        error_buf, error_buf_size);
}

static bool
instantiate_component_start_section(WASMComponentInstance *inst,
                                    const WASMComponentStartSection *start_section,
                                    char *error_buf, uint32 error_buf_size)
{
    const WASMComponentRuntimeFunc *function;
    WASMComponentFuncType *component_type = NULL;
    WASMFuncType *ignored_core_type = NULL;
    wasm_val_t *args = NULL;
    wasm_val_t result = { 0 };
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
        args = wasm_runtime_malloc(sizeof(wasm_val_t)
                                   * start_section->value_args_count);
        if (!args)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "allocate memory failed for component start arguments");
        memset(args, 0, sizeof(wasm_val_t) * start_section->value_args_count);
    }

    for (i = 0; i < start_section->value_args_count; i++) {
        uint32 value_idx = start_section->value_args[i];

        if (value_idx >= inst->component_value_count) {
            wasm_runtime_free(args);
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "component start section value index %u is out of bounds",
                value_idx);
        }

        if (!decode_component_start_argument(&inst->module->component,
                                             &inst->component_values[value_idx],
                                             i, &args[i], error_buf,
                                             error_buf_size)) {
            wasm_runtime_free(args);
            return false;
        }
    }

    if (!wasm_component_call_internal(inst, function, start_section->result,
                                      start_section->result ? &result : NULL,
                                      start_section->value_args_count, args,
                                      false)) {
        wasm_runtime_free(args);
        return set_component_start_error_from_exception(
            inst, error_buf, error_buf_size, "component start section failed");
    }

    if (start_section->result > 0) {
        if (!resolve_component_canon_lift_type(inst, function, &component_type,
                                               &ignored_core_type)) {
            wasm_runtime_free(args);
            return set_component_start_error_from_exception(
                inst, error_buf, error_buf_size,
                "component start section failed");
        }

        if (!component_type || !component_type->results
            || component_type->results->tag != WASM_COMP_RESULT_LIST_WITH_TYPE
            || !component_type->results->results) {
            wasm_runtime_free(args);
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "component start section result metadata is missing");
        }

        if (!init_component_start_result_value(
                inst, component_type->results->results, &result,
                &inst->component_values[inst->component_value_count], error_buf,
                error_buf_size)) {
            wasm_runtime_free(args);
            return false;
        }

        inst->component_value_count++;
    }

    wasm_runtime_free(args);
    return true;
}

static bool
instantiate_nested_component_start_section(
    WASMComponentInstance *inst, WASMComponentRuntimeInstance *runtime_inst,
    WASMNestedComponentLocalBindings *bindings, const WASMComponent *component,
    const WASMComponentStartSection *start_section, char *error_buf,
    uint32 error_buf_size)
{
    const WASMComponentRuntimeFunc *function;
    WASMComponentFuncType *component_type = NULL;
    WASMFuncType *ignored_core_type = NULL;
    wasm_val_t *args = NULL;
    wasm_val_t result = { 0 };
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
        args = wasm_runtime_malloc(sizeof(wasm_val_t)
                                   * start_section->value_args_count);
        if (!args)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "allocate memory failed for nested component start arguments");
        memset(args, 0, sizeof(wasm_val_t) * start_section->value_args_count);
    }

    for (i = 0; i < start_section->value_args_count; i++) {
        uint32 value_idx = start_section->value_args[i];

        if (value_idx >= bindings->value_count) {
            wasm_runtime_free(args);
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "nested component start section value index %u is out of "
                "bounds",
                value_idx);
        }

        if (!decode_component_start_argument(component, bindings->values[value_idx],
                                             i, &args[i], error_buf,
                                             error_buf_size)) {
            wasm_runtime_free(args);
            return false;
        }
    }

    if (!wasm_component_call_internal(inst, function, start_section->result,
                                      start_section->result ? &result : NULL,
                                      start_section->value_args_count, args,
                                      false)) {
        wasm_runtime_free(args);
        return set_component_start_error_from_exception(
            inst, error_buf, error_buf_size,
            "nested component start section failed");
    }

    if (start_section->result > 0) {
        WASMComponentRuntimeValue *runtime_value =
            &runtime_inst->owned_values[runtime_inst->owned_value_count];

        if (!resolve_component_canon_lift_type(inst, function, &component_type,
                                               &ignored_core_type)) {
            wasm_runtime_free(args);
            return set_component_start_error_from_exception(
                inst, error_buf, error_buf_size,
                "nested component start section failed");
        }

        if (!component_type || !component_type->results
            || component_type->results->tag != WASM_COMP_RESULT_LIST_WITH_TYPE
            || !component_type->results->results) {
            wasm_runtime_free(args);
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "nested component start section result metadata is missing");
        }

        if (!init_component_start_result_value(
                inst, component_type->results->results, &result, runtime_value,
                error_buf, error_buf_size)) {
            wasm_runtime_free(args);
            return false;
        }

        if (!append_nested_component_local_value(bindings, runtime_value, error_buf,
                                                 error_buf_size)) {
            wasm_component_runtime_value_clear(runtime_value);
            wasm_runtime_free(args);
            return false;
        }

        runtime_inst->owned_value_count++;
    }

    wasm_runtime_free(args);
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
            if (export_item->ref.type != expected_type)
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

    if (inst->component_func_count >= UINT32_MAX)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size, "too many component functions");

    func = &inst->component_funcs[inst->component_func_count++];
    memset(func, 0, sizeof(*func));
    func->canon_tag = canon->tag;

    if (canon->tag == WASM_COMP_CANON_LIFT) {
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

        if (func->core_func_ref.type != WASM_COMP_CORE_RUNTIME_REF_FUNC)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "canon lift core func index %u does not resolve to a function",
                canon->canon_data.lift.core_func_idx);

        if (!resolve_component_canon_lift_abi(inst, func, error_buf,
                                              error_buf_size))
            return false;
    }
    else {
        func->kind = WASM_COMP_RUNTIME_FUNC_UNSUPPORTED_CANON;
    }

    return true;
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
                                      uint32 *func_count,
                                      uint32 *value_count,
                                      uint32 *instance_count,
                                      uint32 *component_count,
                                      uint32 *owned_value_count,
                                      uint32 *owned_component_count,
                                      uint32 *owned_instance_count,
                                      uint32 *export_count, char *error_buf,
                                      uint32 error_buf_size)
{
    uint32 i;

    *import_count = *core_module_count = *func_count = *value_count
        = *instance_count = *component_count = *owned_value_count
        = *owned_component_count = *owned_instance_count = *export_count = 0;

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
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "nested component core module sections are not supported "
                    "yet");
            case WASM_COMP_SECTION_CORE_INSTANCE:
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "nested component core instance sections are not supported "
                    "yet");
            case WASM_COMP_SECTION_CORE_TYPE:
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "nested component core type sections are not supported yet");
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

static bool
validate_component_import_binding_type(const WASMComponentImport *component_import,
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
            return ref.type == WASM_COMP_RUNTIME_REF_FUNC
                       ? true
                       : set_component_runtime_error_fmt(
                              error_buf, error_buf_size,
                               "component import \"%s\" bound to the wrong "
                               "runtime sort",
                               import_name);
        case WASM_COMP_EXTERN_VALUE:
            return ref.type == WASM_COMP_RUNTIME_REF_VALUE
                       ? true
                       : set_component_runtime_error_fmt(
                             error_buf, error_buf_size,
                             "component import \"%s\" bound to the wrong "
                             "runtime sort",
                             import_name);
        case WASM_COMP_EXTERN_CORE_MODULE:
            return ref.type == WASM_COMP_RUNTIME_REF_CORE_MODULE
                       ? true
                       : set_component_runtime_error_fmt(
                              error_buf, error_buf_size,
                              "component import \"%s\" bound to the wrong "
                              "runtime sort",
                              import_name);
        case WASM_COMP_EXTERN_INSTANCE:
            return ref.type == WASM_COMP_RUNTIME_REF_INSTANCE
                       ? true
                       : set_component_runtime_error_fmt(
                              error_buf, error_buf_size,
                              "component import \"%s\" bound to the wrong "
                              "runtime sort",
                              import_name);
        case WASM_COMP_EXTERN_COMPONENT:
            return ref.type == WASM_COMP_RUNTIME_REF_COMPONENT
                       ? true
                       : set_component_runtime_error_fmt(
                              error_buf, error_buf_size,
                              "component import \"%s\" bound to the wrong "
                              "runtime sort",
                              import_name);
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

static bool
validate_component_import_binding_type(const WASMComponentImport *component_import,
                                       WASMComponentRuntimeRef ref,
                                       char *error_buf,
                                       uint32 error_buf_size);

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

        if (!args || !args->component_imports || args->component_import_count == 0)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "top-level component import \"%s\" is missing a binding",
                import_name);

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
                         component_import, ref, error_buf, error_buf_size)
                     || !append_top_level_component_import(inst, ref, error_buf,
                                                           error_buf_size))
                return false;

            matched = true;
            break;
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
                        component_import, resolved_imports[import_index],
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
                        component_import, resolved_imports[import_index],
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
              owned_value_count = 0, owned_instance_count = 0,
              export_count = 0;

    memset(&bindings, 0, sizeof(bindings));
    runtime_inst->resource_state = wasm_component_resource_state_create(
        component, error_buf, error_buf_size);
    if (!runtime_inst->resource_state)
        return false;
    runtime_inst->owns_resource_state = true;

    if (!count_nested_component_local_bindings(
            component, &import_count, &bindings.core_module_capacity,
            &bindings.func_capacity, &bindings.value_capacity,
            &bindings.instance_capacity, &bindings.component_capacity,
            &owned_value_count, &owned_component_count, &owned_instance_count,
            &export_count, error_buf, error_buf_size))
        return false;

    if (resolved_import_count != import_count)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "nested component import binding count mismatch");

    if (!alloc_nested_component_local_bindings(
            &bindings, bindings.core_module_capacity, bindings.func_capacity,
            bindings.value_capacity, bindings.instance_capacity,
            bindings.component_capacity, error_buf, error_buf_size)
        || !alloc_component_runtime_array(
            (void **)&runtime_inst->owned_values, owned_value_count,
            sizeof(*runtime_inst->owned_values), error_buf, error_buf_size)
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
                break;
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
                            component_import, resolved_imports[import_index],
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
                        inst, runtime_inst, &bindings, component,
                        section->parsed.start_section, error_buf,
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
