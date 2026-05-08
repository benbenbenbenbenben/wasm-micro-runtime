/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wasm_component_runtime.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "wasm_runtime_common.h"

typedef struct WASMComponentRuntimeAllocCounts {
    uint32 core_module_count;
    uint32 core_instance_count;
    uint32 alias_count;
    uint32 component_count;
    uint32 component_instance_count;
    uint32 component_func_count;
    uint32 component_export_count;
} WASMComponentRuntimeAllocCounts;

typedef struct WASMNestedComponentLocalBindings {
    uint32 func_count;
    uint32 func_capacity;
    WASMComponentRuntimeRef *funcs;
    uint32 instance_count;
    uint32 instance_capacity;
    WASMComponentRuntimeRef *instances;
} WASMNestedComponentLocalBindings;

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

static void
collect_component_runtime_alloc_counts(const WASMComponent *component,
                                       WASMComponentRuntimeAllocCounts *counts)
{
    uint32 i;

    memset(counts, 0, sizeof(*counts));

    for (i = 0; i < component->section_count; i++) {
        const WASMComponentSection *section = &component->sections[i];

        switch (section->id) {
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
                    else if (alias->sort->sort == WASM_COMP_SORT_INSTANCE)
                        counts->component_instance_count++;
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
    component_inst->owned_instance_count = 0;
    component_inst->owns_exports = false;
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
    if (inst->component_funcs) {
        wasm_runtime_free(inst->component_funcs);
        inst->component_funcs = NULL;
    }
    if (inst->components) {
        wasm_runtime_free(inst->components);
        inst->components = NULL;
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
    WASMNestedComponentLocalBindings *bindings, uint32 func_capacity,
    uint32 instance_capacity, char *error_buf, uint32 error_buf_size)
{
    memset(bindings, 0, sizeof(*bindings));

    if (!alloc_component_runtime_array((void **)&bindings->funcs, func_capacity,
                                       sizeof(*bindings->funcs), error_buf,
                                       error_buf_size)
        || !alloc_component_runtime_array((void **)&bindings->instances,
                                          instance_capacity,
                                          sizeof(*bindings->instances),
                                          error_buf, error_buf_size)) {
        if (bindings->funcs) {
            wasm_runtime_free(bindings->funcs);
            bindings->funcs = NULL;
        }
        if (bindings->instances) {
            wasm_runtime_free(bindings->instances);
            bindings->instances = NULL;
        }
        return false;
    }

    bindings->func_capacity = func_capacity;
    bindings->instance_capacity = instance_capacity;
    return true;
}

static void
free_nested_component_local_bindings(WASMNestedComponentLocalBindings *bindings)
{
    if (bindings->funcs) {
        wasm_runtime_free(bindings->funcs);
        bindings->funcs = NULL;
    }
    if (bindings->instances) {
        wasm_runtime_free(bindings->instances);
        bindings->instances = NULL;
    }

    bindings->func_count = 0;
    bindings->func_capacity = 0;
    bindings->instance_count = 0;
    bindings->instance_capacity = 0;
}

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
        case WASM_COMP_RUNTIME_REF_INSTANCE:
            if (bindings->instance_count >= bindings->instance_capacity)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "nested component local instance space overflow");
            bindings->instances[bindings->instance_count++] = ref;
            return true;
        default:
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "nested component local ref type %u is not supported yet",
                (unsigned)ref.type);
    }
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
        case WASM_COMP_SORT_FUNC:
            if (sort_idx->idx >= bindings->func_count)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "nested component func index %u is out of bounds",
                    sort_idx->idx);
            *out_ref = bindings->funcs[sort_idx->idx];
            return true;
        case WASM_COMP_SORT_INSTANCE:
            if (sort_idx->idx >= bindings->instance_count)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "nested component instance index %u is out of bounds",
                    sort_idx->idx);
            *out_ref = bindings->instances[sort_idx->idx];
            return true;
        default:
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "nested component sort 0x%02x is not supported yet",
                (unsigned)sort_idx->sort->sort);
    }
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

    if (sort_idx->sort->sort == WASM_COMP_SORT_CORE_SORT)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "core sorts are not valid in component sort resolution");

    switch (sort_idx->sort->sort) {
        case WASM_COMP_SORT_FUNC:
            if (sort_idx->idx >= inst->component_func_count)
                return set_component_runtime_error_fmt(
                    error_buf, error_buf_size,
                    "component func index %u is out of bounds", sort_idx->idx);
            out_ref->type = WASM_COMP_RUNTIME_REF_FUNC;
            out_ref->of.function = &inst->component_funcs[sort_idx->idx];
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
                case WASM_COMP_SORT_INSTANCE:
                    expected_type = WASM_COMP_RUNTIME_REF_INSTANCE;
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
            case WASM_COMP_SORT_FUNC:
                expected_type = WASM_COMP_RUNTIME_REF_FUNC;
                sort_name = "func";
                break;
            case WASM_COMP_SORT_INSTANCE:
                expected_type = WASM_COMP_RUNTIME_REF_INSTANCE;
                sort_name = "instance";
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
                                      uint32 *func_count,
                                      uint32 *instance_count,
                                      uint32 *owned_instance_count,
                                      uint32 *export_count, char *error_buf,
                                      uint32 error_buf_size)
{
    uint32 i;

    *import_count = *func_count = *instance_count = *owned_instance_count
        = *export_count = 0;

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
                        case WASM_COMP_EXTERN_INSTANCE:
                            (*import_count)++;
                            (*instance_count)++;
                            break;
                        default:
                            return set_component_runtime_error_fmt(
                                error_buf, error_buf_size,
                                "nested component imports other than "
                                "func/instance are not supported yet");
                    }
                }
                break;
            }
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
                            "func/instance sorts");

                    switch (alias_def->sort->sort) {
                        case WASM_COMP_SORT_FUNC:
                            (*func_count)++;
                            break;
                        case WASM_COMP_SORT_INSTANCE:
                            (*instance_count)++;
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
                        != WASM_COMP_INSTANCE_EXPRESSION_WITHOUT_ARGS)
                        return set_component_runtime_error_fmt(
                            error_buf, error_buf_size,
                            "nested component instance expressions other than "
                            "inline exports are not supported yet");

                    (*instance_count)++;
                    (*owned_instance_count)++;
                }
                break;
            }
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
    if (!alloc_component_runtime_array(
            (void **)&child_inst->exports,
            component_inst->expression.without_args.inline_expr_len,
            sizeof(*child_inst->exports), error_buf, error_buf_size))
        return false;

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
            return false;
    }

    runtime_inst->owned_instance_count++;

    memset(&ref, 0, sizeof(ref));
    ref.type = WASM_COMP_RUNTIME_REF_INSTANCE;
    ref.of.instance = child_inst;
    return append_nested_component_local_ref(bindings, ref, error_buf,
                                             error_buf_size);
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
        WASMComponentRuntimeRef ref;
        WASMComponentRuntimeRefType expected_type;
        const char *name;

        if (alias_def->alias_target_type != WASM_COMP_ALIAS_TARGET_EXPORT)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "nested component aliases other than export are not "
                "supported yet");

        if (!alias_def->sort || alias_def->sort->sort == WASM_COMP_SORT_CORE_SORT)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "nested component aliases must use component func/instance "
                "sorts");

        if (alias_def->target.exported.instance_idx >= bindings->instance_count)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "nested component alias instance index %u is out of bounds",
                alias_def->target.exported.instance_idx);

        switch (alias_def->sort->sort) {
            case WASM_COMP_SORT_FUNC:
                expected_type = WASM_COMP_RUNTIME_REF_FUNC;
                break;
            case WASM_COMP_SORT_INSTANCE:
                expected_type = WASM_COMP_RUNTIME_REF_INSTANCE;
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
                                              error_buf_size)
            || !append_nested_component_local_ref(bindings, ref, error_buf,
                                                  error_buf_size))
            return false;
    }

    return true;
}

static bool
instantiate_nested_component_instance(WASMComponentInstance *inst,
                                      const WASMComponentInst *component_inst,
                                      char *error_buf, uint32 error_buf_size)
{
    WASMComponentRuntimeInstance *runtime_inst =
        &inst->component_instances[inst->component_instance_count];
    const WASMComponent *nested_component;
    WASMNestedComponentLocalBindings bindings;
    uint32 i, import_count = 0, owned_instance_count = 0, export_count = 0;

    if (component_inst->expression.with_args.idx >= inst->component_count)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "nested component index %u is out of bounds",
            component_inst->expression.with_args.idx);

    nested_component = inst->components[component_inst->expression.with_args.idx];
    memset(&bindings, 0, sizeof(bindings));
    if (!count_nested_component_local_bindings(
            nested_component, &import_count, &bindings.func_capacity,
            &bindings.instance_capacity, &owned_instance_count, &export_count,
            error_buf,
            error_buf_size))
        return false;

    if (component_inst->expression.with_args.arg_len != import_count)
        return set_component_runtime_error_fmt(
            error_buf, error_buf_size,
            "nested component import binding count mismatch");

    if (!alloc_nested_component_local_bindings(
            &bindings, bindings.func_capacity, bindings.instance_capacity,
            error_buf, error_buf_size)
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

    for (i = 0; i < nested_component->section_count; i++) {
        const WASMComponentSection *section = &nested_component->sections[i];

        if (section->id == WASM_COMP_SECTION_IMPORTS) {
            uint32 j;
            const WASMComponentImportSection *import_section =
                section->parsed.import_section;

            for (j = 0; j < import_section->count; j++) {
                const WASMComponentImport *component_import =
                    &import_section->imports[j];
                const char *import_name =
                    get_component_import_name(component_import->import_name);
                WASMComponentRuntimeRef ref;
                bool matched = false;
                uint32 k;

                if (!component_import->extern_desc) {
                    set_component_runtime_error_fmt(
                        error_buf, error_buf_size,
                        "nested component import \"%s\" is missing an "
                        "external descriptor",
                        import_name);
                    goto fail;
                }

                switch (component_import->extern_desc->type) {
                    case WASM_COMP_EXTERN_FUNC:
                    case WASM_COMP_EXTERN_INSTANCE:
                        break;
                    default:
                        set_component_runtime_error_fmt(
                            error_buf, error_buf_size,
                            "nested component imports other than func/instance "
                            "are not supported yet");
                        goto fail;
                }

                for (k = 0; k < component_inst->expression.with_args.arg_len;
                     k++) {
                    const WASMComponentInstArg *arg =
                        &component_inst->expression.with_args.args[k];

                    if (strcmp(arg->name->name, import_name))
                        continue;

                    if (!resolve_component_sort_idx(inst, arg->idx.sort_idx,
                                                    &ref, error_buf,
                                                    error_buf_size))
                        goto fail;

                    if ((component_import->extern_desc->type
                         == WASM_COMP_EXTERN_FUNC)
                        != (ref.type == WASM_COMP_RUNTIME_REF_FUNC)) {
                        set_component_runtime_error_fmt(
                            error_buf, error_buf_size,
                            "nested component import \"%s\" bound to the wrong "
                            "runtime sort",
                            import_name);
                        goto fail;
                    }

                    if (!append_nested_component_local_ref(&bindings, ref,
                                                           error_buf,
                                                           error_buf_size))
                        goto fail;

                    matched = true;
                    break;
                }

                if (!matched) {
                    set_component_runtime_error_fmt(
                        error_buf, error_buf_size,
                        "nested component import \"%s\" is missing a binding",
                        import_name);
                    goto fail;
                }
            }
        }
        else if (section->id == WASM_COMP_SECTION_ALIASES) {
            if (!resolve_nested_component_alias_section(
                    &bindings, section->parsed.alias_section, error_buf,
                    error_buf_size))
                goto fail;
        }
        else if (section->id == WASM_COMP_SECTION_INSTANCES) {
            uint32 j;
            const WASMComponentInstSection *inst_section =
                section->parsed.instance_section;

            for (j = 0; j < inst_section->count; j++) {
                if (!instantiate_nested_component_inline_instance(
                        runtime_inst, &bindings, &inst_section->instances[j],
                        error_buf, error_buf_size))
                    goto fail;
            }
        }
        else if (section->id == WASM_COMP_SECTION_EXPORTS) {
            if (!resolve_nested_component_exports(
                    runtime_inst, section->parsed.export_section, &bindings,
                    error_buf, error_buf_size))
                goto fail;
        }
    }

    free_nested_component_local_bindings(&bindings);
    inst->component_instance_count++;
    return true;

fail:
    free_nested_component_local_bindings(&bindings);
    destroy_component_runtime_instance(runtime_inst);
    return false;
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
        inst->component_export_count++;
    }

    return true;
}

static bool
build_component_instance_graph(WASMComponentInstance *inst, char *error_buf,
                               uint32 error_buf_size)
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
                inst->components[inst->component_count++] =
                    section->parsed.component;
                break;
            case WASM_COMP_SECTION_IMPORTS:
                if (section->parsed.import_section->count > 0)
                    return set_component_runtime_error_fmt(
                        error_buf, error_buf_size,
                        "top-level component imports are not supported yet");
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
wasm_component_module_instantiate(WASMComponentModule *module, char *error_buf,
                                  uint32 error_buf_size)
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
        || !build_component_instance_graph(inst, error_buf, error_buf_size)) {
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
