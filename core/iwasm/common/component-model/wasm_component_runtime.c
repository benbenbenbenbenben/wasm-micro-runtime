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
} WASMComponentRuntimeAllocCounts;

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
                counts->alias_count += section->parsed.alias_section->count;
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

    inst->core_module_count = 0;
    inst->core_instance_count = 0;
    inst->core_func_count = 0;
    inst->core_table_count = 0;
    inst->core_memory_count = 0;
    inst->core_global_count = 0;
    inst->resolved_alias_count = 0;
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
        const WASMComponentCoreRuntimeInstance *core_instance;
        WASMComponentCoreRuntimeRef ref;
        WASMComponentCoreRuntimeRefType expected_type;
        const char *name;

        if (alias_def->alias_target_type != WASM_COMP_ALIAS_TARGET_CORE_EXPORT)
            continue;

        if (!alias_def->sort || alias_def->sort->sort != WASM_COMP_SORT_CORE_SORT)
            return set_component_runtime_error_fmt(
                error_buf, error_buf_size,
                "non-core alias sorts are not supported yet");

        if (alias_def->target.core_exported.instance_idx >= inst->core_instance_count)
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

        core_instance =
            &inst->core_instances[alias_def->target.core_exported.instance_idx];
        name = alias_def->target.core_exported.name->name;
        if (!lookup_core_instance_export(core_instance, name, expected_type, &ref,
                                         error_buf, error_buf_size)
            || !append_core_alias(inst, name, alias_def, ref, error_buf,
                                  error_buf_size))
            return false;
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
