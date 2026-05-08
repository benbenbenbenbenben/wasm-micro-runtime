/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <gtest/gtest.h>
#include "helpers.h"
#include <vector>
#include <memory>
#include <cstdio>
#include <cstring>
#include <string>
#include "bh_read_file.h"
#include "wasm_component_runtime.h"

static std::vector<std::string> component_files = {
    "add.wasm",
    "complex_with_host.wasm",
    "complex.wasm",
    "logging_service.component.wasm",
    "processor_and_logging_merged_wac_plug.wasm",
    "processor_service.component.wasm",
    "sampletypes.wasm"
};

static WASMComponentCoreName *
clone_core_name(const char *name)
{
    const size_t len = strlen(name);
    auto *core_name = (WASMComponentCoreName *)wasm_runtime_malloc(
        sizeof(WASMComponentCoreName));
    if (!core_name) {
        return nullptr;
    }

    core_name->name = (char *)wasm_runtime_malloc((uint32_t)len + 1);
    if (!core_name->name) {
        wasm_runtime_free(core_name);
        return nullptr;
    }

    memcpy(core_name->name, name, len + 1);
    core_name->name_len = (uint32_t)len;
    return core_name;
}

static WASMComponentSort *
create_sort(uint8_t sort)
{
    auto *comp_sort =
        (WASMComponentSort *)wasm_runtime_malloc(sizeof(WASMComponentSort));
    if (!comp_sort) {
        return nullptr;
    }

    comp_sort->sort = sort;
    comp_sort->core_sort = 0;
    return comp_sort;
}

static WASMComponentSortIdx *
create_sort_idx(uint8_t sort, uint32_t idx)
{
    auto *sort_idx =
        (WASMComponentSortIdx *)wasm_runtime_malloc(sizeof(WASMComponentSortIdx));
    if (!sort_idx) {
        return nullptr;
    }

    sort_idx->sort = create_sort(sort);
    if (!sort_idx->sort) {
        wasm_runtime_free(sort_idx);
        return nullptr;
    }

    sort_idx->idx = idx;
    return sort_idx;
}

static WASMComponentSortIdx *
create_core_sort_idx(uint8_t core_sort, uint32_t idx)
{
    auto *sort_idx = create_sort_idx(WASM_COMP_SORT_CORE_SORT, idx);
    if (!sort_idx) {
        return nullptr;
    }

    sort_idx->sort->core_sort = core_sort;
    return sort_idx;
}

static WASMComponentExportName *
create_export_name(const char *name)
{
    auto *export_name = (WASMComponentExportName *)wasm_runtime_malloc(
        sizeof(WASMComponentExportName));
    if (!export_name) {
        return nullptr;
    }

    memset(export_name, 0, sizeof(WASMComponentExportName));
    export_name->tag = WASM_COMP_IMPORTNAME_SIMPLE;
    export_name->exported.simple.name = clone_core_name(name);
    if (!export_name->exported.simple.name) {
        wasm_runtime_free(export_name);
        return nullptr;
    }

    return export_name;
}

static WASMComponentImportName *
create_import_name(const char *name)
{
    auto *import_name = (WASMComponentImportName *)wasm_runtime_malloc(
        sizeof(WASMComponentImportName));
    if (!import_name) {
        return nullptr;
    }

    memset(import_name, 0, sizeof(WASMComponentImportName));
    import_name->tag = WASM_COMP_IMPORTNAME_SIMPLE;
    import_name->imported.simple.name = clone_core_name(name);
    if (!import_name->imported.simple.name) {
        wasm_runtime_free(import_name);
        return nullptr;
    }

    return import_name;
}

static WASMComponentExternDesc *
create_extern_desc(WASMComponentExternDescType type)
{
    auto *extern_desc = (WASMComponentExternDesc *)wasm_runtime_malloc(
        sizeof(WASMComponentExternDesc));
    if (!extern_desc) {
        return nullptr;
    }

    memset(extern_desc, 0, sizeof(WASMComponentExternDesc));
    extern_desc->type = type;
    return extern_desc;
}

static WASMComponentExternDesc *
create_primitive_value_extern_desc(WASMComponentPrimValType primitive_type)
{
    auto *extern_desc = create_extern_desc(WASM_COMP_EXTERN_VALUE);
    if (!extern_desc) {
        return nullptr;
    }

    extern_desc->extern_desc.value.value_bound =
        (WASMComponentValueBound *)wasm_runtime_malloc(
            sizeof(WASMComponentValueBound));
    if (!extern_desc->extern_desc.value.value_bound) {
        wasm_runtime_free(extern_desc);
        return nullptr;
    }
    memset(extern_desc->extern_desc.value.value_bound, 0,
           sizeof(WASMComponentValueBound));
    extern_desc->extern_desc.value.value_bound->tag = WASM_COMP_VALUEBOUND_TYPE;
    extern_desc->extern_desc.value.value_bound->bound.value_type =
        (WASMComponentValueType *)wasm_runtime_malloc(
            sizeof(WASMComponentValueType));
    if (!extern_desc->extern_desc.value.value_bound->bound.value_type) {
        wasm_runtime_free(extern_desc->extern_desc.value.value_bound);
        wasm_runtime_free(extern_desc);
        return nullptr;
    }
    memset(extern_desc->extern_desc.value.value_bound->bound.value_type, 0,
           sizeof(WASMComponentValueType));
    extern_desc->extern_desc.value.value_bound->bound.value_type->type =
        WASM_COMP_VAL_TYPE_PRIMVAL;
    extern_desc->extern_desc.value.value_bound->bound.value_type->type_specific
        .primval_type = primitive_type;
    return extern_desc;
}

static WASMComponentTypeBound *
create_type_bound(WASMComponentTypeBoundTag tag, uint32_t type_idx)
{
    auto *type_bound = (WASMComponentTypeBound *)wasm_runtime_malloc(
        sizeof(WASMComponentTypeBound));
    if (!type_bound) {
        return nullptr;
    }

    type_bound->tag = tag;
    type_bound->type_idx = type_idx;
    return type_bound;
}

static bool
init_resource_type_section(WASMComponentTypeSection *type_section, uint32_t count,
                           bool has_dtor)
{
    if (!type_section || count == 0) {
        return false;
    }

    type_section->count = count;
    type_section->types =
        (WASMComponentTypes *)wasm_runtime_malloc(sizeof(WASMComponentTypes) * count);
    if (!type_section->types) {
        return false;
    }
    memset(type_section->types, 0, sizeof(WASMComponentTypes) * count);

    for (uint32_t i = 0; i < count; i++) {
        auto *resource_type = (WASMComponentResourceType *)wasm_runtime_malloc(
            sizeof(WASMComponentResourceType));
        auto *sync_type = (WASMComponentResourceTypeSync *)wasm_runtime_malloc(
            sizeof(WASMComponentResourceTypeSync));
        if (!resource_type || !sync_type) {
            return false;
        }

        memset(resource_type, 0, sizeof(WASMComponentResourceType));
        memset(sync_type, 0, sizeof(WASMComponentResourceTypeSync));
        sync_type->has_dtor = has_dtor;
        sync_type->dtor_func_idx = 0;
        resource_type->tag = WASM_COMP_RESOURCE_TYPE_SYNC;
        resource_type->resource.sync = sync_type;
        type_section->types[i].tag = WASM_COMP_RESOURCE_TYPE_SYNC;
        type_section->types[i].type.resource_type = resource_type;
    }

    return true;
}

static bool
append_top_level_resource_type_section(WASMComponentModule *component_module)
{
    WASMComponent *component = &component_module->component;
    const uint32_t old_count = component->section_count;
    auto *new_sections = (WASMComponentSection *)wasm_runtime_malloc(
        sizeof(WASMComponentSection) * (old_count + 1));
    if (!new_sections) {
        return false;
    }

    memset(new_sections, 0, sizeof(WASMComponentSection) * (old_count + 1));
    memcpy(new_sections, component->sections,
           sizeof(WASMComponentSection) * old_count);
    wasm_runtime_free(component->sections);
    component->sections = new_sections;
    component->section_count = old_count + 1;

    auto *type_section = &component->sections[old_count];
    type_section->id = WASM_COMP_SECTION_TYPE;
    type_section->parsed.type_section =
        (WASMComponentTypeSection *)wasm_runtime_malloc(
            sizeof(WASMComponentTypeSection));
    if (!type_section->parsed.type_section) {
        return false;
    }
    memset(type_section->parsed.type_section, 0, sizeof(WASMComponentTypeSection));
    return init_resource_type_section(type_section->parsed.type_section, 1, false);
}

static bool
append_nested_resource_component_instance_sections(
    WASMComponentModule *component_module)
{
    WASMComponent *component = &component_module->component;
    uint32_t component_idx = 0;
    const uint32_t old_count = component->section_count;
    auto *new_sections = (WASMComponentSection *)wasm_runtime_malloc(
        sizeof(WASMComponentSection) * (old_count + 2));
    if (!new_sections) {
        return false;
    }

    for (uint32_t i = 0; i < old_count; i++) {
        if (component->sections[i].id == WASM_COMP_SECTION_COMPONENT) {
            component_idx++;
        }
    }

    memset(new_sections, 0, sizeof(WASMComponentSection) * (old_count + 2));
    memcpy(new_sections, component->sections,
           sizeof(WASMComponentSection) * old_count);
    wasm_runtime_free(component->sections);
    component->sections = new_sections;
    component->section_count = old_count + 2;

    auto *component_section = &component->sections[old_count];
    auto *instance_section = &component->sections[old_count + 1];

    component_section->id = WASM_COMP_SECTION_COMPONENT;
    component_section->parsed.component =
        (WASMComponent *)wasm_runtime_malloc(sizeof(WASMComponent));
    if (!component_section->parsed.component) {
        return false;
    }
    memset(component_section->parsed.component, 0, sizeof(WASMComponent));
    component_section->parsed.component->section_count = 1;
    component_section->parsed.component->sections =
        (WASMComponentSection *)wasm_runtime_malloc(sizeof(WASMComponentSection));
    if (!component_section->parsed.component->sections) {
        return false;
    }
    memset(component_section->parsed.component->sections, 0,
           sizeof(WASMComponentSection));
    component_section->parsed.component->sections[0].id = WASM_COMP_SECTION_TYPE;
    component_section->parsed.component->sections[0].parsed.type_section =
        (WASMComponentTypeSection *)wasm_runtime_malloc(
            sizeof(WASMComponentTypeSection));
    if (!component_section->parsed.component->sections[0].parsed.type_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[0].parsed.type_section, 0,
           sizeof(WASMComponentTypeSection));
    if (!init_resource_type_section(
            component_section->parsed.component->sections[0].parsed.type_section, 1,
            false)) {
        return false;
    }

    instance_section->id = WASM_COMP_SECTION_INSTANCES;
    instance_section->parsed.instance_section =
        (WASMComponentInstSection *)wasm_runtime_malloc(
            sizeof(WASMComponentInstSection));
    if (!instance_section->parsed.instance_section) {
        return false;
    }
    memset(instance_section->parsed.instance_section, 0,
           sizeof(WASMComponentInstSection));
    instance_section->parsed.instance_section->count = 1;
    instance_section->parsed.instance_section->instances =
        (WASMComponentInst *)wasm_runtime_malloc(sizeof(WASMComponentInst));
    if (!instance_section->parsed.instance_section->instances) {
        return false;
    }
    memset(instance_section->parsed.instance_section->instances, 0,
           sizeof(WASMComponentInst));
    instance_section->parsed.instance_section->instances[0]
        .instance_expression_tag = WASM_COMP_INSTANCE_EXPRESSION_WITH_ARGS;
    instance_section->parsed.instance_section->instances[0]
        .expression.with_args.idx = component_idx;
    instance_section->parsed.instance_section->instances[0]
        .expression.with_args.arg_len = 0;
    instance_section->parsed.instance_section->instances[0]
        .expression.with_args.args = nullptr;
    return true;
}

static WASMComponent *
create_empty_component()
{
    auto *component = (WASMComponent *)wasm_runtime_malloc(sizeof(WASMComponent));
    if (!component) {
        return nullptr;
    }

    memset(component, 0, sizeof(WASMComponent));
    return component;
}

static const uint8_t runtime_value_section_i32_bytes[] = { 0x2a, 0x00, 0x00, 0x00 };
static const uint8_t runtime_value_section_i32_two_bytes[] = { 0x02 };
static const uint8_t runtime_value_section_i32_three_bytes[] = { 0x03 };
static const uint8_t runtime_value_section_i32_five_bytes[] = { 0x05 };

static wasm_component_value_t
make_component_string_value(const std::string &value)
{
    wasm_component_value_t result = {};
    uint8_t *storage = (uint8_t *)wasm_runtime_malloc((uint32_t)value.size() + 5);
    uint32_t len = (uint32_t)value.size();
    uint32_t leb_len = 0;

    EXPECT_NE(storage, nullptr);
    if (!storage) {
        return result;
    }

    result.type.kind = WASM_COMPONENT_VALUE_TYPE_PRIMITIVE;
    result.type.type.primitive_type = WASM_COMPONENT_PRIMITIVE_VALUE_STRING;
    do {
        uint8_t byte = (uint8_t)(len & 0x7F);
        len >>= 7;
        if (len != 0) {
            byte |= 0x80;
        }
        storage[leb_len++] = byte;
    } while (len != 0);

    memcpy(storage + leb_len, value.data(), value.size());
    result.storage_kind = WASM_COMPONENT_VALUE_STORAGE_OWNED;
    result.byte_size = leb_len + (uint32_t)value.size();
    result.storage.owned_data = storage;
    return result;
}

static wasm_module_inst_t
instantiate_component_with_default_wasi(wasm_module_t module,
                                        ComponentHelper *helper)
{
    struct InstantiationArgs2 *inst_args = nullptr;
    wasm_module_inst_t module_inst = nullptr;

    if (!wasm_runtime_instantiation_args_create(&inst_args)) {
        return nullptr;
    }

    wasm_runtime_instantiation_args_set_default_stack_size(inst_args,
                                                           helper->stack_size);
    wasm_runtime_instantiation_args_set_host_managed_heap_size(
        inst_args, helper->heap_size);
    wasm_runtime_instantiation_args_set_wasi_stdio(inst_args, 0, 1, 2);
    wasm_runtime_instantiation_args_set_wasi_dir(inst_args, nullptr, 0, nullptr,
                                                 0);

    module_inst = wasm_runtime_instantiate_ex2(
        module, inst_args, helper->error_buf,
        (uint32_t)sizeof(helper->error_buf));
    wasm_runtime_instantiation_args_destroy(inst_args);
    return module_inst;
}

struct RuntimeValueFinalizerState {
    uint32_t call_count;
};

static void
runtime_value_test_finalizer(void *data, void *ctx)
{
    auto *state = (RuntimeValueFinalizerState *)ctx;
    if (state) {
        state->call_count++;
    }
    wasm_runtime_free(data);
}

static void
resource_handle_test_finalizer(void *data, void *ctx)
{
    auto *state = (RuntimeValueFinalizerState *)ctx;
    if (state) {
        state->call_count++;
    }
    wasm_runtime_free(data);
}

static bool
init_scalar_value_section_with_bytes(WASMComponentValueSection *value_section,
                                     const uint8_t *bytes, uint32_t byte_len)
{
    if (!value_section) {
        return false;
    }

    value_section->count = 1;
    value_section->values =
        (WASMComponentValue *)wasm_runtime_malloc(sizeof(WASMComponentValue));
    if (!value_section->values) {
        return false;
    }

    memset(value_section->values, 0, sizeof(WASMComponentValue));
    value_section->values[0].val_type = (WASMComponentValueType *)wasm_runtime_malloc(
        sizeof(WASMComponentValueType));
    if (!value_section->values[0].val_type) {
        return false;
    }

    value_section->values[0].val_type->type = WASM_COMP_VAL_TYPE_PRIMVAL;
    value_section->values[0].val_type->type_specific.primval_type =
        WASM_COMP_PRIMVAL_S32;
    value_section->values[0].core_data_len = byte_len;
    value_section->values[0].core_data = bytes;
    return true;
}

static bool
init_scalar_value_section(WASMComponentValueSection *value_section)
{
    return init_scalar_value_section_with_bytes(
        value_section, runtime_value_section_i32_bytes,
        (uint32_t)sizeof(runtime_value_section_i32_bytes));
}

static uint32_t
count_top_level_sort_entries(const WASMComponent *component, uint8_t sort)
{
    uint32_t count = 0;

    if (!component) {
        return 0;
    }

    for (uint32_t i = 0; i < component->section_count; i++) {
        const WASMComponentSection *section = &component->sections[i];

        switch (section->id) {
            case WASM_COMP_SECTION_IMPORTS:
                for (uint32_t j = 0; j < section->parsed.import_section->count; j++) {
                    const WASMComponentImport *component_import =
                        &section->parsed.import_section->imports[j];
                    if (!component_import->extern_desc) {
                        continue;
                    }
                    if ((sort == WASM_COMP_SORT_FUNC
                         && component_import->extern_desc->type
                                == WASM_COMP_EXTERN_FUNC)
                        || (sort == WASM_COMP_SORT_VALUE
                            && component_import->extern_desc->type
                                   == WASM_COMP_EXTERN_VALUE)
                        || (sort == WASM_COMP_SORT_INSTANCE
                            && component_import->extern_desc->type
                                   == WASM_COMP_EXTERN_INSTANCE)
                        || (sort == WASM_COMP_SORT_COMPONENT
                            && component_import->extern_desc->type
                                   == WASM_COMP_EXTERN_COMPONENT)) {
                        count++;
                    }
                }
                break;
            case WASM_COMP_SECTION_ALIASES:
                for (uint32_t j = 0; j < section->parsed.alias_section->count; j++) {
                    const WASMComponentAliasDefinition *alias_def =
                        &section->parsed.alias_section->aliases[j];
                    if (alias_def->alias_target_type == WASM_COMP_ALIAS_TARGET_EXPORT
                        && alias_def->sort && alias_def->sort->sort == sort) {
                        count++;
                    }
                }
                break;
            case WASM_COMP_SECTION_COMPONENT:
                if (sort == WASM_COMP_SORT_COMPONENT) {
                    count++;
                }
                break;
            case WASM_COMP_SECTION_INSTANCES:
                if (sort == WASM_COMP_SORT_INSTANCE) {
                    count += section->parsed.instance_section->count;
                }
                break;
            case WASM_COMP_SECTION_CANONS:
                if (sort == WASM_COMP_SORT_FUNC) {
                    count += section->parsed.canon_section->count;
                }
                break;
            case WASM_COMP_SECTION_VALUES:
                if (sort == WASM_COMP_SORT_VALUE) {
                    count += section->parsed.value_section->count;
                }
                break;
            default:
                break;
        }
    }

    return count;
}

static WASMComponentSection *
find_component_section(WASMComponentModule *component_module,
                       WASMComponentSectionType section_type)
{
    WASMComponent *component = &component_module->component;

    for (uint32_t i = 0; i < component->section_count; i++) {
        if (component->sections[i].id == section_type) {
            return &component->sections[i];
        }
    }

    return nullptr;
}

static WASMComponentCanon *
find_first_canon_lift(WASMComponentModule *component_module)
{
    auto *section =
        find_component_section(component_module, WASM_COMP_SECTION_CANONS);
    if (!section || !section->parsed.canon_section) {
        return nullptr;
    }

    for (uint32_t i = 0; i < section->parsed.canon_section->count; i++) {
        if (section->parsed.canon_section->canons[i].tag == WASM_COMP_CANON_LIFT) {
            return &section->parsed.canon_section->canons[i];
        }
    }

    return nullptr;
}

static WASMComponentFuncType *
lookup_local_component_func_type(WASMComponentModule *component_module,
                                 uint32_t type_idx)
{
    WASMComponent *component = &component_module->component;

    for (uint32_t i = 0; i < component->section_count; i++) {
        WASMComponentSection *section = &component->sections[i];

        if (section->id != WASM_COMP_SECTION_TYPE || !section->parsed.type_section) {
            continue;
        }

        if (type_idx < section->parsed.type_section->count) {
            WASMComponentTypes *type_entry =
                &section->parsed.type_section->types[type_idx];
            return type_entry->tag == WASM_COMP_FUNC_TYPE
                       ? type_entry->type.func_type
                       : nullptr;
        }

        type_idx -= section->parsed.type_section->count;
    }

    return nullptr;
}

static int32_t
find_top_level_export_sort_index(WASMComponentModule *component_module,
                                 const char *export_name, uint8_t sort)
{
    auto *section =
        find_component_section(component_module, WASM_COMP_SECTION_EXPORTS);
    if (!section || !section->parsed.export_section) {
        return -1;
    }

    for (uint32_t i = 0; i < section->parsed.export_section->count; i++) {
        auto *component_export = &section->parsed.export_section->exports[i];
        if (!component_export->export_name || !component_export->sort_idx
            || !component_export->sort_idx->sort) {
            continue;
        }

        const char *name = component_export->export_name->tag
                                   == WASM_COMP_IMPORTNAME_SIMPLE
                               ? component_export->export_name->exported.simple.name
                                     ->name
                               : nullptr;
        if (name && !strcmp(name, export_name)
            && component_export->sort_idx->sort->sort == sort) {
            return (int32_t)component_export->sort_idx->idx;
        }
    }

    return -1;
}

static bool
append_top_level_function_export_alias(WASMComponentModule *component_module,
                                       const char *instance_export_name,
                                       const char *function_name,
                                       const char *top_export_name)
{
    WASMComponent *component = &component_module->component;
    const int32_t instance_idx = find_top_level_export_sort_index(
        component_module, instance_export_name, WASM_COMP_SORT_INSTANCE);
    const uint32_t old_count = component->section_count;
    auto *new_sections = (WASMComponentSection *)wasm_runtime_malloc(
        sizeof(WASMComponentSection) * (old_count + 2));
    if (!new_sections || instance_idx < 0) {
        return false;
    }

    memset(new_sections, 0, sizeof(WASMComponentSection) * (old_count + 2));
    memcpy(new_sections, component->sections,
           sizeof(WASMComponentSection) * old_count);
    wasm_runtime_free(component->sections);
    component->sections = new_sections;
    component->section_count = old_count + 2;

    auto *alias_section = &component->sections[old_count];
    auto *export_section = &component->sections[old_count + 1];

    alias_section->id = WASM_COMP_SECTION_ALIASES;
    alias_section->parsed.alias_section =
        (WASMComponentAliasSection *)wasm_runtime_malloc(
            sizeof(WASMComponentAliasSection));
    if (!alias_section->parsed.alias_section) {
        return false;
    }
    memset(alias_section->parsed.alias_section, 0,
           sizeof(WASMComponentAliasSection));
    alias_section->parsed.alias_section->count = 1;
    alias_section->parsed.alias_section->aliases =
        (WASMComponentAliasDefinition *)wasm_runtime_malloc(
            sizeof(WASMComponentAliasDefinition));
    if (!alias_section->parsed.alias_section->aliases) {
        return false;
    }
    memset(alias_section->parsed.alias_section->aliases, 0,
           sizeof(WASMComponentAliasDefinition));
    alias_section->parsed.alias_section->aliases[0].sort =
        create_sort(WASM_COMP_SORT_FUNC);
    alias_section->parsed.alias_section->aliases[0].alias_target_type =
        WASM_COMP_ALIAS_TARGET_EXPORT;
    alias_section->parsed.alias_section->aliases[0].target.exported.instance_idx =
        (uint32_t)instance_idx;
    alias_section->parsed.alias_section->aliases[0].target.exported.name =
        clone_core_name(function_name);
    if (!alias_section->parsed.alias_section->aliases[0].sort
        || !alias_section->parsed.alias_section->aliases[0]
                .target.exported.name) {
        return false;
    }

    export_section->id = WASM_COMP_SECTION_EXPORTS;
    export_section->parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!export_section->parsed.export_section) {
        return false;
    }
    memset(export_section->parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    export_section->parsed.export_section->count = 1;
    export_section->parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!export_section->parsed.export_section->exports) {
        return false;
    }
    memset(export_section->parsed.export_section->exports, 0,
           sizeof(WASMComponentExport));
    export_section->parsed.export_section->exports[0].export_name =
        create_export_name(top_export_name);
    export_section->parsed.export_section->exports[0].sort_idx =
        create_sort_idx(WASM_COMP_SORT_FUNC,
                        count_top_level_sort_entries(component,
                                                     WASM_COMP_SORT_FUNC));

    return export_section->parsed.export_section->exports[0].export_name
           && export_section->parsed.export_section->exports[0].sort_idx;
}

static bool
configure_first_canon_lift_for_utf8_string(WASMComponentModule *component_module,
                                           bool string_result)
{
    WASMComponentCanon *canon = find_first_canon_lift(component_module);
    WASMComponentFuncType *func_type = canon
                                           ? lookup_local_component_func_type(
                                                 component_module,
                                                 canon->canon_data.lift.type_idx)
                                           : nullptr;

    if (!canon || !func_type || !func_type->params || func_type->params->count == 0
        || !func_type->params->params[0].value_type || !func_type->results
        || func_type->results->tag != WASM_COMP_RESULT_LIST_WITH_TYPE
        || !func_type->results->results) {
        return false;
    }

    func_type->params->count = 1;
    func_type->params->params[0].value_type->type = WASM_COMP_VAL_TYPE_PRIMVAL;
    func_type->params->params[0].value_type->type_specific.primval_type =
        WASM_COMP_PRIMVAL_STRING;

    func_type->results->results->type = WASM_COMP_VAL_TYPE_PRIMVAL;
    func_type->results->results->type_specific.primval_type =
        string_result ? WASM_COMP_PRIMVAL_STRING : WASM_COMP_PRIMVAL_S32;

    canon->canon_data.lift.canon_opts =
        (WASMComponentCanonOpts *)wasm_runtime_malloc(
            sizeof(WASMComponentCanonOpts));
    if (!canon->canon_data.lift.canon_opts) {
        return false;
    }
    memset(canon->canon_data.lift.canon_opts, 0, sizeof(WASMComponentCanonOpts));
    canon->canon_data.lift.canon_opts->canon_opts_count = 3;
    canon->canon_data.lift.canon_opts->canon_opts =
        (WASMComponentCanonOpt *)wasm_runtime_malloc(
            sizeof(WASMComponentCanonOpt) * 3);
    if (!canon->canon_data.lift.canon_opts->canon_opts) {
        return false;
    }
    memset(canon->canon_data.lift.canon_opts->canon_opts, 0,
           sizeof(WASMComponentCanonOpt) * 3);
    canon->canon_data.lift.canon_opts->canon_opts[0].tag =
        WASM_COMP_CANON_OPT_STRING_UTF8;
    canon->canon_data.lift.canon_opts->canon_opts[1].tag =
        WASM_COMP_CANON_OPT_MEMORY;
    canon->canon_data.lift.canon_opts->canon_opts[1].payload.memory.mem_idx = 0;
    canon->canon_data.lift.canon_opts->canon_opts[2].tag =
        WASM_COMP_CANON_OPT_REALLOC;
    canon->canon_data.lift.canon_opts->canon_opts[2].payload.realloc_opt.func_idx =
        2;
    return true;
}

static bool
append_component_export_alias_sections(WASMComponentModule *component_module)
{
    WASMComponent *component = &component_module->component;
    const uint32_t old_count = component->section_count;
    const uint32_t new_count = old_count + 3;
    auto *new_sections = (WASMComponentSection *)wasm_runtime_malloc(
        sizeof(WASMComponentSection) * new_count);
    if (!new_sections) {
        return false;
    }

    memset(new_sections, 0, sizeof(WASMComponentSection) * new_count);
    memcpy(new_sections, component->sections,
           sizeof(WASMComponentSection) * old_count);
    wasm_runtime_free(component->sections);
    component->sections = new_sections;
    component->section_count = new_count;

    WASMComponentSection *instance_section = &component->sections[old_count];
    WASMComponentSection *alias_section = &component->sections[old_count + 1];
    WASMComponentSection *export_section = &component->sections[old_count + 2];

    instance_section->id = WASM_COMP_SECTION_INSTANCES;
    instance_section->parsed.instance_section =
        (WASMComponentInstSection *)wasm_runtime_malloc(
            sizeof(WASMComponentInstSection));
    if (!instance_section->parsed.instance_section) {
        return false;
    }
    memset(instance_section->parsed.instance_section, 0,
           sizeof(WASMComponentInstSection));
    instance_section->parsed.instance_section->count = 1;
    instance_section->parsed.instance_section->instances =
        (WASMComponentInst *)wasm_runtime_malloc(sizeof(WASMComponentInst));
    if (!instance_section->parsed.instance_section->instances) {
        return false;
    }
    memset(instance_section->parsed.instance_section->instances, 0,
           sizeof(WASMComponentInst));
    instance_section->parsed.instance_section->instances[0]
        .instance_expression_tag = WASM_COMP_INSTANCE_EXPRESSION_WITHOUT_ARGS;
    instance_section->parsed.instance_section->instances[0]
        .expression.without_args.inline_expr_len = 1;
    instance_section->parsed.instance_section->instances[0]
        .expression.without_args.inline_expr =
        (WASMComponentInlineExport *)wasm_runtime_malloc(
            sizeof(WASMComponentInlineExport));
    if (!instance_section->parsed.instance_section->instances[0]
             .expression.without_args.inline_expr) {
        return false;
    }
    memset(instance_section->parsed.instance_section->instances[0]
               .expression.without_args.inline_expr,
           0, sizeof(WASMComponentInlineExport));
    instance_section->parsed.instance_section->instances[0]
        .expression.without_args.inline_expr[0]
        .name = clone_core_name("wrapped");
    instance_section->parsed.instance_section->instances[0]
        .expression.without_args.inline_expr[0]
        .sort_idx = create_sort_idx(WASM_COMP_SORT_INSTANCE, 0);
    if (!instance_section->parsed.instance_section->instances[0]
             .expression.without_args.inline_expr[0]
             .name
        || !instance_section->parsed.instance_section->instances[0]
                 .expression.without_args.inline_expr[0]
                 .sort_idx) {
        return false;
    }

    alias_section->id = WASM_COMP_SECTION_ALIASES;
    alias_section->parsed.alias_section =
        (WASMComponentAliasSection *)wasm_runtime_malloc(
            sizeof(WASMComponentAliasSection));
    if (!alias_section->parsed.alias_section) {
        return false;
    }
    memset(alias_section->parsed.alias_section, 0,
           sizeof(WASMComponentAliasSection));
    alias_section->parsed.alias_section->count = 2;
    alias_section->parsed.alias_section->aliases =
        (WASMComponentAliasDefinition *)wasm_runtime_malloc(
            sizeof(WASMComponentAliasDefinition) * 2);
    if (!alias_section->parsed.alias_section->aliases) {
        return false;
    }
    memset(alias_section->parsed.alias_section->aliases, 0,
           sizeof(WASMComponentAliasDefinition) * 2);
    alias_section->parsed.alias_section->aliases[0].sort =
        create_sort(WASM_COMP_SORT_FUNC);
    alias_section->parsed.alias_section->aliases[0].alias_target_type =
        WASM_COMP_ALIAS_TARGET_EXPORT;
    alias_section->parsed.alias_section->aliases[0].target.exported.instance_idx =
        0;
    alias_section->parsed.alias_section->aliases[0].target.exported.name =
        clone_core_name("add");
    alias_section->parsed.alias_section->aliases[1].sort =
        create_sort(WASM_COMP_SORT_INSTANCE);
    alias_section->parsed.alias_section->aliases[1].alias_target_type =
        WASM_COMP_ALIAS_TARGET_EXPORT;
    alias_section->parsed.alias_section->aliases[1].target.exported.instance_idx =
        1;
    alias_section->parsed.alias_section->aliases[1].target.exported.name =
        clone_core_name("wrapped");
    if (!alias_section->parsed.alias_section->aliases[0].sort
        || !alias_section->parsed.alias_section->aliases[0]
                .target.exported.name
        || !alias_section->parsed.alias_section->aliases[1].sort
        || !alias_section->parsed.alias_section->aliases[1]
                .target.exported.name) {
        return false;
    }

    export_section->id = WASM_COMP_SECTION_EXPORTS;
    export_section->parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!export_section->parsed.export_section) {
        return false;
    }
    memset(export_section->parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    export_section->parsed.export_section->count = 2;
    export_section->parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport) * 2);
    if (!export_section->parsed.export_section->exports) {
        return false;
    }
    memset(export_section->parsed.export_section->exports, 0,
           sizeof(WASMComponentExport) * 2);
    export_section->parsed.export_section->exports[0].export_name =
        create_export_name("aliased-add");
    export_section->parsed.export_section->exports[0].sort_idx =
        create_sort_idx(WASM_COMP_SORT_FUNC, 1);
    export_section->parsed.export_section->exports[1].export_name =
        create_export_name("aliased-instance");
    export_section->parsed.export_section->exports[1].sort_idx =
        create_sort_idx(WASM_COMP_SORT_INSTANCE, 2);
    return export_section->parsed.export_section->exports[0].export_name
           && export_section->parsed.export_section->exports[0].sort_idx
           && export_section->parsed.export_section->exports[1].export_name
           && export_section->parsed.export_section->exports[1].sort_idx;
}

static bool
append_nested_component_instance_reexport_sections(
    WASMComponentModule *component_module)
{
    WASMComponent *component = &component_module->component;
    const uint32_t old_count = component->section_count;
    const uint32_t new_count = old_count + 3;
    auto *new_sections = (WASMComponentSection *)wasm_runtime_malloc(
        sizeof(WASMComponentSection) * new_count);
    if (!new_sections) {
        return false;
    }

    memset(new_sections, 0, sizeof(WASMComponentSection) * new_count);
    memcpy(new_sections, component->sections,
           sizeof(WASMComponentSection) * old_count);
    wasm_runtime_free(component->sections);
    component->sections = new_sections;
    component->section_count = new_count;

    WASMComponentSection *component_section = &component->sections[old_count];
    WASMComponentSection *instance_section = &component->sections[old_count + 1];
    WASMComponentSection *export_section = &component->sections[old_count + 2];

    component_section->id = WASM_COMP_SECTION_COMPONENT;
    component_section->parsed.component =
        (WASMComponent *)wasm_runtime_malloc(sizeof(WASMComponent));
    if (!component_section->parsed.component) {
        return false;
    }
    memset(component_section->parsed.component, 0, sizeof(WASMComponent));
    component_section->parsed.component->section_count = 2;
    component_section->parsed.component->sections =
        (WASMComponentSection *)wasm_runtime_malloc(sizeof(WASMComponentSection) * 2);
    if (!component_section->parsed.component->sections) {
        return false;
    }
    memset(component_section->parsed.component->sections, 0,
           sizeof(WASMComponentSection) * 2);

    component_section->parsed.component->sections[0].id = WASM_COMP_SECTION_IMPORTS;
    component_section->parsed.component->sections[0].parsed.import_section =
        (WASMComponentImportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentImportSection));
    if (!component_section->parsed.component->sections[0].parsed.import_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[0].parsed.import_section, 0,
           sizeof(WASMComponentImportSection));
    component_section->parsed.component->sections[0].parsed.import_section->count =
        1;
    component_section->parsed.component->sections[0].parsed.import_section->imports =
        (WASMComponentImport *)wasm_runtime_malloc(sizeof(WASMComponentImport));
    if (!component_section->parsed.component->sections[0]
             .parsed.import_section->imports) {
        return false;
    }
    memset(component_section->parsed.component->sections[0]
               .parsed.import_section->imports,
           0, sizeof(WASMComponentImport));
    component_section->parsed.component->sections[0]
        .parsed.import_section->imports[0]
        .import_name = create_import_name("source");
    component_section->parsed.component->sections[0]
        .parsed.import_section->imports[0]
        .extern_desc = create_extern_desc(WASM_COMP_EXTERN_INSTANCE);
    if (!component_section->parsed.component->sections[0]
             .parsed.import_section->imports[0]
             .import_name
        || !component_section->parsed.component->sections[0]
                 .parsed.import_section->imports[0]
                 .extern_desc) {
        return false;
    }

    component_section->parsed.component->sections[1].id = WASM_COMP_SECTION_EXPORTS;
    component_section->parsed.component->sections[1].parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!component_section->parsed.component->sections[1].parsed.export_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[1].parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    component_section->parsed.component->sections[1].parsed.export_section->count =
        1;
    component_section->parsed.component->sections[1].parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!component_section->parsed.component->sections[1]
             .parsed.export_section->exports) {
        return false;
    }
    memset(component_section->parsed.component->sections[1]
               .parsed.export_section->exports,
           0, sizeof(WASMComponentExport));
    component_section->parsed.component->sections[1]
        .parsed.export_section->exports[0]
        .export_name = create_export_name("forwarded");
    component_section->parsed.component->sections[1]
        .parsed.export_section->exports[0]
        .sort_idx = create_sort_idx(WASM_COMP_SORT_INSTANCE, 0);
    if (!component_section->parsed.component->sections[1]
             .parsed.export_section->exports[0]
             .export_name
        || !component_section->parsed.component->sections[1]
                 .parsed.export_section->exports[0]
                 .sort_idx) {
        return false;
    }

    instance_section->id = WASM_COMP_SECTION_INSTANCES;
    instance_section->parsed.instance_section =
        (WASMComponentInstSection *)wasm_runtime_malloc(
            sizeof(WASMComponentInstSection));
    if (!instance_section->parsed.instance_section) {
        return false;
    }
    memset(instance_section->parsed.instance_section, 0,
           sizeof(WASMComponentInstSection));
    instance_section->parsed.instance_section->count = 1;
    instance_section->parsed.instance_section->instances =
        (WASMComponentInst *)wasm_runtime_malloc(sizeof(WASMComponentInst));
    if (!instance_section->parsed.instance_section->instances) {
        return false;
    }
    memset(instance_section->parsed.instance_section->instances, 0,
           sizeof(WASMComponentInst));
    instance_section->parsed.instance_section->instances[0]
        .instance_expression_tag = WASM_COMP_INSTANCE_EXPRESSION_WITH_ARGS;
    instance_section->parsed.instance_section->instances[0].expression.with_args.idx =
        1;
    instance_section->parsed.instance_section->instances[0]
        .expression.with_args.arg_len = 1;
    instance_section->parsed.instance_section->instances[0]
        .expression.with_args.args =
        (WASMComponentInstArg *)wasm_runtime_malloc(sizeof(WASMComponentInstArg));
    if (!instance_section->parsed.instance_section->instances[0]
             .expression.with_args.args) {
        return false;
    }
    memset(instance_section->parsed.instance_section->instances[0]
               .expression.with_args.args,
           0, sizeof(WASMComponentInstArg));
    instance_section->parsed.instance_section->instances[0]
        .expression.with_args.args[0]
        .name = clone_core_name("source");
    instance_section->parsed.instance_section->instances[0]
        .expression.with_args.args[0]
        .idx.sort_idx = create_sort_idx(WASM_COMP_SORT_INSTANCE, 0);
    if (!instance_section->parsed.instance_section->instances[0]
             .expression.with_args.args[0]
             .name
        || !instance_section->parsed.instance_section->instances[0]
                 .expression.with_args.args[0]
                 .idx.sort_idx) {
        return false;
    }

    export_section->id = WASM_COMP_SECTION_EXPORTS;
    export_section->parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!export_section->parsed.export_section) {
        return false;
    }
    memset(export_section->parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    export_section->parsed.export_section->count = 1;
    export_section->parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!export_section->parsed.export_section->exports) {
        return false;
    }
    memset(export_section->parsed.export_section->exports, 0,
           sizeof(WASMComponentExport));
    export_section->parsed.export_section->exports[0].export_name =
        create_export_name("forwarded-instance");
    export_section->parsed.export_section->exports[0].sort_idx =
        create_sort_idx(WASM_COMP_SORT_INSTANCE, 1);
    return export_section->parsed.export_section->exports[0].export_name
           && export_section->parsed.export_section->exports[0].sort_idx;
}

static bool
append_nested_component_alias_export_sections(
    WASMComponentModule *component_module)
{
    WASMComponent *component = &component_module->component;
    const uint32_t old_count = component->section_count;
    const uint32_t new_count = old_count + 3;
    auto *new_sections = (WASMComponentSection *)wasm_runtime_malloc(
        sizeof(WASMComponentSection) * new_count);
    if (!new_sections) {
        return false;
    }

    memset(new_sections, 0, sizeof(WASMComponentSection) * new_count);
    memcpy(new_sections, component->sections,
           sizeof(WASMComponentSection) * old_count);
    wasm_runtime_free(component->sections);
    component->sections = new_sections;
    component->section_count = new_count;

    WASMComponentSection *component_section = &component->sections[old_count];
    WASMComponentSection *instance_section = &component->sections[old_count + 1];
    WASMComponentSection *export_section = &component->sections[old_count + 2];

    component_section->id = WASM_COMP_SECTION_COMPONENT;
    component_section->parsed.component =
        (WASMComponent *)wasm_runtime_malloc(sizeof(WASMComponent));
    if (!component_section->parsed.component) {
        return false;
    }
    memset(component_section->parsed.component, 0, sizeof(WASMComponent));
    component_section->parsed.component->section_count = 3;
    component_section->parsed.component->sections =
        (WASMComponentSection *)wasm_runtime_malloc(sizeof(WASMComponentSection) * 3);
    if (!component_section->parsed.component->sections) {
        return false;
    }
    memset(component_section->parsed.component->sections, 0,
           sizeof(WASMComponentSection) * 3);

    component_section->parsed.component->sections[0].id = WASM_COMP_SECTION_IMPORTS;
    component_section->parsed.component->sections[0].parsed.import_section =
        (WASMComponentImportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentImportSection));
    if (!component_section->parsed.component->sections[0].parsed.import_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[0].parsed.import_section, 0,
           sizeof(WASMComponentImportSection));
    component_section->parsed.component->sections[0].parsed.import_section->count =
        1;
    component_section->parsed.component->sections[0].parsed.import_section->imports =
        (WASMComponentImport *)wasm_runtime_malloc(sizeof(WASMComponentImport));
    if (!component_section->parsed.component->sections[0]
             .parsed.import_section->imports) {
        return false;
    }
    memset(component_section->parsed.component->sections[0]
               .parsed.import_section->imports,
           0, sizeof(WASMComponentImport));
    component_section->parsed.component->sections[0]
        .parsed.import_section->imports[0]
        .import_name = create_import_name("source");
    component_section->parsed.component->sections[0]
        .parsed.import_section->imports[0]
        .extern_desc = create_extern_desc(WASM_COMP_EXTERN_INSTANCE);
    if (!component_section->parsed.component->sections[0]
             .parsed.import_section->imports[0]
             .import_name
        || !component_section->parsed.component->sections[0]
                 .parsed.import_section->imports[0]
                 .extern_desc) {
        return false;
    }

    component_section->parsed.component->sections[1].id = WASM_COMP_SECTION_ALIASES;
    component_section->parsed.component->sections[1].parsed.alias_section =
        (WASMComponentAliasSection *)wasm_runtime_malloc(
            sizeof(WASMComponentAliasSection));
    if (!component_section->parsed.component->sections[1].parsed.alias_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[1].parsed.alias_section, 0,
           sizeof(WASMComponentAliasSection));
    component_section->parsed.component->sections[1].parsed.alias_section->count =
        1;
    component_section->parsed.component->sections[1].parsed.alias_section->aliases =
        (WASMComponentAliasDefinition *)wasm_runtime_malloc(
            sizeof(WASMComponentAliasDefinition));
    if (!component_section->parsed.component->sections[1]
             .parsed.alias_section->aliases) {
        return false;
    }
    memset(component_section->parsed.component->sections[1]
               .parsed.alias_section->aliases,
           0, sizeof(WASMComponentAliasDefinition));
    component_section->parsed.component->sections[1]
        .parsed.alias_section->aliases[0]
        .sort = create_sort(WASM_COMP_SORT_FUNC);
    component_section->parsed.component->sections[1]
        .parsed.alias_section->aliases[0]
        .alias_target_type = WASM_COMP_ALIAS_TARGET_EXPORT;
    component_section->parsed.component->sections[1]
        .parsed.alias_section->aliases[0]
        .target.exported.instance_idx = 0;
    component_section->parsed.component->sections[1]
        .parsed.alias_section->aliases[0]
        .target.exported.name = clone_core_name("add");
    if (!component_section->parsed.component->sections[1]
             .parsed.alias_section->aliases[0]
             .sort
        || !component_section->parsed.component->sections[1]
                 .parsed.alias_section->aliases[0]
                 .target.exported.name) {
        return false;
    }

    component_section->parsed.component->sections[2].id = WASM_COMP_SECTION_EXPORTS;
    component_section->parsed.component->sections[2].parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!component_section->parsed.component->sections[2].parsed.export_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[2].parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    component_section->parsed.component->sections[2].parsed.export_section->count =
        1;
    component_section->parsed.component->sections[2].parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!component_section->parsed.component->sections[2]
             .parsed.export_section->exports) {
        return false;
    }
    memset(component_section->parsed.component->sections[2]
               .parsed.export_section->exports,
           0, sizeof(WASMComponentExport));
    component_section->parsed.component->sections[2]
        .parsed.export_section->exports[0]
        .export_name = create_export_name("forwarded-add");
    component_section->parsed.component->sections[2]
        .parsed.export_section->exports[0]
        .sort_idx = create_sort_idx(WASM_COMP_SORT_FUNC, 0);
    if (!component_section->parsed.component->sections[2]
             .parsed.export_section->exports[0]
             .export_name
        || !component_section->parsed.component->sections[2]
                 .parsed.export_section->exports[0]
                 .sort_idx) {
        return false;
    }

    instance_section->id = WASM_COMP_SECTION_INSTANCES;
    instance_section->parsed.instance_section =
        (WASMComponentInstSection *)wasm_runtime_malloc(
            sizeof(WASMComponentInstSection));
    if (!instance_section->parsed.instance_section) {
        return false;
    }
    memset(instance_section->parsed.instance_section, 0,
           sizeof(WASMComponentInstSection));
    instance_section->parsed.instance_section->count = 1;
    instance_section->parsed.instance_section->instances =
        (WASMComponentInst *)wasm_runtime_malloc(sizeof(WASMComponentInst));
    if (!instance_section->parsed.instance_section->instances) {
        return false;
    }
    memset(instance_section->parsed.instance_section->instances, 0,
           sizeof(WASMComponentInst));
    instance_section->parsed.instance_section->instances[0]
        .instance_expression_tag = WASM_COMP_INSTANCE_EXPRESSION_WITH_ARGS;
    instance_section->parsed.instance_section->instances[0].expression.with_args.idx =
        1;
    instance_section->parsed.instance_section->instances[0]
        .expression.with_args.arg_len = 1;
    instance_section->parsed.instance_section->instances[0]
        .expression.with_args.args =
        (WASMComponentInstArg *)wasm_runtime_malloc(sizeof(WASMComponentInstArg));
    if (!instance_section->parsed.instance_section->instances[0]
             .expression.with_args.args) {
        return false;
    }
    memset(instance_section->parsed.instance_section->instances[0]
               .expression.with_args.args,
           0, sizeof(WASMComponentInstArg));
    instance_section->parsed.instance_section->instances[0]
        .expression.with_args.args[0]
        .name = clone_core_name("source");
    instance_section->parsed.instance_section->instances[0]
        .expression.with_args.args[0]
        .idx.sort_idx = create_sort_idx(WASM_COMP_SORT_INSTANCE, 0);
    if (!instance_section->parsed.instance_section->instances[0]
             .expression.with_args.args[0]
             .name
        || !instance_section->parsed.instance_section->instances[0]
                 .expression.with_args.args[0]
                 .idx.sort_idx) {
        return false;
    }

    export_section->id = WASM_COMP_SECTION_EXPORTS;
    export_section->parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!export_section->parsed.export_section) {
        return false;
    }
    memset(export_section->parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    export_section->parsed.export_section->count = 1;
    export_section->parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!export_section->parsed.export_section->exports) {
        return false;
    }
    memset(export_section->parsed.export_section->exports, 0,
           sizeof(WASMComponentExport));
    export_section->parsed.export_section->exports[0].export_name =
        create_export_name("aliased-nested-add");
    export_section->parsed.export_section->exports[0].sort_idx =
        create_sort_idx(WASM_COMP_SORT_INSTANCE, 1);
    return export_section->parsed.export_section->exports[0].export_name
           && export_section->parsed.export_section->exports[0].sort_idx;
}

static bool
append_nested_component_inline_instance_sections(
    WASMComponentModule *component_module)
{
    WASMComponent *component = &component_module->component;
    const uint32_t old_count = component->section_count;
    const uint32_t new_count = old_count + 3;
    auto *new_sections = (WASMComponentSection *)wasm_runtime_malloc(
        sizeof(WASMComponentSection) * new_count);
    if (!new_sections) {
        return false;
    }

    memset(new_sections, 0, sizeof(WASMComponentSection) * new_count);
    memcpy(new_sections, component->sections,
           sizeof(WASMComponentSection) * old_count);
    wasm_runtime_free(component->sections);
    component->sections = new_sections;
    component->section_count = new_count;

    WASMComponentSection *component_section = &component->sections[old_count];
    WASMComponentSection *instance_section = &component->sections[old_count + 1];
    WASMComponentSection *export_section = &component->sections[old_count + 2];

    component_section->id = WASM_COMP_SECTION_COMPONENT;
    component_section->parsed.component =
        (WASMComponent *)wasm_runtime_malloc(sizeof(WASMComponent));
    if (!component_section->parsed.component) {
        return false;
    }
    memset(component_section->parsed.component, 0, sizeof(WASMComponent));
    component_section->parsed.component->section_count = 3;
    component_section->parsed.component->sections =
        (WASMComponentSection *)wasm_runtime_malloc(sizeof(WASMComponentSection) * 3);
    if (!component_section->parsed.component->sections) {
        return false;
    }
    memset(component_section->parsed.component->sections, 0,
           sizeof(WASMComponentSection) * 3);

    component_section->parsed.component->sections[0].id = WASM_COMP_SECTION_IMPORTS;
    component_section->parsed.component->sections[0].parsed.import_section =
        (WASMComponentImportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentImportSection));
    if (!component_section->parsed.component->sections[0].parsed.import_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[0].parsed.import_section, 0,
           sizeof(WASMComponentImportSection));
    component_section->parsed.component->sections[0].parsed.import_section->count =
        1;
    component_section->parsed.component->sections[0].parsed.import_section->imports =
        (WASMComponentImport *)wasm_runtime_malloc(sizeof(WASMComponentImport));
    if (!component_section->parsed.component->sections[0]
             .parsed.import_section->imports) {
        return false;
    }
    memset(component_section->parsed.component->sections[0]
               .parsed.import_section->imports,
           0, sizeof(WASMComponentImport));
    component_section->parsed.component->sections[0]
        .parsed.import_section->imports[0]
        .import_name = create_import_name("source");
    component_section->parsed.component->sections[0]
        .parsed.import_section->imports[0]
        .extern_desc = create_extern_desc(WASM_COMP_EXTERN_FUNC);
    if (!component_section->parsed.component->sections[0]
             .parsed.import_section->imports[0]
             .import_name
        || !component_section->parsed.component->sections[0]
                 .parsed.import_section->imports[0]
                 .extern_desc) {
        return false;
    }

    component_section->parsed.component->sections[1].id = WASM_COMP_SECTION_INSTANCES;
    component_section->parsed.component->sections[1].parsed.instance_section =
        (WASMComponentInstSection *)wasm_runtime_malloc(
            sizeof(WASMComponentInstSection));
    if (!component_section->parsed.component->sections[1].parsed.instance_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[1].parsed.instance_section, 0,
           sizeof(WASMComponentInstSection));
    component_section->parsed.component->sections[1].parsed.instance_section->count =
        1;
    component_section->parsed.component->sections[1].parsed.instance_section->instances =
        (WASMComponentInst *)wasm_runtime_malloc(sizeof(WASMComponentInst));
    if (!component_section->parsed.component->sections[1]
             .parsed.instance_section->instances) {
        return false;
    }
    memset(component_section->parsed.component->sections[1]
               .parsed.instance_section->instances,
           0, sizeof(WASMComponentInst));
    component_section->parsed.component->sections[1]
        .parsed.instance_section->instances[0]
        .instance_expression_tag = WASM_COMP_INSTANCE_EXPRESSION_WITHOUT_ARGS;
    component_section->parsed.component->sections[1]
        .parsed.instance_section->instances[0]
        .expression.without_args.inline_expr_len = 1;
    component_section->parsed.component->sections[1]
        .parsed.instance_section->instances[0]
        .expression.without_args.inline_expr =
        (WASMComponentInlineExport *)wasm_runtime_malloc(
            sizeof(WASMComponentInlineExport));
    if (!component_section->parsed.component->sections[1]
             .parsed.instance_section->instances[0]
             .expression.without_args.inline_expr) {
        return false;
    }
    memset(component_section->parsed.component->sections[1]
               .parsed.instance_section->instances[0]
               .expression.without_args.inline_expr,
           0, sizeof(WASMComponentInlineExport));
    component_section->parsed.component->sections[1]
        .parsed.instance_section->instances[0]
        .expression.without_args.inline_expr[0]
        .name = clone_core_name("wrapped");
    component_section->parsed.component->sections[1]
        .parsed.instance_section->instances[0]
        .expression.without_args.inline_expr[0]
        .sort_idx = create_sort_idx(WASM_COMP_SORT_FUNC, 0);
    if (!component_section->parsed.component->sections[1]
             .parsed.instance_section->instances[0]
             .expression.without_args.inline_expr[0]
             .name
        || !component_section->parsed.component->sections[1]
                 .parsed.instance_section->instances[0]
                 .expression.without_args.inline_expr[0]
                 .sort_idx) {
        return false;
    }

    component_section->parsed.component->sections[2].id = WASM_COMP_SECTION_EXPORTS;
    component_section->parsed.component->sections[2].parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!component_section->parsed.component->sections[2].parsed.export_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[2].parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    component_section->parsed.component->sections[2].parsed.export_section->count =
        1;
    component_section->parsed.component->sections[2].parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!component_section->parsed.component->sections[2]
             .parsed.export_section->exports) {
        return false;
    }
    memset(component_section->parsed.component->sections[2]
               .parsed.export_section->exports,
           0, sizeof(WASMComponentExport));
    component_section->parsed.component->sections[2]
        .parsed.export_section->exports[0]
        .export_name = create_export_name("wrapped-instance");
    component_section->parsed.component->sections[2]
        .parsed.export_section->exports[0]
        .sort_idx = create_sort_idx(WASM_COMP_SORT_INSTANCE, 0);
    if (!component_section->parsed.component->sections[2]
             .parsed.export_section->exports[0]
             .export_name
        || !component_section->parsed.component->sections[2]
                 .parsed.export_section->exports[0]
                 .sort_idx) {
        return false;
    }

    instance_section->id = WASM_COMP_SECTION_INSTANCES;
    instance_section->parsed.instance_section =
        (WASMComponentInstSection *)wasm_runtime_malloc(
            sizeof(WASMComponentInstSection));
    if (!instance_section->parsed.instance_section) {
        return false;
    }
    memset(instance_section->parsed.instance_section, 0,
           sizeof(WASMComponentInstSection));
    instance_section->parsed.instance_section->count = 1;
    instance_section->parsed.instance_section->instances =
        (WASMComponentInst *)wasm_runtime_malloc(sizeof(WASMComponentInst));
    if (!instance_section->parsed.instance_section->instances) {
        return false;
    }
    memset(instance_section->parsed.instance_section->instances, 0,
           sizeof(WASMComponentInst));
    instance_section->parsed.instance_section->instances[0]
        .instance_expression_tag = WASM_COMP_INSTANCE_EXPRESSION_WITH_ARGS;
    instance_section->parsed.instance_section->instances[0].expression.with_args.idx =
        1;
    instance_section->parsed.instance_section->instances[0]
        .expression.with_args.arg_len = 1;
    instance_section->parsed.instance_section->instances[0]
        .expression.with_args.args =
        (WASMComponentInstArg *)wasm_runtime_malloc(sizeof(WASMComponentInstArg));
    if (!instance_section->parsed.instance_section->instances[0]
             .expression.with_args.args) {
        return false;
    }
    memset(instance_section->parsed.instance_section->instances[0]
               .expression.with_args.args,
           0, sizeof(WASMComponentInstArg));
    instance_section->parsed.instance_section->instances[0]
        .expression.with_args.args[0]
        .name = clone_core_name("source");
    instance_section->parsed.instance_section->instances[0]
        .expression.with_args.args[0]
        .idx.sort_idx = create_sort_idx(WASM_COMP_SORT_FUNC, 0);
    if (!instance_section->parsed.instance_section->instances[0]
             .expression.with_args.args[0]
             .name
        || !instance_section->parsed.instance_section->instances[0]
                 .expression.with_args.args[0]
                 .idx.sort_idx) {
        return false;
    }

    export_section->id = WASM_COMP_SECTION_EXPORTS;
    export_section->parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!export_section->parsed.export_section) {
        return false;
    }
    memset(export_section->parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    export_section->parsed.export_section->count = 1;
    export_section->parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!export_section->parsed.export_section->exports) {
        return false;
    }
    memset(export_section->parsed.export_section->exports, 0,
           sizeof(WASMComponentExport));
    export_section->parsed.export_section->exports[0].export_name =
        create_export_name("nested-inline-instance");
    export_section->parsed.export_section->exports[0].sort_idx =
        create_sort_idx(WASM_COMP_SORT_INSTANCE, 1);
    return export_section->parsed.export_section->exports[0].export_name
           && export_section->parsed.export_section->exports[0].sort_idx;
}

static bool
append_top_level_inline_component_instance_sections(
    WASMComponentModule *component_module)
{
    WASMComponent *component = &component_module->component;
    const uint32_t old_count = component->section_count;
    const uint32_t new_count = old_count + 2;
    auto *new_sections = (WASMComponentSection *)wasm_runtime_malloc(
        sizeof(WASMComponentSection) * new_count);
    if (!new_sections) {
        return false;
    }

    memset(new_sections, 0, sizeof(WASMComponentSection) * new_count);
    memcpy(new_sections, component->sections,
           sizeof(WASMComponentSection) * old_count);
    wasm_runtime_free(component->sections);
    component->sections = new_sections;
    component->section_count = new_count;

    WASMComponentSection *instance_section = &component->sections[old_count];
    WASMComponentSection *export_section = &component->sections[old_count + 1];

    instance_section->id = WASM_COMP_SECTION_INSTANCES;
    instance_section->parsed.instance_section =
        (WASMComponentInstSection *)wasm_runtime_malloc(
            sizeof(WASMComponentInstSection));
    if (!instance_section->parsed.instance_section) {
        return false;
    }
    memset(instance_section->parsed.instance_section, 0,
           sizeof(WASMComponentInstSection));
    instance_section->parsed.instance_section->count = 1;
    instance_section->parsed.instance_section->instances =
        (WASMComponentInst *)wasm_runtime_malloc(sizeof(WASMComponentInst));
    if (!instance_section->parsed.instance_section->instances) {
        return false;
    }
    memset(instance_section->parsed.instance_section->instances, 0,
           sizeof(WASMComponentInst));
    instance_section->parsed.instance_section->instances[0]
        .instance_expression_tag = WASM_COMP_INSTANCE_EXPRESSION_WITHOUT_ARGS;
    instance_section->parsed.instance_section->instances[0]
        .expression.without_args.inline_expr_len = 1;
    instance_section->parsed.instance_section->instances[0]
        .expression.without_args.inline_expr =
        (WASMComponentInlineExport *)wasm_runtime_malloc(
            sizeof(WASMComponentInlineExport));
    if (!instance_section->parsed.instance_section->instances[0]
             .expression.without_args.inline_expr) {
        return false;
    }
    memset(instance_section->parsed.instance_section->instances[0]
               .expression.without_args.inline_expr,
           0, sizeof(WASMComponentInlineExport));
    instance_section->parsed.instance_section->instances[0]
        .expression.without_args.inline_expr[0]
        .name = clone_core_name("wrapped-component");
    instance_section->parsed.instance_section->instances[0]
        .expression.without_args.inline_expr[0]
        .sort_idx = create_sort_idx(WASM_COMP_SORT_COMPONENT, 0);
    if (!instance_section->parsed.instance_section->instances[0]
             .expression.without_args.inline_expr[0]
             .name
        || !instance_section->parsed.instance_section->instances[0]
                 .expression.without_args.inline_expr[0]
                 .sort_idx) {
        return false;
    }

    export_section->id = WASM_COMP_SECTION_EXPORTS;
    export_section->parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!export_section->parsed.export_section) {
        return false;
    }
    memset(export_section->parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    export_section->parsed.export_section->count = 1;
    export_section->parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!export_section->parsed.export_section->exports) {
        return false;
    }
    memset(export_section->parsed.export_section->exports, 0,
           sizeof(WASMComponentExport));
    export_section->parsed.export_section->exports[0].export_name =
        create_export_name("top-level-inline-component-instance");
    export_section->parsed.export_section->exports[0].sort_idx =
        create_sort_idx(WASM_COMP_SORT_INSTANCE, 1);
    return export_section->parsed.export_section->exports[0].export_name
           && export_section->parsed.export_section->exports[0].sort_idx;
}

static bool
append_top_level_core_module_export_sections(WASMComponentModule *component_module)
{
    WASMComponent *component = &component_module->component;
    const uint32_t old_count = component->section_count;
    const uint32_t new_count = old_count + 1;
    auto *new_sections = (WASMComponentSection *)wasm_runtime_malloc(
        sizeof(WASMComponentSection) * new_count);
    if (!new_sections) {
        return false;
    }

    memset(new_sections, 0, sizeof(WASMComponentSection) * new_count);
    memcpy(new_sections, component->sections,
           sizeof(WASMComponentSection) * old_count);
    wasm_runtime_free(component->sections);
    component->sections = new_sections;
    component->section_count = new_count;

    WASMComponentSection *export_section = &component->sections[old_count];
    export_section->id = WASM_COMP_SECTION_EXPORTS;
    export_section->parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!export_section->parsed.export_section) {
        return false;
    }
    memset(export_section->parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    export_section->parsed.export_section->count = 1;
    export_section->parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!export_section->parsed.export_section->exports) {
        return false;
    }
    memset(export_section->parsed.export_section->exports, 0,
           sizeof(WASMComponentExport));
    export_section->parsed.export_section->exports[0].export_name =
        create_export_name("top-level-core-module");
    export_section->parsed.export_section->exports[0].sort_idx =
        create_core_sort_idx(WASM_COMP_CORE_SORT_MODULE, 0);
    return export_section->parsed.export_section->exports[0].export_name
           && export_section->parsed.export_section->exports[0].sort_idx;
}

static bool
append_top_level_inline_core_module_instance_sections(
    WASMComponentModule *component_module)
{
    WASMComponent *component = &component_module->component;
    const uint32_t old_count = component->section_count;
    const uint32_t new_count = old_count + 2;
    auto *new_sections = (WASMComponentSection *)wasm_runtime_malloc(
        sizeof(WASMComponentSection) * new_count);
    if (!new_sections) {
        return false;
    }

    memset(new_sections, 0, sizeof(WASMComponentSection) * new_count);
    memcpy(new_sections, component->sections,
           sizeof(WASMComponentSection) * old_count);
    wasm_runtime_free(component->sections);
    component->sections = new_sections;
    component->section_count = new_count;

    WASMComponentSection *instance_section = &component->sections[old_count];
    WASMComponentSection *export_section = &component->sections[old_count + 1];

    instance_section->id = WASM_COMP_SECTION_INSTANCES;
    instance_section->parsed.instance_section =
        (WASMComponentInstSection *)wasm_runtime_malloc(
            sizeof(WASMComponentInstSection));
    if (!instance_section->parsed.instance_section) {
        return false;
    }
    memset(instance_section->parsed.instance_section, 0,
           sizeof(WASMComponentInstSection));
    instance_section->parsed.instance_section->count = 1;
    instance_section->parsed.instance_section->instances =
        (WASMComponentInst *)wasm_runtime_malloc(sizeof(WASMComponentInst));
    if (!instance_section->parsed.instance_section->instances) {
        return false;
    }
    memset(instance_section->parsed.instance_section->instances, 0,
           sizeof(WASMComponentInst));
    instance_section->parsed.instance_section->instances[0]
        .instance_expression_tag = WASM_COMP_INSTANCE_EXPRESSION_WITHOUT_ARGS;
    instance_section->parsed.instance_section->instances[0]
        .expression.without_args.inline_expr_len = 1;
    instance_section->parsed.instance_section->instances[0]
        .expression.without_args.inline_expr =
        (WASMComponentInlineExport *)wasm_runtime_malloc(
            sizeof(WASMComponentInlineExport));
    if (!instance_section->parsed.instance_section->instances[0]
             .expression.without_args.inline_expr) {
        return false;
    }
    memset(instance_section->parsed.instance_section->instances[0]
               .expression.without_args.inline_expr,
           0, sizeof(WASMComponentInlineExport));
    instance_section->parsed.instance_section->instances[0]
        .expression.without_args.inline_expr[0]
        .name = clone_core_name("wrapped-core-module");
    instance_section->parsed.instance_section->instances[0]
        .expression.without_args.inline_expr[0]
        .sort_idx = create_core_sort_idx(WASM_COMP_CORE_SORT_MODULE, 0);
    if (!instance_section->parsed.instance_section->instances[0]
             .expression.without_args.inline_expr[0]
             .name
        || !instance_section->parsed.instance_section->instances[0]
                 .expression.without_args.inline_expr[0]
                 .sort_idx) {
        return false;
    }

    export_section->id = WASM_COMP_SECTION_EXPORTS;
    export_section->parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!export_section->parsed.export_section) {
        return false;
    }
    memset(export_section->parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    export_section->parsed.export_section->count = 1;
    export_section->parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!export_section->parsed.export_section->exports) {
        return false;
    }
    memset(export_section->parsed.export_section->exports, 0,
           sizeof(WASMComponentExport));
    export_section->parsed.export_section->exports[0].export_name =
        create_export_name("top-level-inline-core-module-instance");
    export_section->parsed.export_section->exports[0].sort_idx =
        create_sort_idx(WASM_COMP_SORT_INSTANCE, 1);
    return export_section->parsed.export_section->exports[0].export_name
           && export_section->parsed.export_section->exports[0].sort_idx;
}

static bool
append_nested_component_inline_component_instance_sections(
    WASMComponentModule *component_module)
{
    WASMComponent *component = &component_module->component;
    WASMComponentSection *component_section;
    WASMComponent *nested_component;
    WASMComponentImport *component_import;
    WASMComponentInlineExport *inline_export;
    WASMComponentInstArg *inst_arg;

    if (!append_nested_component_inline_instance_sections(component_module))
        return false;

    component_section = &component->sections[component->section_count - 3];
    nested_component = component_section->parsed.component;
    component_import =
        &nested_component->sections[0].parsed.import_section->imports[0];
    inline_export = &nested_component->sections[1]
                         .parsed.instance_section->instances[0]
                         .expression.without_args.inline_expr[0];
    inst_arg = &component->sections[component->section_count - 2]
                    .parsed.instance_section->instances[0]
                    .expression.with_args.args[0];

    component_import->extern_desc->type = WASM_COMP_EXTERN_COMPONENT;
    inline_export->sort_idx->sort->sort = WASM_COMP_SORT_COMPONENT;
    inst_arg->idx.sort_idx->sort->sort = WASM_COMP_SORT_COMPONENT;
    return true;
}

static bool
append_nested_subcomponent_instance_sections(WASMComponentModule *component_module)
{
    WASMComponent *component = &component_module->component;
    const uint32_t old_count = component->section_count;
    const uint32_t new_count = old_count + 3;
    auto *new_sections = (WASMComponentSection *)wasm_runtime_malloc(
        sizeof(WASMComponentSection) * new_count);
    if (!new_sections) {
        return false;
    }

    memset(new_sections, 0, sizeof(WASMComponentSection) * new_count);
    memcpy(new_sections, component->sections,
           sizeof(WASMComponentSection) * old_count);
    wasm_runtime_free(component->sections);
    component->sections = new_sections;
    component->section_count = new_count;

    WASMComponentSection *component_section = &component->sections[old_count];
    WASMComponentSection *instance_section = &component->sections[old_count + 1];
    WASMComponentSection *export_section = &component->sections[old_count + 2];

    component_section->id = WASM_COMP_SECTION_COMPONENT;
    component_section->parsed.component =
        (WASMComponent *)wasm_runtime_malloc(sizeof(WASMComponent));
    if (!component_section->parsed.component) {
        return false;
    }
    memset(component_section->parsed.component, 0, sizeof(WASMComponent));
    component_section->parsed.component->section_count = 4;
    component_section->parsed.component->sections =
        (WASMComponentSection *)wasm_runtime_malloc(sizeof(WASMComponentSection) * 4);
    if (!component_section->parsed.component->sections) {
        return false;
    }
    memset(component_section->parsed.component->sections, 0,
           sizeof(WASMComponentSection) * 4);

    component_section->parsed.component->sections[0].id = WASM_COMP_SECTION_IMPORTS;
    component_section->parsed.component->sections[0].parsed.import_section =
        (WASMComponentImportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentImportSection));
    if (!component_section->parsed.component->sections[0].parsed.import_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[0].parsed.import_section, 0,
           sizeof(WASMComponentImportSection));
    component_section->parsed.component->sections[0].parsed.import_section->count =
        1;
    component_section->parsed.component->sections[0].parsed.import_section->imports =
        (WASMComponentImport *)wasm_runtime_malloc(sizeof(WASMComponentImport));
    if (!component_section->parsed.component->sections[0]
             .parsed.import_section->imports) {
        return false;
    }
    memset(component_section->parsed.component->sections[0]
               .parsed.import_section->imports,
           0, sizeof(WASMComponentImport));
    component_section->parsed.component->sections[0]
        .parsed.import_section->imports[0]
        .import_name = create_import_name("source");
    component_section->parsed.component->sections[0]
        .parsed.import_section->imports[0]
        .extern_desc = create_extern_desc(WASM_COMP_EXTERN_FUNC);
    if (!component_section->parsed.component->sections[0]
             .parsed.import_section->imports[0]
             .import_name
        || !component_section->parsed.component->sections[0]
                 .parsed.import_section->imports[0]
                 .extern_desc) {
        return false;
    }

    component_section->parsed.component->sections[1].id = WASM_COMP_SECTION_COMPONENT;
    component_section->parsed.component->sections[1].parsed.component =
        (WASMComponent *)wasm_runtime_malloc(sizeof(WASMComponent));
    if (!component_section->parsed.component->sections[1].parsed.component) {
        return false;
    }
    memset(component_section->parsed.component->sections[1].parsed.component, 0,
           sizeof(WASMComponent));
    component_section->parsed.component->sections[1].parsed.component->section_count =
        2;
    component_section->parsed.component->sections[1].parsed.component->sections =
        (WASMComponentSection *)wasm_runtime_malloc(sizeof(WASMComponentSection) * 2);
    if (!component_section->parsed.component->sections[1]
             .parsed.component->sections) {
        return false;
    }
    memset(component_section->parsed.component->sections[1]
               .parsed.component->sections,
           0, sizeof(WASMComponentSection) * 2);

    component_section->parsed.component->sections[1]
        .parsed.component->sections[0]
        .id = WASM_COMP_SECTION_IMPORTS;
    component_section->parsed.component->sections[1]
        .parsed.component->sections[0]
        .parsed.import_section = (WASMComponentImportSection *)wasm_runtime_malloc(
        sizeof(WASMComponentImportSection));
    if (!component_section->parsed.component->sections[1]
             .parsed.component->sections[0]
             .parsed.import_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[1]
               .parsed.component->sections[0]
               .parsed.import_section,
           0, sizeof(WASMComponentImportSection));
    component_section->parsed.component->sections[1]
        .parsed.component->sections[0]
        .parsed.import_section->count = 1;
    component_section->parsed.component->sections[1]
        .parsed.component->sections[0]
        .parsed.import_section->imports =
        (WASMComponentImport *)wasm_runtime_malloc(sizeof(WASMComponentImport));
    if (!component_section->parsed.component->sections[1]
             .parsed.component->sections[0]
             .parsed.import_section->imports) {
        return false;
    }
    memset(component_section->parsed.component->sections[1]
               .parsed.component->sections[0]
               .parsed.import_section->imports,
           0, sizeof(WASMComponentImport));
    component_section->parsed.component->sections[1]
        .parsed.component->sections[0]
        .parsed.import_section->imports[0]
        .import_name = create_import_name("source");
    component_section->parsed.component->sections[1]
        .parsed.component->sections[0]
        .parsed.import_section->imports[0]
        .extern_desc = create_extern_desc(WASM_COMP_EXTERN_FUNC);
    if (!component_section->parsed.component->sections[1]
             .parsed.component->sections[0]
             .parsed.import_section->imports[0]
             .import_name
        || !component_section->parsed.component->sections[1]
                 .parsed.component->sections[0]
                 .parsed.import_section->imports[0]
                 .extern_desc) {
        return false;
    }

    component_section->parsed.component->sections[1]
        .parsed.component->sections[1]
        .id = WASM_COMP_SECTION_EXPORTS;
    component_section->parsed.component->sections[1]
        .parsed.component->sections[1]
        .parsed.export_section = (WASMComponentExportSection *)wasm_runtime_malloc(
        sizeof(WASMComponentExportSection));
    if (!component_section->parsed.component->sections[1]
             .parsed.component->sections[1]
             .parsed.export_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[1]
               .parsed.component->sections[1]
               .parsed.export_section,
           0, sizeof(WASMComponentExportSection));
    component_section->parsed.component->sections[1]
        .parsed.component->sections[1]
        .parsed.export_section->count = 1;
    component_section->parsed.component->sections[1]
        .parsed.component->sections[1]
        .parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!component_section->parsed.component->sections[1]
             .parsed.component->sections[1]
             .parsed.export_section->exports) {
        return false;
    }
    memset(component_section->parsed.component->sections[1]
               .parsed.component->sections[1]
               .parsed.export_section->exports,
           0, sizeof(WASMComponentExport));
    component_section->parsed.component->sections[1]
        .parsed.component->sections[1]
        .parsed.export_section->exports[0]
        .export_name = create_export_name("wrapped");
    component_section->parsed.component->sections[1]
        .parsed.component->sections[1]
        .parsed.export_section->exports[0]
        .sort_idx = create_sort_idx(WASM_COMP_SORT_FUNC, 0);
    if (!component_section->parsed.component->sections[1]
             .parsed.component->sections[1]
             .parsed.export_section->exports[0]
             .export_name
        || !component_section->parsed.component->sections[1]
                 .parsed.component->sections[1]
                 .parsed.export_section->exports[0]
                 .sort_idx) {
        return false;
    }

    component_section->parsed.component->sections[2].id = WASM_COMP_SECTION_INSTANCES;
    component_section->parsed.component->sections[2].parsed.instance_section =
        (WASMComponentInstSection *)wasm_runtime_malloc(
            sizeof(WASMComponentInstSection));
    if (!component_section->parsed.component->sections[2].parsed.instance_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[2].parsed.instance_section, 0,
           sizeof(WASMComponentInstSection));
    component_section->parsed.component->sections[2].parsed.instance_section->count =
        1;
    component_section->parsed.component->sections[2].parsed.instance_section->instances =
        (WASMComponentInst *)wasm_runtime_malloc(sizeof(WASMComponentInst));
    if (!component_section->parsed.component->sections[2]
             .parsed.instance_section->instances) {
        return false;
    }
    memset(component_section->parsed.component->sections[2]
               .parsed.instance_section->instances,
           0, sizeof(WASMComponentInst));
    component_section->parsed.component->sections[2]
        .parsed.instance_section->instances[0]
        .instance_expression_tag = WASM_COMP_INSTANCE_EXPRESSION_WITH_ARGS;
    component_section->parsed.component->sections[2]
        .parsed.instance_section->instances[0]
        .expression.with_args.idx = 0;
    component_section->parsed.component->sections[2]
        .parsed.instance_section->instances[0]
        .expression.with_args.arg_len = 1;
    component_section->parsed.component->sections[2]
        .parsed.instance_section->instances[0]
        .expression.with_args.args =
        (WASMComponentInstArg *)wasm_runtime_malloc(sizeof(WASMComponentInstArg));
    if (!component_section->parsed.component->sections[2]
             .parsed.instance_section->instances[0]
             .expression.with_args.args) {
        return false;
    }
    memset(component_section->parsed.component->sections[2]
               .parsed.instance_section->instances[0]
               .expression.with_args.args,
           0, sizeof(WASMComponentInstArg));
    component_section->parsed.component->sections[2]
        .parsed.instance_section->instances[0]
        .expression.with_args.args[0]
        .name = clone_core_name("source");
    component_section->parsed.component->sections[2]
        .parsed.instance_section->instances[0]
        .expression.with_args.args[0]
        .idx.sort_idx = create_sort_idx(WASM_COMP_SORT_FUNC, 0);
    if (!component_section->parsed.component->sections[2]
             .parsed.instance_section->instances[0]
             .expression.with_args.args[0]
             .name
        || !component_section->parsed.component->sections[2]
                 .parsed.instance_section->instances[0]
                 .expression.with_args.args[0]
                 .idx.sort_idx) {
        return false;
    }

    component_section->parsed.component->sections[3].id = WASM_COMP_SECTION_EXPORTS;
    component_section->parsed.component->sections[3].parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!component_section->parsed.component->sections[3].parsed.export_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[3].parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    component_section->parsed.component->sections[3].parsed.export_section->count =
        1;
    component_section->parsed.component->sections[3].parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!component_section->parsed.component->sections[3]
             .parsed.export_section->exports) {
        return false;
    }
    memset(component_section->parsed.component->sections[3]
               .parsed.export_section->exports,
           0, sizeof(WASMComponentExport));
    component_section->parsed.component->sections[3]
        .parsed.export_section->exports[0]
        .export_name = create_export_name("wrapped-instance");
    component_section->parsed.component->sections[3]
        .parsed.export_section->exports[0]
        .sort_idx = create_sort_idx(WASM_COMP_SORT_INSTANCE, 0);
    if (!component_section->parsed.component->sections[3]
             .parsed.export_section->exports[0]
             .export_name
        || !component_section->parsed.component->sections[3]
                 .parsed.export_section->exports[0]
                 .sort_idx) {
        return false;
    }

    instance_section->id = WASM_COMP_SECTION_INSTANCES;
    instance_section->parsed.instance_section =
        (WASMComponentInstSection *)wasm_runtime_malloc(
            sizeof(WASMComponentInstSection));
    if (!instance_section->parsed.instance_section) {
        return false;
    }
    memset(instance_section->parsed.instance_section, 0,
           sizeof(WASMComponentInstSection));
    instance_section->parsed.instance_section->count = 1;
    instance_section->parsed.instance_section->instances =
        (WASMComponentInst *)wasm_runtime_malloc(sizeof(WASMComponentInst));
    if (!instance_section->parsed.instance_section->instances) {
        return false;
    }
    memset(instance_section->parsed.instance_section->instances, 0,
           sizeof(WASMComponentInst));
    instance_section->parsed.instance_section->instances[0]
        .instance_expression_tag = WASM_COMP_INSTANCE_EXPRESSION_WITH_ARGS;
    instance_section->parsed.instance_section->instances[0].expression.with_args.idx =
        1;
    instance_section->parsed.instance_section->instances[0]
        .expression.with_args.arg_len = 1;
    instance_section->parsed.instance_section->instances[0]
        .expression.with_args.args =
        (WASMComponentInstArg *)wasm_runtime_malloc(sizeof(WASMComponentInstArg));
    if (!instance_section->parsed.instance_section->instances[0]
             .expression.with_args.args) {
        return false;
    }
    memset(instance_section->parsed.instance_section->instances[0]
               .expression.with_args.args,
           0, sizeof(WASMComponentInstArg));
    instance_section->parsed.instance_section->instances[0]
        .expression.with_args.args[0]
        .name = clone_core_name("source");
    instance_section->parsed.instance_section->instances[0]
        .expression.with_args.args[0]
        .idx.sort_idx = create_sort_idx(WASM_COMP_SORT_FUNC, 0);
    if (!instance_section->parsed.instance_section->instances[0]
             .expression.with_args.args[0]
             .name
        || !instance_section->parsed.instance_section->instances[0]
                 .expression.with_args.args[0]
                 .idx.sort_idx) {
        return false;
    }

    export_section->id = WASM_COMP_SECTION_EXPORTS;
    export_section->parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!export_section->parsed.export_section) {
        return false;
    }
    memset(export_section->parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    export_section->parsed.export_section->count = 1;
    export_section->parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!export_section->parsed.export_section->exports) {
        return false;
    }
    memset(export_section->parsed.export_section->exports, 0,
           sizeof(WASMComponentExport));
    export_section->parsed.export_section->exports[0].export_name =
        create_export_name("nested-subcomponent-instance");
    export_section->parsed.export_section->exports[0].sort_idx =
        create_sort_idx(WASM_COMP_SORT_INSTANCE, 1);
    return export_section->parsed.export_section->exports[0].export_name
           && export_section->parsed.export_section->exports[0].sort_idx;
}

static bool
append_top_level_component_export_alias_sections(
    WASMComponentModule *component_module)
{
    WASMComponent *component = &component_module->component;
    const uint32_t old_count = component->section_count;
    const uint32_t new_count = old_count + 4;
    auto *new_sections = (WASMComponentSection *)wasm_runtime_malloc(
        sizeof(WASMComponentSection) * new_count);
    if (!new_sections) {
        return false;
    }

    memset(new_sections, 0, sizeof(WASMComponentSection) * new_count);
    memcpy(new_sections, component->sections,
           sizeof(WASMComponentSection) * old_count);
    wasm_runtime_free(component->sections);
    component->sections = new_sections;
    component->section_count = new_count;

    WASMComponentSection *component_section = &component->sections[old_count];
    WASMComponentSection *instance_section = &component->sections[old_count + 1];
    WASMComponentSection *alias_section = &component->sections[old_count + 2];
    WASMComponentSection *export_section = &component->sections[old_count + 3];

    component_section->id = WASM_COMP_SECTION_COMPONENT;
    component_section->parsed.component = create_empty_component();
    if (!component_section->parsed.component) {
        return false;
    }

    instance_section->id = WASM_COMP_SECTION_INSTANCES;
    instance_section->parsed.instance_section =
        (WASMComponentInstSection *)wasm_runtime_malloc(
            sizeof(WASMComponentInstSection));
    if (!instance_section->parsed.instance_section) {
        return false;
    }
    memset(instance_section->parsed.instance_section, 0,
           sizeof(WASMComponentInstSection));
    instance_section->parsed.instance_section->count = 1;
    instance_section->parsed.instance_section->instances =
        (WASMComponentInst *)wasm_runtime_malloc(sizeof(WASMComponentInst));
    if (!instance_section->parsed.instance_section->instances) {
        return false;
    }
    memset(instance_section->parsed.instance_section->instances, 0,
           sizeof(WASMComponentInst));
    instance_section->parsed.instance_section->instances[0]
        .instance_expression_tag = WASM_COMP_INSTANCE_EXPRESSION_WITHOUT_ARGS;
    instance_section->parsed.instance_section->instances[0]
        .expression.without_args.inline_expr_len = 1;
    instance_section->parsed.instance_section->instances[0]
        .expression.without_args.inline_expr =
        (WASMComponentInlineExport *)wasm_runtime_malloc(
            sizeof(WASMComponentInlineExport));
    if (!instance_section->parsed.instance_section->instances[0]
             .expression.without_args.inline_expr) {
        return false;
    }
    memset(instance_section->parsed.instance_section->instances[0]
               .expression.without_args.inline_expr,
           0, sizeof(WASMComponentInlineExport));
    instance_section->parsed.instance_section->instances[0]
        .expression.without_args.inline_expr[0]
        .name = clone_core_name("wrapped-component");
    instance_section->parsed.instance_section->instances[0]
        .expression.without_args.inline_expr[0]
        .sort_idx = create_sort_idx(WASM_COMP_SORT_COMPONENT, 1);
    if (!instance_section->parsed.instance_section->instances[0]
             .expression.without_args.inline_expr[0]
             .name
        || !instance_section->parsed.instance_section->instances[0]
                 .expression.without_args.inline_expr[0]
                 .sort_idx) {
        return false;
    }

    alias_section->id = WASM_COMP_SECTION_ALIASES;
    alias_section->parsed.alias_section =
        (WASMComponentAliasSection *)wasm_runtime_malloc(
            sizeof(WASMComponentAliasSection));
    if (!alias_section->parsed.alias_section) {
        return false;
    }
    memset(alias_section->parsed.alias_section, 0,
           sizeof(WASMComponentAliasSection));
    alias_section->parsed.alias_section->count = 1;
    alias_section->parsed.alias_section->aliases =
        (WASMComponentAliasDefinition *)wasm_runtime_malloc(
            sizeof(WASMComponentAliasDefinition));
    if (!alias_section->parsed.alias_section->aliases) {
        return false;
    }
    memset(alias_section->parsed.alias_section->aliases, 0,
           sizeof(WASMComponentAliasDefinition));
    alias_section->parsed.alias_section->aliases[0].sort =
        create_sort(WASM_COMP_SORT_COMPONENT);
    alias_section->parsed.alias_section->aliases[0].alias_target_type =
        WASM_COMP_ALIAS_TARGET_EXPORT;
    alias_section->parsed.alias_section->aliases[0].target.exported.instance_idx =
        1;
    alias_section->parsed.alias_section->aliases[0].target.exported.name =
        clone_core_name("wrapped-component");
    if (!alias_section->parsed.alias_section->aliases[0].sort
        || !alias_section->parsed.alias_section->aliases[0]
                .target.exported.name) {
        return false;
    }

    export_section->id = WASM_COMP_SECTION_EXPORTS;
    export_section->parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!export_section->parsed.export_section) {
        return false;
    }
    memset(export_section->parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    export_section->parsed.export_section->count = 1;
    export_section->parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!export_section->parsed.export_section->exports) {
        return false;
    }
    memset(export_section->parsed.export_section->exports, 0,
           sizeof(WASMComponentExport));
    export_section->parsed.export_section->exports[0].export_name =
        create_export_name("aliased-component");
    export_section->parsed.export_section->exports[0].sort_idx =
        create_sort_idx(WASM_COMP_SORT_COMPONENT, 2);
    return export_section->parsed.export_section->exports[0].export_name
           && export_section->parsed.export_section->exports[0].sort_idx;
}

static bool
append_nested_component_component_alias_sections(
    WASMComponentModule *component_module)
{
    WASMComponent *component = &component_module->component;
    const uint32_t old_count = component->section_count;
    const uint32_t new_count = old_count + 3;
    auto *new_sections = (WASMComponentSection *)wasm_runtime_malloc(
        sizeof(WASMComponentSection) * new_count);
    if (!new_sections) {
        return false;
    }

    memset(new_sections, 0, sizeof(WASMComponentSection) * new_count);
    memcpy(new_sections, component->sections,
           sizeof(WASMComponentSection) * old_count);
    wasm_runtime_free(component->sections);
    component->sections = new_sections;
    component->section_count = new_count;

    WASMComponentSection *component_section = &component->sections[old_count];
    WASMComponentSection *instance_section = &component->sections[old_count + 1];
    WASMComponentSection *export_section = &component->sections[old_count + 2];

    component_section->id = WASM_COMP_SECTION_COMPONENT;
    component_section->parsed.component =
        (WASMComponent *)wasm_runtime_malloc(sizeof(WASMComponent));
    if (!component_section->parsed.component) {
        return false;
    }
    memset(component_section->parsed.component, 0, sizeof(WASMComponent));
    component_section->parsed.component->section_count = 4;
    component_section->parsed.component->sections =
        (WASMComponentSection *)wasm_runtime_malloc(sizeof(WASMComponentSection) * 4);
    if (!component_section->parsed.component->sections) {
        return false;
    }
    memset(component_section->parsed.component->sections, 0,
           sizeof(WASMComponentSection) * 4);

    component_section->parsed.component->sections[0].id = WASM_COMP_SECTION_COMPONENT;
    component_section->parsed.component->sections[0].parsed.component =
        create_empty_component();
    if (!component_section->parsed.component->sections[0].parsed.component) {
        return false;
    }

    component_section->parsed.component->sections[1].id = WASM_COMP_SECTION_INSTANCES;
    component_section->parsed.component->sections[1].parsed.instance_section =
        (WASMComponentInstSection *)wasm_runtime_malloc(
            sizeof(WASMComponentInstSection));
    if (!component_section->parsed.component->sections[1].parsed.instance_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[1].parsed.instance_section, 0,
           sizeof(WASMComponentInstSection));
    component_section->parsed.component->sections[1].parsed.instance_section->count =
        1;
    component_section->parsed.component->sections[1].parsed.instance_section->instances =
        (WASMComponentInst *)wasm_runtime_malloc(sizeof(WASMComponentInst));
    if (!component_section->parsed.component->sections[1]
             .parsed.instance_section->instances) {
        return false;
    }
    memset(component_section->parsed.component->sections[1]
               .parsed.instance_section->instances,
           0, sizeof(WASMComponentInst));
    component_section->parsed.component->sections[1]
        .parsed.instance_section->instances[0]
        .instance_expression_tag = WASM_COMP_INSTANCE_EXPRESSION_WITHOUT_ARGS;
    component_section->parsed.component->sections[1]
        .parsed.instance_section->instances[0]
        .expression.without_args.inline_expr_len = 1;
    component_section->parsed.component->sections[1]
        .parsed.instance_section->instances[0]
        .expression.without_args.inline_expr =
        (WASMComponentInlineExport *)wasm_runtime_malloc(
            sizeof(WASMComponentInlineExport));
    if (!component_section->parsed.component->sections[1]
             .parsed.instance_section->instances[0]
             .expression.without_args.inline_expr) {
        return false;
    }
    memset(component_section->parsed.component->sections[1]
               .parsed.instance_section->instances[0]
               .expression.without_args.inline_expr,
           0, sizeof(WASMComponentInlineExport));
    component_section->parsed.component->sections[1]
        .parsed.instance_section->instances[0]
        .expression.without_args.inline_expr[0]
        .name = clone_core_name("wrapped-component");
    component_section->parsed.component->sections[1]
        .parsed.instance_section->instances[0]
        .expression.without_args.inline_expr[0]
        .sort_idx = create_sort_idx(WASM_COMP_SORT_COMPONENT, 0);
    if (!component_section->parsed.component->sections[1]
             .parsed.instance_section->instances[0]
             .expression.without_args.inline_expr[0]
             .name
        || !component_section->parsed.component->sections[1]
                 .parsed.instance_section->instances[0]
                 .expression.without_args.inline_expr[0]
                 .sort_idx) {
        return false;
    }

    component_section->parsed.component->sections[2].id = WASM_COMP_SECTION_ALIASES;
    component_section->parsed.component->sections[2].parsed.alias_section =
        (WASMComponentAliasSection *)wasm_runtime_malloc(
            sizeof(WASMComponentAliasSection));
    if (!component_section->parsed.component->sections[2].parsed.alias_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[2].parsed.alias_section, 0,
           sizeof(WASMComponentAliasSection));
    component_section->parsed.component->sections[2].parsed.alias_section->count =
        1;
    component_section->parsed.component->sections[2].parsed.alias_section->aliases =
        (WASMComponentAliasDefinition *)wasm_runtime_malloc(
            sizeof(WASMComponentAliasDefinition));
    if (!component_section->parsed.component->sections[2]
             .parsed.alias_section->aliases) {
        return false;
    }
    memset(component_section->parsed.component->sections[2]
               .parsed.alias_section->aliases,
           0, sizeof(WASMComponentAliasDefinition));
    component_section->parsed.component->sections[2]
        .parsed.alias_section->aliases[0]
        .sort = create_sort(WASM_COMP_SORT_COMPONENT);
    component_section->parsed.component->sections[2]
        .parsed.alias_section->aliases[0]
        .alias_target_type = WASM_COMP_ALIAS_TARGET_EXPORT;
    component_section->parsed.component->sections[2]
        .parsed.alias_section->aliases[0]
        .target.exported.instance_idx = 0;
    component_section->parsed.component->sections[2]
        .parsed.alias_section->aliases[0]
        .target.exported.name = clone_core_name("wrapped-component");
    if (!component_section->parsed.component->sections[2]
             .parsed.alias_section->aliases[0]
             .sort
        || !component_section->parsed.component->sections[2]
                 .parsed.alias_section->aliases[0]
                 .target.exported.name) {
        return false;
    }

    component_section->parsed.component->sections[3].id = WASM_COMP_SECTION_EXPORTS;
    component_section->parsed.component->sections[3].parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!component_section->parsed.component->sections[3].parsed.export_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[3].parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    component_section->parsed.component->sections[3].parsed.export_section->count =
        1;
    component_section->parsed.component->sections[3].parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!component_section->parsed.component->sections[3]
             .parsed.export_section->exports) {
        return false;
    }
    memset(component_section->parsed.component->sections[3]
               .parsed.export_section->exports,
           0, sizeof(WASMComponentExport));
    component_section->parsed.component->sections[3]
        .parsed.export_section->exports[0]
        .export_name = create_export_name("aliased-component");
    component_section->parsed.component->sections[3]
        .parsed.export_section->exports[0]
        .sort_idx = create_sort_idx(WASM_COMP_SORT_COMPONENT, 1);
    if (!component_section->parsed.component->sections[3]
             .parsed.export_section->exports[0]
             .export_name
        || !component_section->parsed.component->sections[3]
                 .parsed.export_section->exports[0]
                 .sort_idx) {
        return false;
    }

    instance_section->id = WASM_COMP_SECTION_INSTANCES;
    instance_section->parsed.instance_section =
        (WASMComponentInstSection *)wasm_runtime_malloc(
            sizeof(WASMComponentInstSection));
    if (!instance_section->parsed.instance_section) {
        return false;
    }
    memset(instance_section->parsed.instance_section, 0,
           sizeof(WASMComponentInstSection));
    instance_section->parsed.instance_section->count = 1;
    instance_section->parsed.instance_section->instances =
        (WASMComponentInst *)wasm_runtime_malloc(sizeof(WASMComponentInst));
    if (!instance_section->parsed.instance_section->instances) {
        return false;
    }
    memset(instance_section->parsed.instance_section->instances, 0,
           sizeof(WASMComponentInst));
    instance_section->parsed.instance_section->instances[0]
        .instance_expression_tag = WASM_COMP_INSTANCE_EXPRESSION_WITH_ARGS;
    instance_section->parsed.instance_section->instances[0].expression.with_args.idx =
        1;
    instance_section->parsed.instance_section->instances[0]
        .expression.with_args.arg_len = 0;
    instance_section->parsed.instance_section->instances[0]
        .expression.with_args.args = nullptr;

    export_section->id = WASM_COMP_SECTION_EXPORTS;
    export_section->parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!export_section->parsed.export_section) {
        return false;
    }
    memset(export_section->parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    export_section->parsed.export_section->count = 1;
    export_section->parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!export_section->parsed.export_section->exports) {
        return false;
    }
    memset(export_section->parsed.export_section->exports, 0,
           sizeof(WASMComponentExport));
    export_section->parsed.export_section->exports[0].export_name =
        create_export_name("nested-component-alias");
    export_section->parsed.export_section->exports[0].sort_idx =
        create_sort_idx(WASM_COMP_SORT_INSTANCE, 1);
    return export_section->parsed.export_section->exports[0].export_name
           && export_section->parsed.export_section->exports[0].sort_idx;
}

static bool
append_nested_component_outer_alias_sections(
    WASMComponentModule *component_module)
{
    WASMComponent *component = &component_module->component;
    const uint32_t old_count = component->section_count;
    const uint32_t new_count = old_count + 3;
    auto *new_sections = (WASMComponentSection *)wasm_runtime_malloc(
        sizeof(WASMComponentSection) * new_count);
    if (!new_sections) {
        return false;
    }

    memset(new_sections, 0, sizeof(WASMComponentSection) * new_count);
    memcpy(new_sections, component->sections,
           sizeof(WASMComponentSection) * old_count);
    wasm_runtime_free(component->sections);
    component->sections = new_sections;
    component->section_count = new_count;

    WASMComponentSection *component_section = &component->sections[old_count];
    WASMComponentSection *instance_section = &component->sections[old_count + 1];
    WASMComponentSection *export_section = &component->sections[old_count + 2];

    component_section->id = WASM_COMP_SECTION_COMPONENT;
    component_section->parsed.component = create_empty_component();
    if (!component_section->parsed.component) {
        return false;
    }
    component_section->parsed.component->section_count = 4;
    component_section->parsed.component->sections =
        (WASMComponentSection *)wasm_runtime_malloc(sizeof(WASMComponentSection) * 4);
    if (!component_section->parsed.component->sections) {
        return false;
    }
    memset(component_section->parsed.component->sections, 0,
           sizeof(WASMComponentSection) * 4);

    component_section->parsed.component->sections[0].id = WASM_COMP_SECTION_COMPONENT;
    component_section->parsed.component->sections[0].parsed.component =
        create_empty_component();
    if (!component_section->parsed.component->sections[0].parsed.component) {
        return false;
    }

    component_section->parsed.component->sections[1].id = WASM_COMP_SECTION_COMPONENT;
    component_section->parsed.component->sections[1].parsed.component =
        create_empty_component();
    if (!component_section->parsed.component->sections[1].parsed.component) {
        return false;
    }
    component_section->parsed.component->sections[1].parsed.component->section_count = 2;
    component_section->parsed.component->sections[1].parsed.component->sections =
        (WASMComponentSection *)wasm_runtime_malloc(sizeof(WASMComponentSection) * 2);
    if (!component_section->parsed.component->sections[1]
             .parsed.component->sections) {
        return false;
    }
    memset(component_section->parsed.component->sections[1].parsed.component->sections,
           0, sizeof(WASMComponentSection) * 2);

    component_section->parsed.component->sections[1]
        .parsed.component->sections[0].id = WASM_COMP_SECTION_ALIASES;
    component_section->parsed.component->sections[1]
        .parsed.component->sections[0].parsed.alias_section =
        (WASMComponentAliasSection *)wasm_runtime_malloc(
            sizeof(WASMComponentAliasSection));
    if (!component_section->parsed.component->sections[1]
             .parsed.component->sections[0].parsed.alias_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[1]
               .parsed.component->sections[0].parsed.alias_section,
           0, sizeof(WASMComponentAliasSection));
    component_section->parsed.component->sections[1]
        .parsed.component->sections[0].parsed.alias_section->count = 1;
    component_section->parsed.component->sections[1]
        .parsed.component->sections[0].parsed.alias_section->aliases =
        (WASMComponentAliasDefinition *)wasm_runtime_malloc(
            sizeof(WASMComponentAliasDefinition));
    if (!component_section->parsed.component->sections[1]
             .parsed.component->sections[0].parsed.alias_section->aliases) {
        return false;
    }
    memset(component_section->parsed.component->sections[1]
               .parsed.component->sections[0].parsed.alias_section->aliases,
           0, sizeof(WASMComponentAliasDefinition));
    component_section->parsed.component->sections[1]
        .parsed.component->sections[0].parsed.alias_section->aliases[0]
        .sort = create_sort(WASM_COMP_SORT_COMPONENT);
    component_section->parsed.component->sections[1]
        .parsed.component->sections[0].parsed.alias_section->aliases[0]
        .alias_target_type = WASM_COMP_ALIAS_TARGET_OUTER;
    component_section->parsed.component->sections[1]
        .parsed.component->sections[0].parsed.alias_section->aliases[0]
        .target.outer.ct = 0;
    component_section->parsed.component->sections[1]
        .parsed.component->sections[0].parsed.alias_section->aliases[0]
        .target.outer.idx = 0;
    if (!component_section->parsed.component->sections[1]
             .parsed.component->sections[0].parsed.alias_section->aliases[0]
             .sort) {
        return false;
    }

    component_section->parsed.component->sections[1]
        .parsed.component->sections[1].id = WASM_COMP_SECTION_EXPORTS;
    component_section->parsed.component->sections[1]
        .parsed.component->sections[1].parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!component_section->parsed.component->sections[1]
             .parsed.component->sections[1].parsed.export_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[1]
               .parsed.component->sections[1].parsed.export_section,
           0, sizeof(WASMComponentExportSection));
    component_section->parsed.component->sections[1]
        .parsed.component->sections[1].parsed.export_section->count = 1;
    component_section->parsed.component->sections[1]
        .parsed.component->sections[1].parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!component_section->parsed.component->sections[1]
             .parsed.component->sections[1].parsed.export_section->exports) {
        return false;
    }
    memset(component_section->parsed.component->sections[1]
               .parsed.component->sections[1].parsed.export_section->exports,
           0, sizeof(WASMComponentExport));
    component_section->parsed.component->sections[1]
        .parsed.component->sections[1].parsed.export_section->exports[0]
        .export_name = create_export_name("outer-component");
    component_section->parsed.component->sections[1]
        .parsed.component->sections[1].parsed.export_section->exports[0]
        .sort_idx = create_sort_idx(WASM_COMP_SORT_COMPONENT, 0);
    if (!component_section->parsed.component->sections[1]
             .parsed.component->sections[1].parsed.export_section->exports[0]
             .export_name
        || !component_section->parsed.component->sections[1]
                 .parsed.component->sections[1].parsed.export_section->exports[0]
                 .sort_idx) {
        return false;
    }

    component_section->parsed.component->sections[2].id = WASM_COMP_SECTION_INSTANCES;
    component_section->parsed.component->sections[2].parsed.instance_section =
        (WASMComponentInstSection *)wasm_runtime_malloc(
            sizeof(WASMComponentInstSection));
    if (!component_section->parsed.component->sections[2].parsed.instance_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[2].parsed.instance_section, 0,
           sizeof(WASMComponentInstSection));
    component_section->parsed.component->sections[2].parsed.instance_section->count = 1;
    component_section->parsed.component->sections[2].parsed.instance_section->instances =
        (WASMComponentInst *)wasm_runtime_malloc(sizeof(WASMComponentInst));
    if (!component_section->parsed.component->sections[2]
             .parsed.instance_section->instances) {
        return false;
    }
    memset(component_section->parsed.component->sections[2]
               .parsed.instance_section->instances,
           0, sizeof(WASMComponentInst));
    component_section->parsed.component->sections[2]
        .parsed.instance_section->instances[0]
        .instance_expression_tag = WASM_COMP_INSTANCE_EXPRESSION_WITH_ARGS;
    component_section->parsed.component->sections[2]
        .parsed.instance_section->instances[0].expression.with_args.idx = 1;

    component_section->parsed.component->sections[3].id = WASM_COMP_SECTION_EXPORTS;
    component_section->parsed.component->sections[3].parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!component_section->parsed.component->sections[3].parsed.export_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[3].parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    component_section->parsed.component->sections[3].parsed.export_section->count = 1;
    component_section->parsed.component->sections[3].parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!component_section->parsed.component->sections[3]
             .parsed.export_section->exports) {
        return false;
    }
    memset(component_section->parsed.component->sections[3]
               .parsed.export_section->exports,
           0, sizeof(WASMComponentExport));
    component_section->parsed.component->sections[3]
        .parsed.export_section->exports[0].export_name =
        create_export_name("outer-alias-instance");
    component_section->parsed.component->sections[3]
        .parsed.export_section->exports[0].sort_idx =
        create_sort_idx(WASM_COMP_SORT_INSTANCE, 0);
    if (!component_section->parsed.component->sections[3]
             .parsed.export_section->exports[0].export_name
        || !component_section->parsed.component->sections[3]
                 .parsed.export_section->exports[0].sort_idx) {
        return false;
    }

    instance_section->id = WASM_COMP_SECTION_INSTANCES;
    instance_section->parsed.instance_section =
        (WASMComponentInstSection *)wasm_runtime_malloc(sizeof(WASMComponentInstSection));
    if (!instance_section->parsed.instance_section) {
        return false;
    }
    memset(instance_section->parsed.instance_section, 0,
           sizeof(WASMComponentInstSection));
    instance_section->parsed.instance_section->count = 1;
    instance_section->parsed.instance_section->instances =
        (WASMComponentInst *)wasm_runtime_malloc(sizeof(WASMComponentInst));
    if (!instance_section->parsed.instance_section->instances) {
        return false;
    }
    memset(instance_section->parsed.instance_section->instances, 0,
           sizeof(WASMComponentInst));
    instance_section->parsed.instance_section->instances[0].instance_expression_tag =
        WASM_COMP_INSTANCE_EXPRESSION_WITH_ARGS;
    instance_section->parsed.instance_section->instances[0].expression.with_args.idx = 1;

    export_section->id = WASM_COMP_SECTION_EXPORTS;
    export_section->parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!export_section->parsed.export_section) {
        return false;
    }
    memset(export_section->parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    export_section->parsed.export_section->count = 1;
    export_section->parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!export_section->parsed.export_section->exports) {
        return false;
    }
    memset(export_section->parsed.export_section->exports, 0,
           sizeof(WASMComponentExport));
    export_section->parsed.export_section->exports[0].export_name =
        create_export_name("ct0-outer-alias");
    export_section->parsed.export_section->exports[0].sort_idx =
        create_sort_idx(WASM_COMP_SORT_INSTANCE, 1);
    return export_section->parsed.export_section->exports[0].export_name
           && export_section->parsed.export_section->exports[0].sort_idx;
}

static bool
append_nested_component_outer_alias_parent_sections(
    WASMComponentModule *component_module)
{
    WASMComponent *component = &component_module->component;
    const uint32_t old_count = component->section_count;
    const uint32_t new_count = old_count + 3;
    auto *new_sections = (WASMComponentSection *)wasm_runtime_malloc(
        sizeof(WASMComponentSection) * new_count);
    if (!new_sections) {
        return false;
    }

    memset(new_sections, 0, sizeof(WASMComponentSection) * new_count);
    memcpy(new_sections, component->sections,
           sizeof(WASMComponentSection) * old_count);
    wasm_runtime_free(component->sections);
    component->sections = new_sections;
    component->section_count = new_count;

    WASMComponentSection *component_section = &component->sections[old_count];
    WASMComponentSection *instance_section = &component->sections[old_count + 1];
    WASMComponentSection *export_section = &component->sections[old_count + 2];

    component_section->id = WASM_COMP_SECTION_COMPONENT;
    component_section->parsed.component = create_empty_component();
    if (!component_section->parsed.component) {
        return false;
    }
    component_section->parsed.component->section_count = 4;
    component_section->parsed.component->sections =
        (WASMComponentSection *)wasm_runtime_malloc(sizeof(WASMComponentSection) * 4);
    if (!component_section->parsed.component->sections) {
        return false;
    }
    memset(component_section->parsed.component->sections, 0,
           sizeof(WASMComponentSection) * 4);

    component_section->parsed.component->sections[0].id = WASM_COMP_SECTION_COMPONENT;
    component_section->parsed.component->sections[0].parsed.component =
        create_empty_component();
    if (!component_section->parsed.component->sections[0].parsed.component) {
        return false;
    }

    component_section->parsed.component->sections[1].id = WASM_COMP_SECTION_COMPONENT;
    component_section->parsed.component->sections[1].parsed.component =
        create_empty_component();
    if (!component_section->parsed.component->sections[1].parsed.component) {
        return false;
    }
    component_section->parsed.component->sections[1].parsed.component->section_count = 3;
    component_section->parsed.component->sections[1].parsed.component->sections =
        (WASMComponentSection *)wasm_runtime_malloc(sizeof(WASMComponentSection) * 3);
    if (!component_section->parsed.component->sections[1]
             .parsed.component->sections) {
        return false;
    }
    memset(component_section->parsed.component->sections[1].parsed.component->sections,
           0, sizeof(WASMComponentSection) * 3);

    component_section->parsed.component->sections[1]
        .parsed.component->sections[0].id = WASM_COMP_SECTION_COMPONENT;
    component_section->parsed.component->sections[1]
        .parsed.component->sections[0].parsed.component = create_empty_component();
    if (!component_section->parsed.component->sections[1]
             .parsed.component->sections[0].parsed.component) {
        return false;
    }
    component_section->parsed.component->sections[1]
        .parsed.component->sections[0].parsed.component->section_count = 2;
    component_section->parsed.component->sections[1]
        .parsed.component->sections[0].parsed.component->sections =
        (WASMComponentSection *)wasm_runtime_malloc(sizeof(WASMComponentSection) * 2);
    if (!component_section->parsed.component->sections[1]
             .parsed.component->sections[0].parsed.component->sections) {
        return false;
    }
    memset(component_section->parsed.component->sections[1]
               .parsed.component->sections[0].parsed.component->sections,
           0, sizeof(WASMComponentSection) * 2);

    component_section->parsed.component->sections[1]
        .parsed.component->sections[0].parsed.component->sections[0].id =
        WASM_COMP_SECTION_ALIASES;
    component_section->parsed.component->sections[1]
        .parsed.component->sections[0].parsed.component->sections[0]
        .parsed.alias_section = (WASMComponentAliasSection *)wasm_runtime_malloc(
        sizeof(WASMComponentAliasSection));
    if (!component_section->parsed.component->sections[1]
             .parsed.component->sections[0].parsed.component->sections[0]
             .parsed.alias_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[1]
               .parsed.component->sections[0].parsed.component->sections[0]
               .parsed.alias_section,
           0, sizeof(WASMComponentAliasSection));
    component_section->parsed.component->sections[1]
        .parsed.component->sections[0].parsed.component->sections[0]
        .parsed.alias_section->count = 1;
    component_section->parsed.component->sections[1]
        .parsed.component->sections[0].parsed.component->sections[0]
        .parsed.alias_section->aliases =
        (WASMComponentAliasDefinition *)wasm_runtime_malloc(
            sizeof(WASMComponentAliasDefinition));
    if (!component_section->parsed.component->sections[1]
             .parsed.component->sections[0].parsed.component->sections[0]
             .parsed.alias_section->aliases) {
        return false;
    }
    memset(component_section->parsed.component->sections[1]
               .parsed.component->sections[0].parsed.component->sections[0]
               .parsed.alias_section->aliases,
           0, sizeof(WASMComponentAliasDefinition));
    component_section->parsed.component->sections[1]
        .parsed.component->sections[0].parsed.component->sections[0]
        .parsed.alias_section->aliases[0].sort =
        create_sort(WASM_COMP_SORT_COMPONENT);
    component_section->parsed.component->sections[1]
        .parsed.component->sections[0].parsed.component->sections[0]
        .parsed.alias_section->aliases[0].alias_target_type =
        WASM_COMP_ALIAS_TARGET_OUTER;
    component_section->parsed.component->sections[1]
        .parsed.component->sections[0].parsed.component->sections[0]
        .parsed.alias_section->aliases[0].target.outer.ct = 1;
    component_section->parsed.component->sections[1]
        .parsed.component->sections[0].parsed.component->sections[0]
        .parsed.alias_section->aliases[0].target.outer.idx = 0;
    if (!component_section->parsed.component->sections[1]
             .parsed.component->sections[0].parsed.component->sections[0]
             .parsed.alias_section->aliases[0].sort) {
        return false;
    }

    component_section->parsed.component->sections[1]
        .parsed.component->sections[0].parsed.component->sections[1].id =
        WASM_COMP_SECTION_EXPORTS;
    component_section->parsed.component->sections[1]
        .parsed.component->sections[0].parsed.component->sections[1]
        .parsed.export_section = (WASMComponentExportSection *)wasm_runtime_malloc(
        sizeof(WASMComponentExportSection));
    if (!component_section->parsed.component->sections[1]
             .parsed.component->sections[0].parsed.component->sections[1]
             .parsed.export_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[1]
               .parsed.component->sections[0].parsed.component->sections[1]
               .parsed.export_section,
           0, sizeof(WASMComponentExportSection));
    component_section->parsed.component->sections[1]
        .parsed.component->sections[0].parsed.component->sections[1]
        .parsed.export_section->count = 1;
    component_section->parsed.component->sections[1]
        .parsed.component->sections[0].parsed.component->sections[1]
        .parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!component_section->parsed.component->sections[1]
             .parsed.component->sections[0].parsed.component->sections[1]
             .parsed.export_section->exports) {
        return false;
    }
    memset(component_section->parsed.component->sections[1]
               .parsed.component->sections[0].parsed.component->sections[1]
               .parsed.export_section->exports,
           0, sizeof(WASMComponentExport));
    component_section->parsed.component->sections[1]
        .parsed.component->sections[0].parsed.component->sections[1]
        .parsed.export_section->exports[0].export_name =
        create_export_name("outer-component");
    component_section->parsed.component->sections[1]
        .parsed.component->sections[0].parsed.component->sections[1]
        .parsed.export_section->exports[0].sort_idx =
        create_sort_idx(WASM_COMP_SORT_COMPONENT, 0);
    if (!component_section->parsed.component->sections[1]
             .parsed.component->sections[0].parsed.component->sections[1]
             .parsed.export_section->exports[0].export_name
        || !component_section->parsed.component->sections[1]
                 .parsed.component->sections[0].parsed.component->sections[1]
                 .parsed.export_section->exports[0].sort_idx) {
        return false;
    }

    component_section->parsed.component->sections[1]
        .parsed.component->sections[1].id = WASM_COMP_SECTION_INSTANCES;
    component_section->parsed.component->sections[1]
        .parsed.component->sections[1].parsed.instance_section =
        (WASMComponentInstSection *)wasm_runtime_malloc(
            sizeof(WASMComponentInstSection));
    if (!component_section->parsed.component->sections[1]
             .parsed.component->sections[1].parsed.instance_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[1]
               .parsed.component->sections[1].parsed.instance_section,
           0, sizeof(WASMComponentInstSection));
    component_section->parsed.component->sections[1]
        .parsed.component->sections[1].parsed.instance_section->count = 1;
    component_section->parsed.component->sections[1]
        .parsed.component->sections[1].parsed.instance_section->instances =
        (WASMComponentInst *)wasm_runtime_malloc(sizeof(WASMComponentInst));
    if (!component_section->parsed.component->sections[1]
             .parsed.component->sections[1].parsed.instance_section->instances) {
        return false;
    }
    memset(component_section->parsed.component->sections[1]
               .parsed.component->sections[1].parsed.instance_section->instances,
           0, sizeof(WASMComponentInst));
    component_section->parsed.component->sections[1]
        .parsed.component->sections[1].parsed.instance_section->instances[0]
        .instance_expression_tag = WASM_COMP_INSTANCE_EXPRESSION_WITH_ARGS;
    component_section->parsed.component->sections[1]
        .parsed.component->sections[1].parsed.instance_section->instances[0]
        .expression.with_args.idx = 0;

    component_section->parsed.component->sections[1]
        .parsed.component->sections[2].id = WASM_COMP_SECTION_EXPORTS;
    component_section->parsed.component->sections[1]
        .parsed.component->sections[2].parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!component_section->parsed.component->sections[1]
             .parsed.component->sections[2].parsed.export_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[1]
               .parsed.component->sections[2].parsed.export_section,
           0, sizeof(WASMComponentExportSection));
    component_section->parsed.component->sections[1]
        .parsed.component->sections[2].parsed.export_section->count = 1;
    component_section->parsed.component->sections[1]
        .parsed.component->sections[2].parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!component_section->parsed.component->sections[1]
             .parsed.component->sections[2].parsed.export_section->exports) {
        return false;
    }
    memset(component_section->parsed.component->sections[1]
               .parsed.component->sections[2].parsed.export_section->exports,
           0, sizeof(WASMComponentExport));
    component_section->parsed.component->sections[1]
        .parsed.component->sections[2].parsed.export_section->exports[0]
        .export_name = create_export_name("parent-outer-alias-instance");
    component_section->parsed.component->sections[1]
        .parsed.component->sections[2].parsed.export_section->exports[0]
        .sort_idx = create_sort_idx(WASM_COMP_SORT_INSTANCE, 0);
    if (!component_section->parsed.component->sections[1]
             .parsed.component->sections[2].parsed.export_section->exports[0]
             .export_name
        || !component_section->parsed.component->sections[1]
                 .parsed.component->sections[2].parsed.export_section->exports[0]
                 .sort_idx) {
        return false;
    }

    component_section->parsed.component->sections[2].id = WASM_COMP_SECTION_INSTANCES;
    component_section->parsed.component->sections[2].parsed.instance_section =
        (WASMComponentInstSection *)wasm_runtime_malloc(
            sizeof(WASMComponentInstSection));
    if (!component_section->parsed.component->sections[2].parsed.instance_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[2].parsed.instance_section, 0,
           sizeof(WASMComponentInstSection));
    component_section->parsed.component->sections[2].parsed.instance_section->count = 1;
    component_section->parsed.component->sections[2].parsed.instance_section->instances =
        (WASMComponentInst *)wasm_runtime_malloc(sizeof(WASMComponentInst));
    if (!component_section->parsed.component->sections[2]
             .parsed.instance_section->instances) {
        return false;
    }
    memset(component_section->parsed.component->sections[2]
               .parsed.instance_section->instances,
           0, sizeof(WASMComponentInst));
    component_section->parsed.component->sections[2]
        .parsed.instance_section->instances[0]
        .instance_expression_tag = WASM_COMP_INSTANCE_EXPRESSION_WITH_ARGS;
    component_section->parsed.component->sections[2]
        .parsed.instance_section->instances[0].expression.with_args.idx = 1;

    component_section->parsed.component->sections[3].id = WASM_COMP_SECTION_EXPORTS;
    component_section->parsed.component->sections[3].parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!component_section->parsed.component->sections[3].parsed.export_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[3].parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    component_section->parsed.component->sections[3].parsed.export_section->count = 1;
    component_section->parsed.component->sections[3].parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!component_section->parsed.component->sections[3]
             .parsed.export_section->exports) {
        return false;
    }
    memset(component_section->parsed.component->sections[3]
               .parsed.export_section->exports,
           0, sizeof(WASMComponentExport));
    component_section->parsed.component->sections[3]
        .parsed.export_section->exports[0].export_name =
        create_export_name("ct1-outer-alias");
    component_section->parsed.component->sections[3]
        .parsed.export_section->exports[0].sort_idx =
        create_sort_idx(WASM_COMP_SORT_INSTANCE, 0);
    if (!component_section->parsed.component->sections[3]
             .parsed.export_section->exports[0].export_name
        || !component_section->parsed.component->sections[3]
                 .parsed.export_section->exports[0].sort_idx) {
        return false;
    }

    instance_section->id = WASM_COMP_SECTION_INSTANCES;
    instance_section->parsed.instance_section =
        (WASMComponentInstSection *)wasm_runtime_malloc(sizeof(WASMComponentInstSection));
    if (!instance_section->parsed.instance_section) {
        return false;
    }
    memset(instance_section->parsed.instance_section, 0,
           sizeof(WASMComponentInstSection));
    instance_section->parsed.instance_section->count = 1;
    instance_section->parsed.instance_section->instances =
        (WASMComponentInst *)wasm_runtime_malloc(sizeof(WASMComponentInst));
    if (!instance_section->parsed.instance_section->instances) {
        return false;
    }
    memset(instance_section->parsed.instance_section->instances, 0,
           sizeof(WASMComponentInst));
    instance_section->parsed.instance_section->instances[0].instance_expression_tag =
        WASM_COMP_INSTANCE_EXPRESSION_WITH_ARGS;
    instance_section->parsed.instance_section->instances[0].expression.with_args.idx = 1;

    export_section->id = WASM_COMP_SECTION_EXPORTS;
    export_section->parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!export_section->parsed.export_section) {
        return false;
    }
    memset(export_section->parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    export_section->parsed.export_section->count = 1;
    export_section->parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!export_section->parsed.export_section->exports) {
        return false;
    }
    memset(export_section->parsed.export_section->exports, 0,
           sizeof(WASMComponentExport));
    export_section->parsed.export_section->exports[0].export_name =
        create_export_name("ct1-top-level-outer-alias");
    export_section->parsed.export_section->exports[0].sort_idx =
        create_sort_idx(WASM_COMP_SORT_INSTANCE, 1);
    return export_section->parsed.export_section->exports[0].export_name
           && export_section->parsed.export_section->exports[0].sort_idx;
}

static bool
append_top_level_start_section(WASMComponentModule *component_module,
                               uint32_t func_idx,
                               const uint32_t *value_args,
                               uint32_t value_args_count,
                               uint32_t result_count)
{
    WASMComponent *component = &component_module->component;
    const uint32_t old_count = component->section_count;
    const uint32_t new_count = old_count + 1;
    auto *new_sections = (WASMComponentSection *)wasm_runtime_malloc(
        sizeof(WASMComponentSection) * new_count);
    if (!new_sections) {
        return false;
    }

    memset(new_sections, 0, sizeof(WASMComponentSection) * new_count);
    memcpy(new_sections, component->sections,
           sizeof(WASMComponentSection) * old_count);
    wasm_runtime_free(component->sections);
    component->sections = new_sections;
    component->section_count = new_count;

    WASMComponentSection *start_section = &component->sections[old_count];
    start_section->id = WASM_COMP_SECTION_START;
    start_section->parsed.start_section =
        (WASMComponentStartSection *)wasm_runtime_malloc(
            sizeof(WASMComponentStartSection));
    if (!start_section->parsed.start_section) {
        return false;
    }

    memset(start_section->parsed.start_section, 0, sizeof(WASMComponentStartSection));
    start_section->parsed.start_section->func_idx = func_idx;
    start_section->parsed.start_section->value_args_count = value_args_count;
    start_section->parsed.start_section->result = result_count;
    if (value_args_count > 0) {
        start_section->parsed.start_section->value_args =
            (uint32_t *)wasm_runtime_malloc(sizeof(uint32_t) * value_args_count);
        if (!start_section->parsed.start_section->value_args) {
            return false;
        }
        memcpy(start_section->parsed.start_section->value_args, value_args,
               sizeof(uint32_t) * value_args_count);
    }
    return true;
}

static bool
append_nested_start_section(WASMComponentModule *component_module,
                            uint32_t result_count)
{
    WASMComponent *component = &component_module->component;
    const uint32_t component_idx =
        count_top_level_sort_entries(component, WASM_COMP_SORT_COMPONENT);
    const uint32_t instance_idx =
        count_top_level_sort_entries(component, WASM_COMP_SORT_INSTANCE);
    const uint32_t old_count = component->section_count;
    const uint32_t new_count = old_count + 3;
    const uint32_t start_args[] = { 0, 1 };
    auto *new_sections = (WASMComponentSection *)wasm_runtime_malloc(
        sizeof(WASMComponentSection) * new_count);
    if (!new_sections) {
        return false;
    }

    memset(new_sections, 0, sizeof(WASMComponentSection) * new_count);
    memcpy(new_sections, component->sections,
           sizeof(WASMComponentSection) * old_count);
    wasm_runtime_free(component->sections);
    component->sections = new_sections;
    component->section_count = new_count;

    WASMComponentSection *component_section = &component->sections[old_count];
    WASMComponentSection *instance_section = &component->sections[old_count + 1];
    WASMComponentSection *export_section = &component->sections[old_count + 2];
    component_section->id = WASM_COMP_SECTION_COMPONENT;
    component_section->parsed.component = create_empty_component();
    if (!component_section->parsed.component) {
        return false;
    }

    component_section->parsed.component->section_count = 5;
    component_section->parsed.component->sections =
        (WASMComponentSection *)wasm_runtime_malloc(sizeof(WASMComponentSection) * 5);
    if (!component_section->parsed.component->sections) {
        return false;
    }
    memset(component_section->parsed.component->sections, 0,
           sizeof(WASMComponentSection) * 5);

    component_section->parsed.component->sections[0].id = WASM_COMP_SECTION_IMPORTS;
    component_section->parsed.component->sections[0].parsed.import_section =
        (WASMComponentImportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentImportSection));
    if (!component_section->parsed.component->sections[0].parsed.import_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[0].parsed.import_section, 0,
           sizeof(WASMComponentImportSection));
    component_section->parsed.component->sections[0].parsed.import_section->count = 1;
    component_section->parsed.component->sections[0].parsed.import_section->imports =
        (WASMComponentImport *)wasm_runtime_malloc(sizeof(WASMComponentImport));
    if (!component_section->parsed.component->sections[0]
             .parsed.import_section->imports) {
        return false;
    }
    memset(component_section->parsed.component->sections[0]
               .parsed.import_section->imports,
           0, sizeof(WASMComponentImport));
    component_section->parsed.component->sections[0]
        .parsed.import_section->imports[0]
        .import_name = create_import_name("source");
    component_section->parsed.component->sections[0]
        .parsed.import_section->imports[0]
        .extern_desc = create_extern_desc(WASM_COMP_EXTERN_FUNC);
    if (!component_section->parsed.component->sections[0]
             .parsed.import_section->imports[0]
             .import_name
        || !component_section->parsed.component->sections[0]
                 .parsed.import_section->imports[0]
                 .extern_desc) {
        return false;
    }

    component_section->parsed.component->sections[1].id = WASM_COMP_SECTION_VALUES;
    component_section->parsed.component->sections[1].parsed.value_section =
        (WASMComponentValueSection *)wasm_runtime_malloc(
            sizeof(WASMComponentValueSection));
    if (!component_section->parsed.component->sections[1].parsed.value_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[1].parsed.value_section, 0,
           sizeof(WASMComponentValueSection));
    if (!init_scalar_value_section_with_bytes(
            component_section->parsed.component->sections[1].parsed.value_section,
            runtime_value_section_i32_two_bytes,
            (uint32_t)sizeof(runtime_value_section_i32_two_bytes))) {
        return false;
    }

    component_section->parsed.component->sections[2].id = WASM_COMP_SECTION_VALUES;
    component_section->parsed.component->sections[2].parsed.value_section =
        (WASMComponentValueSection *)wasm_runtime_malloc(
            sizeof(WASMComponentValueSection));
    if (!component_section->parsed.component->sections[2].parsed.value_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[2].parsed.value_section, 0,
           sizeof(WASMComponentValueSection));
    if (!init_scalar_value_section_with_bytes(
            component_section->parsed.component->sections[2].parsed.value_section,
            runtime_value_section_i32_three_bytes,
            (uint32_t)sizeof(runtime_value_section_i32_three_bytes))) {
        return false;
    }

    component_section->parsed.component->sections[3].id = WASM_COMP_SECTION_START;
    component_section->parsed.component->sections[3].parsed.start_section =
        (WASMComponentStartSection *)wasm_runtime_malloc(
            sizeof(WASMComponentStartSection));
    if (!component_section->parsed.component->sections[3].parsed.start_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[3].parsed.start_section, 0,
           sizeof(WASMComponentStartSection));
    component_section->parsed.component->sections[3].parsed.start_section->func_idx =
        0;
    component_section->parsed.component->sections[3]
        .parsed.start_section->value_args_count = 2;
    component_section->parsed.component->sections[3].parsed.start_section->result =
        result_count;
    component_section->parsed.component->sections[3].parsed.start_section->value_args =
        (uint32_t *)wasm_runtime_malloc(sizeof(start_args));
    if (!component_section->parsed.component->sections[3]
             .parsed.start_section->value_args) {
        return false;
    }
    memcpy(component_section->parsed.component->sections[3]
               .parsed.start_section->value_args,
           start_args, sizeof(start_args));

    component_section->parsed.component->sections[4].id = WASM_COMP_SECTION_EXPORTS;
    component_section->parsed.component->sections[4].parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!component_section->parsed.component->sections[4].parsed.export_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[4].parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    component_section->parsed.component->sections[4].parsed.export_section->count =
        1;
    component_section->parsed.component->sections[4].parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!component_section->parsed.component->sections[4]
             .parsed.export_section->exports) {
        return false;
    }
    memset(component_section->parsed.component->sections[4]
               .parsed.export_section->exports,
           0, sizeof(WASMComponentExport));
    component_section->parsed.component->sections[4]
        .parsed.export_section->exports[0]
        .export_name = create_export_name("start-result");
    component_section->parsed.component->sections[4]
        .parsed.export_section->exports[0]
        .sort_idx = create_sort_idx(WASM_COMP_SORT_VALUE, 2);
    if (!component_section->parsed.component->sections[4]
             .parsed.export_section->exports[0]
             .export_name
        || !component_section->parsed.component->sections[4]
                 .parsed.export_section->exports[0]
                 .sort_idx) {
        return false;
    }

    instance_section->id = WASM_COMP_SECTION_INSTANCES;
    instance_section->parsed.instance_section =
        (WASMComponentInstSection *)wasm_runtime_malloc(
            sizeof(WASMComponentInstSection));
    if (!instance_section->parsed.instance_section) {
        return false;
    }
    memset(instance_section->parsed.instance_section, 0,
           sizeof(WASMComponentInstSection));
    instance_section->parsed.instance_section->count = 1;
    instance_section->parsed.instance_section->instances =
        (WASMComponentInst *)wasm_runtime_malloc(sizeof(WASMComponentInst));
    if (!instance_section->parsed.instance_section->instances) {
        return false;
    }
    memset(instance_section->parsed.instance_section->instances, 0,
           sizeof(WASMComponentInst));
    instance_section->parsed.instance_section->instances[0].instance_expression_tag =
        WASM_COMP_INSTANCE_EXPRESSION_WITH_ARGS;
    instance_section->parsed.instance_section->instances[0].expression.with_args.idx =
        component_idx;
    instance_section->parsed.instance_section->instances[0]
        .expression.with_args.arg_len = 1;
    instance_section->parsed.instance_section->instances[0]
        .expression.with_args.args =
        (WASMComponentInstArg *)wasm_runtime_malloc(sizeof(WASMComponentInstArg));
    if (!instance_section->parsed.instance_section->instances[0]
             .expression.with_args.args) {
        return false;
    }
    memset(instance_section->parsed.instance_section->instances[0]
               .expression.with_args.args,
           0, sizeof(WASMComponentInstArg));
    instance_section->parsed.instance_section->instances[0]
        .expression.with_args.args[0]
        .name = clone_core_name("source");
    instance_section->parsed.instance_section->instances[0]
        .expression.with_args.args[0]
        .idx.sort_idx = create_sort_idx(WASM_COMP_SORT_FUNC, 0);
    if (!instance_section->parsed.instance_section->instances[0]
             .expression.with_args.args[0]
             .name
        || !instance_section->parsed.instance_section->instances[0]
                 .expression.with_args.args[0]
                 .idx.sort_idx) {
        return false;
    }

    export_section->id = WASM_COMP_SECTION_EXPORTS;
    export_section->parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!export_section->parsed.export_section) {
        return false;
    }
    memset(export_section->parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    export_section->parsed.export_section->count = 1;
    export_section->parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!export_section->parsed.export_section->exports) {
        return false;
    }
    memset(export_section->parsed.export_section->exports, 0,
           sizeof(WASMComponentExport));
    export_section->parsed.export_section->exports[0].export_name =
        create_export_name("nested-start-instance");
    export_section->parsed.export_section->exports[0].sort_idx =
        create_sort_idx(WASM_COMP_SORT_INSTANCE, instance_idx);
    return export_section->parsed.export_section->exports[0].export_name
           && export_section->parsed.export_section->exports[0].sort_idx;
}

static bool
append_nested_value_section(WASMComponentModule *component_module)
{
    WASMComponent *component = &component_module->component;
    const uint32_t old_count = component->section_count;
    const uint32_t new_count = old_count + 2;
    auto *new_sections = (WASMComponentSection *)wasm_runtime_malloc(
        sizeof(WASMComponentSection) * new_count);
    if (!new_sections) {
        return false;
    }

    memset(new_sections, 0, sizeof(WASMComponentSection) * new_count);
    memcpy(new_sections, component->sections,
           sizeof(WASMComponentSection) * old_count);
    wasm_runtime_free(component->sections);
    component->sections = new_sections;
    component->section_count = new_count;

    WASMComponentSection *component_section = &component->sections[old_count];
    WASMComponentSection *instance_section = &component->sections[old_count + 1];

    component_section->id = WASM_COMP_SECTION_COMPONENT;
    component_section->parsed.component = create_empty_component();
    if (!component_section->parsed.component) {
        return false;
    }
    component_section->parsed.component->section_count = 1;
    component_section->parsed.component->sections =
        (WASMComponentSection *)wasm_runtime_malloc(sizeof(WASMComponentSection));
    if (!component_section->parsed.component->sections) {
        return false;
    }
    memset(component_section->parsed.component->sections, 0,
           sizeof(WASMComponentSection));
    component_section->parsed.component->sections[0].id = WASM_COMP_SECTION_VALUES;
    component_section->parsed.component->sections[0].parsed.value_section =
        (WASMComponentValueSection *)wasm_runtime_malloc(
            sizeof(WASMComponentValueSection));
    if (!component_section->parsed.component->sections[0].parsed.value_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[0].parsed.value_section, 0,
           sizeof(WASMComponentValueSection));
    if (!init_scalar_value_section(
            component_section->parsed.component->sections[0].parsed.value_section)) {
        return false;
    }

    instance_section->id = WASM_COMP_SECTION_INSTANCES;
    instance_section->parsed.instance_section =
        (WASMComponentInstSection *)wasm_runtime_malloc(
            sizeof(WASMComponentInstSection));
    if (!instance_section->parsed.instance_section) {
        return false;
    }
    memset(instance_section->parsed.instance_section, 0,
           sizeof(WASMComponentInstSection));
    instance_section->parsed.instance_section->count = 1;
    instance_section->parsed.instance_section->instances =
        (WASMComponentInst *)wasm_runtime_malloc(sizeof(WASMComponentInst));
    if (!instance_section->parsed.instance_section->instances) {
        return false;
    }
    memset(instance_section->parsed.instance_section->instances, 0,
           sizeof(WASMComponentInst));
    instance_section->parsed.instance_section->instances[0].instance_expression_tag =
        WASM_COMP_INSTANCE_EXPRESSION_WITH_ARGS;
    instance_section->parsed.instance_section->instances[0].expression.with_args.idx =
        1;
    return true;
}

static bool
append_top_level_s32_value_section(WASMComponentModule *component_module,
                                   const uint8_t *bytes, uint32_t byte_len);

static bool
append_top_level_value_section(WASMComponentModule *component_module)
{
    return append_top_level_s32_value_section(
        component_module, runtime_value_section_i32_bytes,
        (uint32_t)sizeof(runtime_value_section_i32_bytes));
}

static bool
append_top_level_s32_value_section(WASMComponentModule *component_module,
                                   const uint8_t *bytes, uint32_t byte_len)
{
    WASMComponent *component = &component_module->component;
    const uint32_t old_count = component->section_count;
    const uint32_t new_count = old_count + 1;
    auto *new_sections = (WASMComponentSection *)wasm_runtime_malloc(
        sizeof(WASMComponentSection) * new_count);
    if (!new_sections) {
        return false;
    }

    memset(new_sections, 0, sizeof(WASMComponentSection) * new_count);
    memcpy(new_sections, component->sections,
           sizeof(WASMComponentSection) * old_count);
    wasm_runtime_free(component->sections);
    component->sections = new_sections;
    component->section_count = new_count;

    WASMComponentSection *value_section = &component->sections[old_count];
    value_section->id = WASM_COMP_SECTION_VALUES;
    value_section->parsed.value_section =
        (WASMComponentValueSection *)wasm_runtime_malloc(
            sizeof(WASMComponentValueSection));
    if (!value_section->parsed.value_section) {
        return false;
    }

    memset(value_section->parsed.value_section, 0, sizeof(WASMComponentValueSection));
    return init_scalar_value_section_with_bytes(value_section->parsed.value_section,
                                                bytes, byte_len);
}

static bool
append_top_level_value_export_section(WASMComponentModule *component_module,
                                      const char *name, uint32_t value_idx)
{
    WASMComponent *component = &component_module->component;
    const uint32_t old_count = component->section_count;
    const uint32_t new_count = old_count + 1;
    auto *new_sections = (WASMComponentSection *)wasm_runtime_malloc(
        sizeof(WASMComponentSection) * new_count);
    if (!new_sections) {
        return false;
    }

    memset(new_sections, 0, sizeof(WASMComponentSection) * new_count);
    memcpy(new_sections, component->sections,
           sizeof(WASMComponentSection) * old_count);
    wasm_runtime_free(component->sections);
    component->sections = new_sections;
    component->section_count = new_count;

    WASMComponentSection *export_section = &component->sections[old_count];
    export_section->id = WASM_COMP_SECTION_EXPORTS;
    export_section->parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!export_section->parsed.export_section) {
        return false;
    }
    memset(export_section->parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    export_section->parsed.export_section->count = 1;
    export_section->parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!export_section->parsed.export_section->exports) {
        return false;
    }
    memset(export_section->parsed.export_section->exports, 0,
           sizeof(WASMComponentExport));
    export_section->parsed.export_section->exports[0].export_name =
        create_export_name(name);
    export_section->parsed.export_section->exports[0].sort_idx =
        create_sort_idx(WASM_COMP_SORT_VALUE, value_idx);
    return export_section->parsed.export_section->exports[0].export_name
           && export_section->parsed.export_section->exports[0].sort_idx;
}

static bool
append_public_top_level_value_export_sections(
    WASMComponentModule *component_module)
{
    WASMComponent *component = &component_module->component;
    const uint32_t value_idx =
        count_top_level_sort_entries(component, WASM_COMP_SORT_VALUE);
    const uint32_t old_count = component->section_count;
    const uint32_t new_count = old_count + 2;
    auto *new_sections = (WASMComponentSection *)wasm_runtime_malloc(
        sizeof(WASMComponentSection) * new_count);
    if (!new_sections) {
        return false;
    }

    memset(new_sections, 0, sizeof(WASMComponentSection) * new_count);
    memcpy(new_sections, component->sections,
           sizeof(WASMComponentSection) * old_count);
    wasm_runtime_free(component->sections);
    component->sections = new_sections;
    component->section_count = new_count;

    WASMComponentSection *value_section = &component->sections[old_count];
    WASMComponentSection *export_section = &component->sections[old_count + 1];

    value_section->id = WASM_COMP_SECTION_VALUES;
    value_section->parsed.value_section =
        (WASMComponentValueSection *)wasm_runtime_malloc(
            sizeof(WASMComponentValueSection));
    if (!value_section->parsed.value_section) {
        return false;
    }
    memset(value_section->parsed.value_section, 0, sizeof(WASMComponentValueSection));
    if (!init_scalar_value_section(value_section->parsed.value_section)) {
        return false;
    }

    export_section->id = WASM_COMP_SECTION_EXPORTS;
    export_section->parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!export_section->parsed.export_section) {
        return false;
    }
    memset(export_section->parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    export_section->parsed.export_section->count = 1;
    export_section->parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!export_section->parsed.export_section->exports) {
        return false;
    }
    memset(export_section->parsed.export_section->exports, 0,
           sizeof(WASMComponentExport));
    export_section->parsed.export_section->exports[0].export_name =
        create_export_name("exported-value");
    export_section->parsed.export_section->exports[0].sort_idx =
        create_sort_idx(WASM_COMP_SORT_VALUE, value_idx);
    return export_section->parsed.export_section->exports[0].export_name
           && export_section->parsed.export_section->exports[0].sort_idx;
}

static bool
append_top_level_component_import_sections(WASMComponentModule *component_module)
{
    WASMComponent *component = &component_module->component;
    const uint32_t old_count = component->section_count;
    const uint32_t new_count = old_count + 4;
    auto *new_sections = (WASMComponentSection *)wasm_runtime_malloc(
        sizeof(WASMComponentSection) * new_count);
    if (!new_sections) {
        return false;
    }

    memset(new_sections, 0, sizeof(WASMComponentSection) * new_count);
    memcpy(new_sections, component->sections,
           sizeof(WASMComponentSection) * old_count);
    wasm_runtime_free(component->sections);
    component->sections = new_sections;
    component->section_count = new_count;

    WASMComponentSection *target_section = &component->sections[old_count];
    WASMComponentSection *wrapper_section = &component->sections[old_count + 1];
    WASMComponentSection *instance_section = &component->sections[old_count + 2];
    WASMComponentSection *export_section = &component->sections[old_count + 3];

    target_section->id = WASM_COMP_SECTION_COMPONENT;
    target_section->parsed.component = create_empty_component();
    if (!target_section->parsed.component) {
        return false;
    }

    wrapper_section->id = WASM_COMP_SECTION_COMPONENT;
    wrapper_section->parsed.component = create_empty_component();
    if (!wrapper_section->parsed.component) {
        return false;
    }
    wrapper_section->parsed.component->section_count = 2;
    wrapper_section->parsed.component->sections =
        (WASMComponentSection *)wasm_runtime_malloc(sizeof(WASMComponentSection) * 2);
    if (!wrapper_section->parsed.component->sections) {
        return false;
    }
    memset(wrapper_section->parsed.component->sections, 0,
           sizeof(WASMComponentSection) * 2);

    wrapper_section->parsed.component->sections[0].id = WASM_COMP_SECTION_IMPORTS;
    wrapper_section->parsed.component->sections[0].parsed.import_section =
        (WASMComponentImportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentImportSection));
    if (!wrapper_section->parsed.component->sections[0].parsed.import_section) {
        return false;
    }
    memset(wrapper_section->parsed.component->sections[0].parsed.import_section, 0,
           sizeof(WASMComponentImportSection));
    wrapper_section->parsed.component->sections[0].parsed.import_section->count = 1;
    wrapper_section->parsed.component->sections[0].parsed.import_section->imports =
        (WASMComponentImport *)wasm_runtime_malloc(sizeof(WASMComponentImport));
    if (!wrapper_section->parsed.component->sections[0]
             .parsed.import_section->imports) {
        return false;
    }
    memset(wrapper_section->parsed.component->sections[0]
               .parsed.import_section->imports,
           0, sizeof(WASMComponentImport));
    wrapper_section->parsed.component->sections[0]
        .parsed.import_section->imports[0].import_name =
        create_import_name("source");
    wrapper_section->parsed.component->sections[0]
        .parsed.import_section->imports[0].extern_desc =
        create_extern_desc(WASM_COMP_EXTERN_COMPONENT);
    if (!wrapper_section->parsed.component->sections[0]
             .parsed.import_section->imports[0].import_name
        || !wrapper_section->parsed.component->sections[0]
                 .parsed.import_section->imports[0].extern_desc) {
        return false;
    }

    wrapper_section->parsed.component->sections[1].id = WASM_COMP_SECTION_EXPORTS;
    wrapper_section->parsed.component->sections[1].parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!wrapper_section->parsed.component->sections[1].parsed.export_section) {
        return false;
    }
    memset(wrapper_section->parsed.component->sections[1].parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    wrapper_section->parsed.component->sections[1].parsed.export_section->count = 1;
    wrapper_section->parsed.component->sections[1].parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!wrapper_section->parsed.component->sections[1]
             .parsed.export_section->exports) {
        return false;
    }
    memset(wrapper_section->parsed.component->sections[1]
               .parsed.export_section->exports,
           0, sizeof(WASMComponentExport));
    wrapper_section->parsed.component->sections[1]
        .parsed.export_section->exports[0].export_name =
        create_export_name("forwarded-component");
    wrapper_section->parsed.component->sections[1]
        .parsed.export_section->exports[0].sort_idx =
        create_sort_idx(WASM_COMP_SORT_COMPONENT, 0);
    if (!wrapper_section->parsed.component->sections[1]
             .parsed.export_section->exports[0].export_name
        || !wrapper_section->parsed.component->sections[1]
                 .parsed.export_section->exports[0].sort_idx) {
        return false;
    }

    instance_section->id = WASM_COMP_SECTION_INSTANCES;
    instance_section->parsed.instance_section =
        (WASMComponentInstSection *)wasm_runtime_malloc(
            sizeof(WASMComponentInstSection));
    if (!instance_section->parsed.instance_section) {
        return false;
    }
    memset(instance_section->parsed.instance_section, 0,
           sizeof(WASMComponentInstSection));
    instance_section->parsed.instance_section->count = 1;
    instance_section->parsed.instance_section->instances =
        (WASMComponentInst *)wasm_runtime_malloc(sizeof(WASMComponentInst));
    if (!instance_section->parsed.instance_section->instances) {
        return false;
    }
    memset(instance_section->parsed.instance_section->instances, 0,
           sizeof(WASMComponentInst));
    instance_section->parsed.instance_section->instances[0].instance_expression_tag =
        WASM_COMP_INSTANCE_EXPRESSION_WITH_ARGS;
    instance_section->parsed.instance_section->instances[0].expression.with_args.idx =
        2;
    instance_section->parsed.instance_section->instances[0].expression.with_args.arg_len =
        1;
    instance_section->parsed.instance_section->instances[0].expression.with_args.args =
        (WASMComponentInstArg *)wasm_runtime_malloc(sizeof(WASMComponentInstArg));
    if (!instance_section->parsed.instance_section->instances[0]
             .expression.with_args.args) {
        return false;
    }
    memset(instance_section->parsed.instance_section->instances[0]
               .expression.with_args.args,
           0, sizeof(WASMComponentInstArg));
    instance_section->parsed.instance_section->instances[0]
        .expression.with_args.args[0].name = clone_core_name("source");
    instance_section->parsed.instance_section->instances[0]
        .expression.with_args.args[0].idx.sort_idx =
        create_sort_idx(WASM_COMP_SORT_COMPONENT, 1);
    if (!instance_section->parsed.instance_section->instances[0]
             .expression.with_args.args[0].name
        || !instance_section->parsed.instance_section->instances[0]
                 .expression.with_args.args[0].idx.sort_idx) {
        return false;
    }

    export_section->id = WASM_COMP_SECTION_EXPORTS;
    export_section->parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!export_section->parsed.export_section) {
        return false;
    }
    memset(export_section->parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    export_section->parsed.export_section->count = 1;
    export_section->parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!export_section->parsed.export_section->exports) {
        return false;
    }
    memset(export_section->parsed.export_section->exports, 0,
           sizeof(WASMComponentExport));
    export_section->parsed.export_section->exports[0].export_name =
        create_export_name("component-import-instance");
    export_section->parsed.export_section->exports[0].sort_idx =
        create_sort_idx(WASM_COMP_SORT_INSTANCE, 1);
    return export_section->parsed.export_section->exports[0].export_name
           && export_section->parsed.export_section->exports[0].sort_idx;
}

static bool
append_top_level_value_alias_sections(WASMComponentModule *component_module)
{
    WASMComponent *component = &component_module->component;
    const uint32_t value_idx =
        count_top_level_sort_entries(component, WASM_COMP_SORT_VALUE);
    const uint32_t component_idx =
        count_top_level_sort_entries(component, WASM_COMP_SORT_COMPONENT);
    const uint32_t instance_idx =
        count_top_level_sort_entries(component, WASM_COMP_SORT_INSTANCE);
    const uint32_t aliased_value_idx = value_idx + 1;
    const uint32_t old_count = component->section_count;
    const uint32_t new_count = old_count + 5;
    auto *new_sections = (WASMComponentSection *)wasm_runtime_malloc(
        sizeof(WASMComponentSection) * new_count);
    if (!new_sections) {
        return false;
    }

    memset(new_sections, 0, sizeof(WASMComponentSection) * new_count);
    memcpy(new_sections, component->sections,
           sizeof(WASMComponentSection) * old_count);
    wasm_runtime_free(component->sections);
    component->sections = new_sections;
    component->section_count = new_count;

    WASMComponentSection *value_section = &component->sections[old_count];
    WASMComponentSection *component_section = &component->sections[old_count + 1];
    WASMComponentSection *instance_section = &component->sections[old_count + 2];
    WASMComponentSection *alias_section = &component->sections[old_count + 3];
    WASMComponentSection *export_section = &component->sections[old_count + 4];

    value_section->id = WASM_COMP_SECTION_VALUES;
    value_section->parsed.value_section =
        (WASMComponentValueSection *)wasm_runtime_malloc(
            sizeof(WASMComponentValueSection));
    if (!value_section->parsed.value_section) {
        return false;
    }
    memset(value_section->parsed.value_section, 0, sizeof(WASMComponentValueSection));
    if (!init_scalar_value_section(value_section->parsed.value_section)) {
        return false;
    }

    component_section->id = WASM_COMP_SECTION_COMPONENT;
    component_section->parsed.component = create_empty_component();
    if (!component_section->parsed.component) {
        return false;
    }
    component_section->parsed.component->section_count = 2;
    component_section->parsed.component->sections =
        (WASMComponentSection *)wasm_runtime_malloc(sizeof(WASMComponentSection) * 2);
    if (!component_section->parsed.component->sections) {
        return false;
    }
    memset(component_section->parsed.component->sections, 0,
           sizeof(WASMComponentSection) * 2);

    component_section->parsed.component->sections[0].id = WASM_COMP_SECTION_IMPORTS;
    component_section->parsed.component->sections[0].parsed.import_section =
        (WASMComponentImportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentImportSection));
    if (!component_section->parsed.component->sections[0].parsed.import_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[0].parsed.import_section, 0,
           sizeof(WASMComponentImportSection));
    component_section->parsed.component->sections[0].parsed.import_section->count = 1;
    component_section->parsed.component->sections[0].parsed.import_section->imports =
        (WASMComponentImport *)wasm_runtime_malloc(sizeof(WASMComponentImport));
    if (!component_section->parsed.component->sections[0]
             .parsed.import_section->imports) {
        return false;
    }
    memset(component_section->parsed.component->sections[0]
               .parsed.import_section->imports,
           0, sizeof(WASMComponentImport));
    component_section->parsed.component->sections[0]
        .parsed.import_section->imports[0].import_name =
        create_import_name("source");
    component_section->parsed.component->sections[0]
        .parsed.import_section->imports[0].extern_desc =
        create_extern_desc(WASM_COMP_EXTERN_VALUE);
    if (!component_section->parsed.component->sections[0]
             .parsed.import_section->imports[0].import_name
        || !component_section->parsed.component->sections[0]
                 .parsed.import_section->imports[0].extern_desc) {
        return false;
    }

    component_section->parsed.component->sections[1].id = WASM_COMP_SECTION_EXPORTS;
    component_section->parsed.component->sections[1].parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!component_section->parsed.component->sections[1].parsed.export_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[1].parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    component_section->parsed.component->sections[1].parsed.export_section->count = 1;
    component_section->parsed.component->sections[1].parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!component_section->parsed.component->sections[1]
             .parsed.export_section->exports) {
        return false;
    }
    memset(component_section->parsed.component->sections[1]
               .parsed.export_section->exports,
           0, sizeof(WASMComponentExport));
    component_section->parsed.component->sections[1]
        .parsed.export_section->exports[0].export_name =
        create_export_name("forwarded-value");
    component_section->parsed.component->sections[1]
        .parsed.export_section->exports[0].sort_idx =
        create_sort_idx(WASM_COMP_SORT_VALUE, 0);
    if (!component_section->parsed.component->sections[1]
             .parsed.export_section->exports[0].export_name
        || !component_section->parsed.component->sections[1]
                 .parsed.export_section->exports[0].sort_idx) {
        return false;
    }

    instance_section->id = WASM_COMP_SECTION_INSTANCES;
    instance_section->parsed.instance_section =
        (WASMComponentInstSection *)wasm_runtime_malloc(
            sizeof(WASMComponentInstSection));
    if (!instance_section->parsed.instance_section) {
        return false;
    }
    memset(instance_section->parsed.instance_section, 0,
           sizeof(WASMComponentInstSection));
    instance_section->parsed.instance_section->count = 1;
    instance_section->parsed.instance_section->instances =
        (WASMComponentInst *)wasm_runtime_malloc(sizeof(WASMComponentInst));
    if (!instance_section->parsed.instance_section->instances) {
        return false;
    }
    memset(instance_section->parsed.instance_section->instances, 0,
           sizeof(WASMComponentInst));
    instance_section->parsed.instance_section->instances[0].instance_expression_tag =
        WASM_COMP_INSTANCE_EXPRESSION_WITH_ARGS;
    instance_section->parsed.instance_section->instances[0].expression.with_args.idx =
        component_idx;
    instance_section->parsed.instance_section->instances[0].expression.with_args.arg_len =
        1;
    instance_section->parsed.instance_section->instances[0].expression.with_args.args =
        (WASMComponentInstArg *)wasm_runtime_malloc(sizeof(WASMComponentInstArg));
    if (!instance_section->parsed.instance_section->instances[0]
             .expression.with_args.args) {
        return false;
    }
    memset(instance_section->parsed.instance_section->instances[0]
               .expression.with_args.args,
           0, sizeof(WASMComponentInstArg));
    instance_section->parsed.instance_section->instances[0]
        .expression.with_args.args[0].name = clone_core_name("source");
    instance_section->parsed.instance_section->instances[0]
        .expression.with_args.args[0].idx.sort_idx =
        create_sort_idx(WASM_COMP_SORT_VALUE, value_idx);
    if (!instance_section->parsed.instance_section->instances[0]
             .expression.with_args.args[0].name
        || !instance_section->parsed.instance_section->instances[0]
                 .expression.with_args.args[0].idx.sort_idx) {
        return false;
    }

    alias_section->id = WASM_COMP_SECTION_ALIASES;
    alias_section->parsed.alias_section =
        (WASMComponentAliasSection *)wasm_runtime_malloc(
            sizeof(WASMComponentAliasSection));
    if (!alias_section->parsed.alias_section) {
        return false;
    }
    memset(alias_section->parsed.alias_section, 0,
           sizeof(WASMComponentAliasSection));
    alias_section->parsed.alias_section->count = 1;
    alias_section->parsed.alias_section->aliases =
        (WASMComponentAliasDefinition *)wasm_runtime_malloc(
            sizeof(WASMComponentAliasDefinition));
    if (!alias_section->parsed.alias_section->aliases) {
        return false;
    }
    memset(alias_section->parsed.alias_section->aliases, 0,
           sizeof(WASMComponentAliasDefinition));
    alias_section->parsed.alias_section->aliases[0].sort =
        create_sort(WASM_COMP_SORT_VALUE);
    alias_section->parsed.alias_section->aliases[0].alias_target_type =
        WASM_COMP_ALIAS_TARGET_EXPORT;
    alias_section->parsed.alias_section->aliases[0].target.exported.instance_idx =
        instance_idx;
    alias_section->parsed.alias_section->aliases[0].target.exported.name =
        clone_core_name("forwarded-value");
    if (!alias_section->parsed.alias_section->aliases[0].sort
        || !alias_section->parsed.alias_section->aliases[0]
                .target.exported.name) {
        return false;
    }

    export_section->id = WASM_COMP_SECTION_EXPORTS;
    export_section->parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!export_section->parsed.export_section) {
        return false;
    }
    memset(export_section->parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    export_section->parsed.export_section->count = 2;
    export_section->parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport) * 2);
    if (!export_section->parsed.export_section->exports) {
        return false;
    }
    memset(export_section->parsed.export_section->exports, 0,
           sizeof(WASMComponentExport) * 2);
    export_section->parsed.export_section->exports[0].export_name =
        create_export_name("value-import-instance");
    export_section->parsed.export_section->exports[0].sort_idx =
        create_sort_idx(WASM_COMP_SORT_INSTANCE, instance_idx);
    export_section->parsed.export_section->exports[1].export_name =
        create_export_name("aliased-value");
    export_section->parsed.export_section->exports[1].sort_idx =
        create_sort_idx(WASM_COMP_SORT_VALUE, aliased_value_idx);
    return export_section->parsed.export_section->exports[0].export_name
           && export_section->parsed.export_section->exports[0].sort_idx
           && export_section->parsed.export_section->exports[1].export_name
           && export_section->parsed.export_section->exports[1].sort_idx;
}

static bool
append_top_level_direct_component_import_sections(
    WASMComponentModule *component_module)
{
    WASMComponent *component = &component_module->component;
    uint32_t component_sort_count = 0;
    const uint32_t old_count = component->section_count;
    const uint32_t new_count = old_count + 2;
    auto *new_sections = (WASMComponentSection *)wasm_runtime_malloc(
        sizeof(WASMComponentSection) * new_count);
    if (!new_sections) {
        return false;
    }

    memset(new_sections, 0, sizeof(WASMComponentSection) * new_count);
    memcpy(new_sections, component->sections,
           sizeof(WASMComponentSection) * old_count);
    wasm_runtime_free(component->sections);
    component->sections = new_sections;
    component->section_count = new_count;

    WASMComponentSection *import_section = &component->sections[old_count];
    WASMComponentSection *export_section = &component->sections[old_count + 1];

    for (uint32_t i = 0; i < old_count; i++) {
        const WASMComponentSection *section = &component->sections[i];
        if (section->id == WASM_COMP_SECTION_COMPONENT) {
            component_sort_count++;
        }
        else if (section->id == WASM_COMP_SECTION_ALIASES) {
            for (uint32_t j = 0; j < section->parsed.alias_section->count; j++) {
                const WASMComponentAliasDefinition *alias_def =
                    &section->parsed.alias_section->aliases[j];
                if (alias_def->sort
                    && alias_def->sort->sort == WASM_COMP_SORT_COMPONENT) {
                    component_sort_count++;
                }
            }
        }
    }

    import_section->id = WASM_COMP_SECTION_IMPORTS;
    import_section->parsed.import_section =
        (WASMComponentImportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentImportSection));
    if (!import_section->parsed.import_section) {
        return false;
    }
    memset(import_section->parsed.import_section, 0,
           sizeof(WASMComponentImportSection));
    import_section->parsed.import_section->count = 1;
    import_section->parsed.import_section->imports =
        (WASMComponentImport *)wasm_runtime_malloc(sizeof(WASMComponentImport));
    if (!import_section->parsed.import_section->imports) {
        return false;
    }
    memset(import_section->parsed.import_section->imports, 0,
           sizeof(WASMComponentImport));
    import_section->parsed.import_section->imports[0].import_name =
        create_import_name("source");
    import_section->parsed.import_section->imports[0].extern_desc =
        create_extern_desc(WASM_COMP_EXTERN_COMPONENT);
    if (!import_section->parsed.import_section->imports[0].import_name
        || !import_section->parsed.import_section->imports[0].extern_desc) {
        return false;
    }

    export_section->id = WASM_COMP_SECTION_EXPORTS;
    export_section->parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!export_section->parsed.export_section) {
        return false;
    }
    memset(export_section->parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    export_section->parsed.export_section->count = 1;
    export_section->parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!export_section->parsed.export_section->exports) {
        return false;
    }
    memset(export_section->parsed.export_section->exports, 0,
           sizeof(WASMComponentExport));
    export_section->parsed.export_section->exports[0].export_name =
        create_export_name("forwarded-source");
    export_section->parsed.export_section->exports[0].sort_idx =
        create_sort_idx(WASM_COMP_SORT_COMPONENT, component_sort_count);
    return export_section->parsed.export_section->exports[0].export_name
           && export_section->parsed.export_section->exports[0].sort_idx;
}

static bool
append_top_level_public_value_import_sections(
    WASMComponentModule *component_module)
{
    WASMComponent *component = &component_module->component;
    const uint32_t value_idx =
        count_top_level_sort_entries(component, WASM_COMP_SORT_VALUE);
    const uint32_t old_count = component->section_count;
    const uint32_t new_count = old_count + 2;
    auto *new_sections = (WASMComponentSection *)wasm_runtime_malloc(
        sizeof(WASMComponentSection) * new_count);
    if (!new_sections) {
        return false;
    }

    memset(new_sections, 0, sizeof(WASMComponentSection) * new_count);
    memcpy(new_sections, component->sections,
           sizeof(WASMComponentSection) * old_count);
    wasm_runtime_free(component->sections);
    component->sections = new_sections;
    component->section_count = new_count;

    WASMComponentSection *import_section = &component->sections[old_count];
    WASMComponentSection *export_section = &component->sections[old_count + 1];

    import_section->id = WASM_COMP_SECTION_IMPORTS;
    import_section->parsed.import_section =
        (WASMComponentImportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentImportSection));
    if (!import_section->parsed.import_section) {
        return false;
    }
    memset(import_section->parsed.import_section, 0,
           sizeof(WASMComponentImportSection));
    import_section->parsed.import_section->count = 1;
    import_section->parsed.import_section->imports =
        (WASMComponentImport *)wasm_runtime_malloc(sizeof(WASMComponentImport));
    if (!import_section->parsed.import_section->imports) {
        return false;
    }
    memset(import_section->parsed.import_section->imports, 0,
           sizeof(WASMComponentImport));
    import_section->parsed.import_section->imports[0].import_name =
        create_import_name("source");
    import_section->parsed.import_section->imports[0].extern_desc =
        create_primitive_value_extern_desc(WASM_COMP_PRIMVAL_S32);
    if (!import_section->parsed.import_section->imports[0].import_name
        || !import_section->parsed.import_section->imports[0].extern_desc) {
        return false;
    }

    export_section->id = WASM_COMP_SECTION_EXPORTS;
    export_section->parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!export_section->parsed.export_section) {
        return false;
    }
    memset(export_section->parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    export_section->parsed.export_section->count = 1;
    export_section->parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!export_section->parsed.export_section->exports) {
        return false;
    }
    memset(export_section->parsed.export_section->exports, 0,
           sizeof(WASMComponentExport));
    export_section->parsed.export_section->exports[0].export_name =
        create_export_name("forwarded-value");
    export_section->parsed.export_section->exports[0].sort_idx =
        create_sort_idx(WASM_COMP_SORT_VALUE, value_idx);
    return export_section->parsed.export_section->exports[0].export_name
           && export_section->parsed.export_section->exports[0].sort_idx;
}

static bool
append_nested_component_import_sections(WASMComponentModule *component_module)
{
    WASMComponent *component = &component_module->component;
    const uint32_t old_count = component->section_count;
    const uint32_t new_count = old_count + 3;
    auto *new_sections = (WASMComponentSection *)wasm_runtime_malloc(
        sizeof(WASMComponentSection) * new_count);
    if (!new_sections) {
        return false;
    }

    memset(new_sections, 0, sizeof(WASMComponentSection) * new_count);
    memcpy(new_sections, component->sections,
           sizeof(WASMComponentSection) * old_count);
    wasm_runtime_free(component->sections);
    component->sections = new_sections;
    component->section_count = new_count;

    WASMComponentSection *outer_section = &component->sections[old_count];
    WASMComponentSection *instance_section = &component->sections[old_count + 1];
    WASMComponentSection *export_section = &component->sections[old_count + 2];

    outer_section->id = WASM_COMP_SECTION_COMPONENT;
    outer_section->parsed.component = create_empty_component();
    if (!outer_section->parsed.component) {
        return false;
    }
    outer_section->parsed.component->section_count = 4;
    outer_section->parsed.component->sections =
        (WASMComponentSection *)wasm_runtime_malloc(sizeof(WASMComponentSection) * 4);
    if (!outer_section->parsed.component->sections) {
        return false;
    }
    memset(outer_section->parsed.component->sections, 0,
           sizeof(WASMComponentSection) * 4);

    outer_section->parsed.component->sections[0].id = WASM_COMP_SECTION_COMPONENT;
    outer_section->parsed.component->sections[0].parsed.component =
        create_empty_component();
    if (!outer_section->parsed.component->sections[0].parsed.component) {
        return false;
    }

    outer_section->parsed.component->sections[1].id = WASM_COMP_SECTION_COMPONENT;
    outer_section->parsed.component->sections[1].parsed.component =
        create_empty_component();
    if (!outer_section->parsed.component->sections[1].parsed.component) {
        return false;
    }
    outer_section->parsed.component->sections[1].parsed.component->section_count = 2;
    outer_section->parsed.component->sections[1].parsed.component->sections =
        (WASMComponentSection *)wasm_runtime_malloc(sizeof(WASMComponentSection) * 2);
    if (!outer_section->parsed.component->sections[1]
             .parsed.component->sections) {
        return false;
    }
    memset(outer_section->parsed.component->sections[1].parsed.component->sections,
           0, sizeof(WASMComponentSection) * 2);

    outer_section->parsed.component->sections[1]
        .parsed.component->sections[0].id = WASM_COMP_SECTION_IMPORTS;
    outer_section->parsed.component->sections[1]
        .parsed.component->sections[0].parsed.import_section =
        (WASMComponentImportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentImportSection));
    if (!outer_section->parsed.component->sections[1]
             .parsed.component->sections[0].parsed.import_section) {
        return false;
    }
    memset(outer_section->parsed.component->sections[1]
               .parsed.component->sections[0].parsed.import_section,
           0, sizeof(WASMComponentImportSection));
    outer_section->parsed.component->sections[1]
        .parsed.component->sections[0].parsed.import_section->count = 1;
    outer_section->parsed.component->sections[1]
        .parsed.component->sections[0].parsed.import_section->imports =
        (WASMComponentImport *)wasm_runtime_malloc(sizeof(WASMComponentImport));
    if (!outer_section->parsed.component->sections[1]
             .parsed.component->sections[0].parsed.import_section->imports) {
        return false;
    }
    memset(outer_section->parsed.component->sections[1]
               .parsed.component->sections[0].parsed.import_section->imports,
           0, sizeof(WASMComponentImport));
    outer_section->parsed.component->sections[1]
        .parsed.component->sections[0].parsed.import_section->imports[0]
        .import_name = create_import_name("source");
    outer_section->parsed.component->sections[1]
        .parsed.component->sections[0].parsed.import_section->imports[0]
        .extern_desc = create_extern_desc(WASM_COMP_EXTERN_COMPONENT);
    if (!outer_section->parsed.component->sections[1]
             .parsed.component->sections[0].parsed.import_section->imports[0]
             .import_name
        || !outer_section->parsed.component->sections[1]
                 .parsed.component->sections[0].parsed.import_section->imports[0]
                 .extern_desc) {
        return false;
    }

    outer_section->parsed.component->sections[1]
        .parsed.component->sections[1].id = WASM_COMP_SECTION_EXPORTS;
    outer_section->parsed.component->sections[1]
        .parsed.component->sections[1].parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!outer_section->parsed.component->sections[1]
             .parsed.component->sections[1].parsed.export_section) {
        return false;
    }
    memset(outer_section->parsed.component->sections[1]
               .parsed.component->sections[1].parsed.export_section,
           0, sizeof(WASMComponentExportSection));
    outer_section->parsed.component->sections[1]
        .parsed.component->sections[1].parsed.export_section->count = 1;
    outer_section->parsed.component->sections[1]
        .parsed.component->sections[1].parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!outer_section->parsed.component->sections[1]
             .parsed.component->sections[1].parsed.export_section->exports) {
        return false;
    }
    memset(outer_section->parsed.component->sections[1]
               .parsed.component->sections[1].parsed.export_section->exports,
           0, sizeof(WASMComponentExport));
    outer_section->parsed.component->sections[1]
        .parsed.component->sections[1].parsed.export_section->exports[0]
        .export_name = create_export_name("forwarded-component");
    outer_section->parsed.component->sections[1]
        .parsed.component->sections[1].parsed.export_section->exports[0]
        .sort_idx = create_sort_idx(WASM_COMP_SORT_COMPONENT, 0);
    if (!outer_section->parsed.component->sections[1]
             .parsed.component->sections[1].parsed.export_section->exports[0]
             .export_name
        || !outer_section->parsed.component->sections[1]
                 .parsed.component->sections[1].parsed.export_section->exports[0]
                 .sort_idx) {
        return false;
    }

    outer_section->parsed.component->sections[2].id = WASM_COMP_SECTION_INSTANCES;
    outer_section->parsed.component->sections[2].parsed.instance_section =
        (WASMComponentInstSection *)wasm_runtime_malloc(
            sizeof(WASMComponentInstSection));
    if (!outer_section->parsed.component->sections[2].parsed.instance_section) {
        return false;
    }
    memset(outer_section->parsed.component->sections[2].parsed.instance_section, 0,
           sizeof(WASMComponentInstSection));
    outer_section->parsed.component->sections[2].parsed.instance_section->count = 1;
    outer_section->parsed.component->sections[2].parsed.instance_section->instances =
        (WASMComponentInst *)wasm_runtime_malloc(sizeof(WASMComponentInst));
    if (!outer_section->parsed.component->sections[2]
             .parsed.instance_section->instances) {
        return false;
    }
    memset(outer_section->parsed.component->sections[2]
               .parsed.instance_section->instances,
           0, sizeof(WASMComponentInst));
    outer_section->parsed.component->sections[2]
        .parsed.instance_section->instances[0]
        .instance_expression_tag = WASM_COMP_INSTANCE_EXPRESSION_WITH_ARGS;
    outer_section->parsed.component->sections[2]
        .parsed.instance_section->instances[0].expression.with_args.idx = 1;
    outer_section->parsed.component->sections[2]
        .parsed.instance_section->instances[0].expression.with_args.arg_len = 1;
    outer_section->parsed.component->sections[2]
        .parsed.instance_section->instances[0].expression.with_args.args =
        (WASMComponentInstArg *)wasm_runtime_malloc(sizeof(WASMComponentInstArg));
    if (!outer_section->parsed.component->sections[2]
             .parsed.instance_section->instances[0].expression.with_args.args) {
        return false;
    }
    memset(outer_section->parsed.component->sections[2]
               .parsed.instance_section->instances[0].expression.with_args.args,
           0, sizeof(WASMComponentInstArg));
    outer_section->parsed.component->sections[2]
        .parsed.instance_section->instances[0].expression.with_args.args[0]
        .name = clone_core_name("source");
    outer_section->parsed.component->sections[2]
        .parsed.instance_section->instances[0].expression.with_args.args[0]
        .idx.sort_idx = create_sort_idx(WASM_COMP_SORT_COMPONENT, 0);
    if (!outer_section->parsed.component->sections[2]
             .parsed.instance_section->instances[0].expression.with_args.args[0]
             .name
        || !outer_section->parsed.component->sections[2]
                 .parsed.instance_section->instances[0].expression.with_args.args[0]
                 .idx.sort_idx) {
        return false;
    }

    outer_section->parsed.component->sections[3].id = WASM_COMP_SECTION_EXPORTS;
    outer_section->parsed.component->sections[3].parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!outer_section->parsed.component->sections[3].parsed.export_section) {
        return false;
    }
    memset(outer_section->parsed.component->sections[3].parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    outer_section->parsed.component->sections[3].parsed.export_section->count = 1;
    outer_section->parsed.component->sections[3].parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!outer_section->parsed.component->sections[3]
             .parsed.export_section->exports) {
        return false;
    }
    memset(outer_section->parsed.component->sections[3]
               .parsed.export_section->exports,
           0, sizeof(WASMComponentExport));
    outer_section->parsed.component->sections[3]
        .parsed.export_section->exports[0].export_name =
        create_export_name("nested-component-import-instance");
    outer_section->parsed.component->sections[3]
        .parsed.export_section->exports[0].sort_idx =
        create_sort_idx(WASM_COMP_SORT_INSTANCE, 0);
    if (!outer_section->parsed.component->sections[3]
             .parsed.export_section->exports[0].export_name
        || !outer_section->parsed.component->sections[3]
                 .parsed.export_section->exports[0].sort_idx) {
        return false;
    }

    instance_section->id = WASM_COMP_SECTION_INSTANCES;
    instance_section->parsed.instance_section =
        (WASMComponentInstSection *)wasm_runtime_malloc(
            sizeof(WASMComponentInstSection));
    if (!instance_section->parsed.instance_section) {
        return false;
    }
    memset(instance_section->parsed.instance_section, 0,
           sizeof(WASMComponentInstSection));
    instance_section->parsed.instance_section->count = 1;
    instance_section->parsed.instance_section->instances =
        (WASMComponentInst *)wasm_runtime_malloc(sizeof(WASMComponentInst));
    if (!instance_section->parsed.instance_section->instances) {
        return false;
    }
    memset(instance_section->parsed.instance_section->instances, 0,
           sizeof(WASMComponentInst));
    instance_section->parsed.instance_section->instances[0].instance_expression_tag =
        WASM_COMP_INSTANCE_EXPRESSION_WITH_ARGS;
    instance_section->parsed.instance_section->instances[0].expression.with_args.idx =
        1;

    export_section->id = WASM_COMP_SECTION_EXPORTS;
    export_section->parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!export_section->parsed.export_section) {
        return false;
    }
    memset(export_section->parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    export_section->parsed.export_section->count = 1;
    export_section->parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!export_section->parsed.export_section->exports) {
        return false;
    }
    memset(export_section->parsed.export_section->exports, 0,
           sizeof(WASMComponentExport));
    export_section->parsed.export_section->exports[0].export_name =
        create_export_name("top-level-nested-component-import-instance");
    export_section->parsed.export_section->exports[0].sort_idx =
        create_sort_idx(WASM_COMP_SORT_INSTANCE, 1);
    return export_section->parsed.export_section->exports[0].export_name
           && export_section->parsed.export_section->exports[0].sort_idx;
}

static bool
append_nested_value_import_alias_sections(WASMComponentModule *component_module)
{
    WASMComponent *component = &component_module->component;
    const uint32_t component_idx =
        count_top_level_sort_entries(component, WASM_COMP_SORT_COMPONENT);
    const uint32_t instance_idx =
        count_top_level_sort_entries(component, WASM_COMP_SORT_INSTANCE);
    const uint32_t old_count = component->section_count;
    const uint32_t new_count = old_count + 3;
    auto *new_sections = (WASMComponentSection *)wasm_runtime_malloc(
        sizeof(WASMComponentSection) * new_count);
    if (!new_sections) {
        return false;
    }

    memset(new_sections, 0, sizeof(WASMComponentSection) * new_count);
    memcpy(new_sections, component->sections,
           sizeof(WASMComponentSection) * old_count);
    wasm_runtime_free(component->sections);
    component->sections = new_sections;
    component->section_count = new_count;

    WASMComponentSection *component_section = &component->sections[old_count];
    WASMComponentSection *instance_section = &component->sections[old_count + 1];
    WASMComponentSection *export_section = &component->sections[old_count + 2];

    component_section->id = WASM_COMP_SECTION_COMPONENT;
    component_section->parsed.component = create_empty_component();
    if (!component_section->parsed.component) {
        return false;
    }
    component_section->parsed.component->section_count = 6;
    component_section->parsed.component->sections =
        (WASMComponentSection *)wasm_runtime_malloc(sizeof(WASMComponentSection) * 6);
    if (!component_section->parsed.component->sections) {
        return false;
    }
    memset(component_section->parsed.component->sections, 0,
           sizeof(WASMComponentSection) * 6);

    component_section->parsed.component->sections[0].id = WASM_COMP_SECTION_COMPONENT;
    component_section->parsed.component->sections[0].parsed.component =
        create_empty_component();
    if (!component_section->parsed.component->sections[0].parsed.component) {
        return false;
    }
    component_section->parsed.component->sections[0].parsed.component->section_count = 2;
    component_section->parsed.component->sections[0].parsed.component->sections =
        (WASMComponentSection *)wasm_runtime_malloc(sizeof(WASMComponentSection) * 2);
    if (!component_section->parsed.component->sections[0]
             .parsed.component->sections) {
        return false;
    }
    memset(component_section->parsed.component->sections[0].parsed.component->sections,
           0, sizeof(WASMComponentSection) * 2);

    component_section->parsed.component->sections[0]
        .parsed.component->sections[0].id = WASM_COMP_SECTION_IMPORTS;
    component_section->parsed.component->sections[0]
        .parsed.component->sections[0].parsed.import_section =
        (WASMComponentImportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentImportSection));
    if (!component_section->parsed.component->sections[0]
             .parsed.component->sections[0].parsed.import_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[0]
               .parsed.component->sections[0].parsed.import_section,
           0, sizeof(WASMComponentImportSection));
    component_section->parsed.component->sections[0]
        .parsed.component->sections[0].parsed.import_section->count = 1;
    component_section->parsed.component->sections[0]
        .parsed.component->sections[0].parsed.import_section->imports =
        (WASMComponentImport *)wasm_runtime_malloc(sizeof(WASMComponentImport));
    if (!component_section->parsed.component->sections[0]
             .parsed.component->sections[0].parsed.import_section->imports) {
        return false;
    }
    memset(component_section->parsed.component->sections[0]
               .parsed.component->sections[0].parsed.import_section->imports,
           0, sizeof(WASMComponentImport));
    component_section->parsed.component->sections[0]
        .parsed.component->sections[0].parsed.import_section->imports[0]
        .import_name = create_import_name("source");
    component_section->parsed.component->sections[0]
        .parsed.component->sections[0].parsed.import_section->imports[0]
        .extern_desc = create_extern_desc(WASM_COMP_EXTERN_VALUE);
    if (!component_section->parsed.component->sections[0]
             .parsed.component->sections[0].parsed.import_section->imports[0]
             .import_name
        || !component_section->parsed.component->sections[0]
                 .parsed.component->sections[0].parsed.import_section->imports[0]
                 .extern_desc) {
        return false;
    }

    component_section->parsed.component->sections[0]
        .parsed.component->sections[1].id = WASM_COMP_SECTION_EXPORTS;
    component_section->parsed.component->sections[0]
        .parsed.component->sections[1].parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!component_section->parsed.component->sections[0]
             .parsed.component->sections[1].parsed.export_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[0]
               .parsed.component->sections[1].parsed.export_section,
           0, sizeof(WASMComponentExportSection));
    component_section->parsed.component->sections[0]
        .parsed.component->sections[1].parsed.export_section->count = 1;
    component_section->parsed.component->sections[0]
        .parsed.component->sections[1].parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!component_section->parsed.component->sections[0]
             .parsed.component->sections[1].parsed.export_section->exports) {
        return false;
    }
    memset(component_section->parsed.component->sections[0]
               .parsed.component->sections[1].parsed.export_section->exports,
           0, sizeof(WASMComponentExport));
    component_section->parsed.component->sections[0]
        .parsed.component->sections[1].parsed.export_section->exports[0]
        .export_name = create_export_name("forwarded-value");
    component_section->parsed.component->sections[0]
        .parsed.component->sections[1].parsed.export_section->exports[0]
        .sort_idx = create_sort_idx(WASM_COMP_SORT_VALUE, 0);
    if (!component_section->parsed.component->sections[0]
             .parsed.component->sections[1].parsed.export_section->exports[0]
             .export_name
        || !component_section->parsed.component->sections[0]
                 .parsed.component->sections[1].parsed.export_section->exports[0]
                 .sort_idx) {
        return false;
    }

    component_section->parsed.component->sections[1].id = WASM_COMP_SECTION_VALUES;
    component_section->parsed.component->sections[1].parsed.value_section =
        (WASMComponentValueSection *)wasm_runtime_malloc(
            sizeof(WASMComponentValueSection));
    if (!component_section->parsed.component->sections[1].parsed.value_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[1].parsed.value_section, 0,
           sizeof(WASMComponentValueSection));
    if (!init_scalar_value_section(
            component_section->parsed.component->sections[1].parsed.value_section)) {
        return false;
    }

    component_section->parsed.component->sections[2].id = WASM_COMP_SECTION_INSTANCES;
    component_section->parsed.component->sections[2].parsed.instance_section =
        (WASMComponentInstSection *)wasm_runtime_malloc(
            sizeof(WASMComponentInstSection));
    if (!component_section->parsed.component->sections[2].parsed.instance_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[2].parsed.instance_section, 0,
           sizeof(WASMComponentInstSection));
    component_section->parsed.component->sections[2].parsed.instance_section->count = 1;
    component_section->parsed.component->sections[2].parsed.instance_section->instances =
        (WASMComponentInst *)wasm_runtime_malloc(sizeof(WASMComponentInst));
    if (!component_section->parsed.component->sections[2]
             .parsed.instance_section->instances) {
        return false;
    }
    memset(component_section->parsed.component->sections[2]
               .parsed.instance_section->instances,
           0, sizeof(WASMComponentInst));
    component_section->parsed.component->sections[2]
        .parsed.instance_section->instances[0].instance_expression_tag =
        WASM_COMP_INSTANCE_EXPRESSION_WITH_ARGS;
    component_section->parsed.component->sections[2]
        .parsed.instance_section->instances[0].expression.with_args.idx = 0;
    component_section->parsed.component->sections[2]
        .parsed.instance_section->instances[0].expression.with_args.arg_len = 1;
    component_section->parsed.component->sections[2]
        .parsed.instance_section->instances[0].expression.with_args.args =
        (WASMComponentInstArg *)wasm_runtime_malloc(sizeof(WASMComponentInstArg));
    if (!component_section->parsed.component->sections[2]
             .parsed.instance_section->instances[0].expression.with_args.args) {
        return false;
    }
    memset(component_section->parsed.component->sections[2]
               .parsed.instance_section->instances[0].expression.with_args.args,
           0, sizeof(WASMComponentInstArg));
    component_section->parsed.component->sections[2]
        .parsed.instance_section->instances[0].expression.with_args.args[0]
        .name = clone_core_name("source");
    component_section->parsed.component->sections[2]
        .parsed.instance_section->instances[0].expression.with_args.args[0]
        .idx.sort_idx = create_sort_idx(WASM_COMP_SORT_VALUE, 0);
    if (!component_section->parsed.component->sections[2]
             .parsed.instance_section->instances[0].expression.with_args.args[0]
             .name
        || !component_section->parsed.component->sections[2]
                 .parsed.instance_section->instances[0].expression.with_args.args[0]
                 .idx.sort_idx) {
        return false;
    }

    component_section->parsed.component->sections[3].id = WASM_COMP_SECTION_ALIASES;
    component_section->parsed.component->sections[3].parsed.alias_section =
        (WASMComponentAliasSection *)wasm_runtime_malloc(
            sizeof(WASMComponentAliasSection));
    if (!component_section->parsed.component->sections[3].parsed.alias_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[3].parsed.alias_section, 0,
           sizeof(WASMComponentAliasSection));
    component_section->parsed.component->sections[3].parsed.alias_section->count = 1;
    component_section->parsed.component->sections[3].parsed.alias_section->aliases =
        (WASMComponentAliasDefinition *)wasm_runtime_malloc(
            sizeof(WASMComponentAliasDefinition));
    if (!component_section->parsed.component->sections[3]
             .parsed.alias_section->aliases) {
        return false;
    }
    memset(component_section->parsed.component->sections[3]
               .parsed.alias_section->aliases,
           0, sizeof(WASMComponentAliasDefinition));
    component_section->parsed.component->sections[3]
        .parsed.alias_section->aliases[0].sort = create_sort(WASM_COMP_SORT_VALUE);
    component_section->parsed.component->sections[3]
        .parsed.alias_section->aliases[0].alias_target_type =
        WASM_COMP_ALIAS_TARGET_EXPORT;
    component_section->parsed.component->sections[3]
        .parsed.alias_section->aliases[0].target.exported.instance_idx = 0;
    component_section->parsed.component->sections[3]
        .parsed.alias_section->aliases[0].target.exported.name =
        clone_core_name("forwarded-value");
    if (!component_section->parsed.component->sections[3]
             .parsed.alias_section->aliases[0].sort
        || !component_section->parsed.component->sections[3]
                 .parsed.alias_section->aliases[0].target.exported.name) {
        return false;
    }

    component_section->parsed.component->sections[4].id = WASM_COMP_SECTION_INSTANCES;
    component_section->parsed.component->sections[4].parsed.instance_section =
        (WASMComponentInstSection *)wasm_runtime_malloc(
            sizeof(WASMComponentInstSection));
    if (!component_section->parsed.component->sections[4].parsed.instance_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[4].parsed.instance_section, 0,
           sizeof(WASMComponentInstSection));
    component_section->parsed.component->sections[4].parsed.instance_section->count = 1;
    component_section->parsed.component->sections[4].parsed.instance_section->instances =
        (WASMComponentInst *)wasm_runtime_malloc(sizeof(WASMComponentInst));
    if (!component_section->parsed.component->sections[4]
             .parsed.instance_section->instances) {
        return false;
    }
    memset(component_section->parsed.component->sections[4]
               .parsed.instance_section->instances,
           0, sizeof(WASMComponentInst));
    component_section->parsed.component->sections[4]
        .parsed.instance_section->instances[0].instance_expression_tag =
        WASM_COMP_INSTANCE_EXPRESSION_WITHOUT_ARGS;
    component_section->parsed.component->sections[4]
        .parsed.instance_section->instances[0].expression.without_args.inline_expr_len =
        1;
    component_section->parsed.component->sections[4]
        .parsed.instance_section->instances[0].expression.without_args.inline_expr =
        (WASMComponentInlineExport *)wasm_runtime_malloc(
            sizeof(WASMComponentInlineExport));
    if (!component_section->parsed.component->sections[4]
             .parsed.instance_section->instances[0]
             .expression.without_args.inline_expr) {
        return false;
    }
    memset(component_section->parsed.component->sections[4]
               .parsed.instance_section->instances[0]
               .expression.without_args.inline_expr,
           0, sizeof(WASMComponentInlineExport));
    component_section->parsed.component->sections[4]
        .parsed.instance_section->instances[0]
        .expression.without_args.inline_expr[0].name = clone_core_name("wrapped");
    component_section->parsed.component->sections[4]
        .parsed.instance_section->instances[0]
        .expression.without_args.inline_expr[0].sort_idx =
        create_sort_idx(WASM_COMP_SORT_VALUE, 1);
    if (!component_section->parsed.component->sections[4]
             .parsed.instance_section->instances[0]
             .expression.without_args.inline_expr[0].name
        || !component_section->parsed.component->sections[4]
                 .parsed.instance_section->instances[0]
                 .expression.without_args.inline_expr[0].sort_idx) {
        return false;
    }

    component_section->parsed.component->sections[5].id = WASM_COMP_SECTION_EXPORTS;
    component_section->parsed.component->sections[5].parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!component_section->parsed.component->sections[5].parsed.export_section) {
        return false;
    }
    memset(component_section->parsed.component->sections[5].parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    component_section->parsed.component->sections[5].parsed.export_section->count = 1;
    component_section->parsed.component->sections[5].parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!component_section->parsed.component->sections[5]
             .parsed.export_section->exports) {
        return false;
    }
    memset(component_section->parsed.component->sections[5]
               .parsed.export_section->exports,
           0, sizeof(WASMComponentExport));
    component_section->parsed.component->sections[5]
        .parsed.export_section->exports[0].export_name =
        create_export_name("value-alias-instance");
    component_section->parsed.component->sections[5]
        .parsed.export_section->exports[0].sort_idx =
        create_sort_idx(WASM_COMP_SORT_INSTANCE, 1);
    if (!component_section->parsed.component->sections[5]
             .parsed.export_section->exports[0].export_name
        || !component_section->parsed.component->sections[5]
                 .parsed.export_section->exports[0].sort_idx) {
        return false;
    }

    instance_section->id = WASM_COMP_SECTION_INSTANCES;
    instance_section->parsed.instance_section =
        (WASMComponentInstSection *)wasm_runtime_malloc(
            sizeof(WASMComponentInstSection));
    if (!instance_section->parsed.instance_section) {
        return false;
    }
    memset(instance_section->parsed.instance_section, 0,
           sizeof(WASMComponentInstSection));
    instance_section->parsed.instance_section->count = 1;
    instance_section->parsed.instance_section->instances =
        (WASMComponentInst *)wasm_runtime_malloc(sizeof(WASMComponentInst));
    if (!instance_section->parsed.instance_section->instances) {
        return false;
    }
    memset(instance_section->parsed.instance_section->instances, 0,
           sizeof(WASMComponentInst));
    instance_section->parsed.instance_section->instances[0].instance_expression_tag =
        WASM_COMP_INSTANCE_EXPRESSION_WITH_ARGS;
    instance_section->parsed.instance_section->instances[0].expression.with_args.idx =
        component_idx;

    export_section->id = WASM_COMP_SECTION_EXPORTS;
    export_section->parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!export_section->parsed.export_section) {
        return false;
    }
    memset(export_section->parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    export_section->parsed.export_section->count = 1;
    export_section->parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!export_section->parsed.export_section->exports) {
        return false;
    }
    memset(export_section->parsed.export_section->exports, 0,
           sizeof(WASMComponentExport));
    export_section->parsed.export_section->exports[0].export_name =
        create_export_name("nested-value-alias-instance");
    export_section->parsed.export_section->exports[0].sort_idx =
        create_sort_idx(WASM_COMP_SORT_INSTANCE, instance_idx);
    return export_section->parsed.export_section->exports[0].export_name
           && export_section->parsed.export_section->exports[0].sort_idx;
}

static bool
append_nested_core_module_import_sections(WASMComponentModule *component_module)
{
    WASMComponent *component = &component_module->component;
    const uint32_t old_count = component->section_count;
    const uint32_t new_count = old_count + 3;
    auto *new_sections = (WASMComponentSection *)wasm_runtime_malloc(
        sizeof(WASMComponentSection) * new_count);
    if (!new_sections) {
        return false;
    }

    memset(new_sections, 0, sizeof(WASMComponentSection) * new_count);
    memcpy(new_sections, component->sections,
           sizeof(WASMComponentSection) * old_count);
    wasm_runtime_free(component->sections);
    component->sections = new_sections;
    component->section_count = new_count;

    WASMComponentSection *outer_section = &component->sections[old_count];
    WASMComponentSection *instance_section = &component->sections[old_count + 1];
    WASMComponentSection *export_section = &component->sections[old_count + 2];

    outer_section->id = WASM_COMP_SECTION_COMPONENT;
    outer_section->parsed.component = create_empty_component();
    if (!outer_section->parsed.component) {
        return false;
    }
    outer_section->parsed.component->section_count = 2;
    outer_section->parsed.component->sections =
        (WASMComponentSection *)wasm_runtime_malloc(sizeof(WASMComponentSection) * 2);
    if (!outer_section->parsed.component->sections) {
        return false;
    }
    memset(outer_section->parsed.component->sections, 0,
           sizeof(WASMComponentSection) * 2);

    outer_section->parsed.component->sections[0].id = WASM_COMP_SECTION_IMPORTS;
    outer_section->parsed.component->sections[0].parsed.import_section =
        (WASMComponentImportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentImportSection));
    if (!outer_section->parsed.component->sections[0].parsed.import_section) {
        return false;
    }
    memset(outer_section->parsed.component->sections[0].parsed.import_section, 0,
           sizeof(WASMComponentImportSection));
    outer_section->parsed.component->sections[0].parsed.import_section->count = 1;
    outer_section->parsed.component->sections[0].parsed.import_section->imports =
        (WASMComponentImport *)wasm_runtime_malloc(sizeof(WASMComponentImport));
    if (!outer_section->parsed.component->sections[0]
             .parsed.import_section->imports) {
        return false;
    }
    memset(outer_section->parsed.component->sections[0]
               .parsed.import_section->imports,
           0, sizeof(WASMComponentImport));
    outer_section->parsed.component->sections[0]
        .parsed.import_section->imports[0].import_name =
        create_import_name("source");
    outer_section->parsed.component->sections[0]
        .parsed.import_section->imports[0].extern_desc =
        create_extern_desc(WASM_COMP_EXTERN_CORE_MODULE);
    if (!outer_section->parsed.component->sections[0]
             .parsed.import_section->imports[0].import_name
        || !outer_section->parsed.component->sections[0]
                 .parsed.import_section->imports[0].extern_desc) {
        return false;
    }

    outer_section->parsed.component->sections[1].id = WASM_COMP_SECTION_EXPORTS;
    outer_section->parsed.component->sections[1].parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!outer_section->parsed.component->sections[1].parsed.export_section) {
        return false;
    }
    memset(outer_section->parsed.component->sections[1].parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    outer_section->parsed.component->sections[1].parsed.export_section->count = 1;
    outer_section->parsed.component->sections[1].parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!outer_section->parsed.component->sections[1]
             .parsed.export_section->exports) {
        return false;
    }
    memset(outer_section->parsed.component->sections[1]
               .parsed.export_section->exports,
           0, sizeof(WASMComponentExport));
    outer_section->parsed.component->sections[1]
        .parsed.export_section->exports[0].export_name =
        create_export_name("forwarded-core-module");
    outer_section->parsed.component->sections[1]
        .parsed.export_section->exports[0].sort_idx =
        create_core_sort_idx(WASM_COMP_CORE_SORT_MODULE, 0);
    if (!outer_section->parsed.component->sections[1]
             .parsed.export_section->exports[0].export_name
        || !outer_section->parsed.component->sections[1]
                 .parsed.export_section->exports[0].sort_idx) {
        return false;
    }

    instance_section->id = WASM_COMP_SECTION_INSTANCES;
    instance_section->parsed.instance_section =
        (WASMComponentInstSection *)wasm_runtime_malloc(
            sizeof(WASMComponentInstSection));
    if (!instance_section->parsed.instance_section) {
        return false;
    }
    memset(instance_section->parsed.instance_section, 0,
           sizeof(WASMComponentInstSection));
    instance_section->parsed.instance_section->count = 1;
    instance_section->parsed.instance_section->instances =
        (WASMComponentInst *)wasm_runtime_malloc(sizeof(WASMComponentInst));
    if (!instance_section->parsed.instance_section->instances) {
        return false;
    }
    memset(instance_section->parsed.instance_section->instances, 0,
           sizeof(WASMComponentInst));
    instance_section->parsed.instance_section->instances[0]
        .instance_expression_tag = WASM_COMP_INSTANCE_EXPRESSION_WITH_ARGS;
    instance_section->parsed.instance_section->instances[0].expression.with_args.idx =
        1;
    instance_section->parsed.instance_section->instances[0]
        .expression.with_args.arg_len = 1;
    instance_section->parsed.instance_section->instances[0]
        .expression.with_args.args =
        (WASMComponentInstArg *)wasm_runtime_malloc(sizeof(WASMComponentInstArg));
    if (!instance_section->parsed.instance_section->instances[0]
             .expression.with_args.args) {
        return false;
    }
    memset(instance_section->parsed.instance_section->instances[0]
               .expression.with_args.args,
           0, sizeof(WASMComponentInstArg));
    instance_section->parsed.instance_section->instances[0]
        .expression.with_args.args[0].name = clone_core_name("source");
    instance_section->parsed.instance_section->instances[0]
        .expression.with_args.args[0].idx.sort_idx =
        create_core_sort_idx(WASM_COMP_CORE_SORT_MODULE, 0);
    if (!instance_section->parsed.instance_section->instances[0]
             .expression.with_args.args[0].name
        || !instance_section->parsed.instance_section->instances[0]
                 .expression.with_args.args[0].idx.sort_idx) {
        return false;
    }

    export_section->id = WASM_COMP_SECTION_EXPORTS;
    export_section->parsed.export_section =
        (WASMComponentExportSection *)wasm_runtime_malloc(
            sizeof(WASMComponentExportSection));
    if (!export_section->parsed.export_section) {
        return false;
    }
    memset(export_section->parsed.export_section, 0,
           sizeof(WASMComponentExportSection));
    export_section->parsed.export_section->count = 1;
    export_section->parsed.export_section->exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    if (!export_section->parsed.export_section->exports) {
        return false;
    }
    memset(export_section->parsed.export_section->exports, 0,
           sizeof(WASMComponentExport));
    export_section->parsed.export_section->exports[0].export_name =
        create_export_name("nested-core-module-import-instance");
    export_section->parsed.export_section->exports[0].sort_idx =
        create_sort_idx(WASM_COMP_SORT_INSTANCE, 1);
    return export_section->parsed.export_section->exports[0].export_name
           && export_section->parsed.export_section->exports[0].sort_idx;
}

class BinaryParserTest : public testing::Test
{
  public:
    std::unique_ptr<ComponentHelper> helper;
    BinaryParserTest() {}
    ~BinaryParserTest() {}
  
    virtual void SetUp() {
        helper = std::make_unique<ComponentHelper>();
        helper->do_setup();
    }

    virtual void TearDown() {
        helper->do_teardown();
        helper = nullptr;
    }
};

TEST_F(BinaryParserTest, TestAllComponentsLoadAndUnload)
{
    // Load and unload every listed component
    for (const std::string &name : component_files) {
        helper->reset_component();
        std::string path = name;
        printf("LoadAndUnloadComponent: %s\n", path.c_str());
        bool ret = helper->read_wasm_file(path.c_str());
        ASSERT_TRUE(ret);
        ret = helper->load_component();
        ASSERT_TRUE(ret);
        ASSERT_TRUE(helper->is_loaded());

        // Unload component and free raw buffer to simulate full unload
        helper->reset_component();
        if (helper->component_raw) {
            BH_FREE(helper->component_raw);
            helper->component_raw = NULL;
        }
        ASSERT_FALSE(helper->is_loaded());
        ASSERT_EQ(helper->get_section_count(), 0u);
    }
}

TEST_F(BinaryParserTest, TestLoadCorruptComponent)
{
    // Corrupt header and expect load failure
    helper->reset_component();
    bool ret = helper->read_wasm_file((std::string("add.wasm").c_str()));
    ASSERT_TRUE(ret);
    ASSERT_TRUE(helper->component_raw != NULL);
    helper->component_raw[0] ^= 0xFF; // corrupt
    ret = helper->load_component();
    ASSERT_FALSE(ret);
}

TEST_F(BinaryParserTest, TestDecodeHeaderValid)
{
    helper->reset_component();
    bool ret = helper->read_wasm_file("logging_service.component.wasm");
    ASSERT_TRUE(ret);
    ASSERT_TRUE(helper->component_raw != NULL);

    WASMHeader header;
    bool ok = wasm_decode_header(helper->component_raw,
                                           helper->wasm_file_size,
                                           &header);
    ASSERT_TRUE(ok);
    ASSERT_EQ(header.magic, WASM_MAGIC_NUMBER);
    ASSERT_EQ(header.version, WASM_COMPONENT_VERSION);
    ASSERT_EQ(header.layer, WASM_COMPONENT_LAYER);
}

TEST_F(BinaryParserTest, TestDecodeHeaderInvalid)
{
    helper->reset_component();
    bool ret = helper->read_wasm_file("logging_service.component.wasm");
    ASSERT_TRUE(ret);
    ASSERT_TRUE(helper->component_raw != NULL);

    std::vector<uint8_t> corrupted(helper->component_raw,
                                   helper->component_raw + helper->wasm_file_size);
    corrupted[0] ^= 0xFF; // corrupt magic byte

    WASMHeader header;
    ret = wasm_decode_header(corrupted.data(),
                                           (uint32_t)corrupted.size(),
                                           &header);
    ASSERT_TRUE(ret);
    ASSERT_FALSE(is_wasm_component(header));
}

TEST_F(BinaryParserTest, TestSectionAliasIndividual)
{
    helper->reset_component();
    bool ret = helper->read_wasm_file((std::string("add.wasm").c_str()));
    ASSERT_TRUE(ret);
    ASSERT_TRUE(helper->component_raw != NULL);
    ret = helper->load_component();
    ASSERT_TRUE(ret);
    ASSERT_TRUE(helper->is_loaded());

    std::string check_against = "test:project/my-interface@0.1.0#add";

    auto sections = helper->get_section(WASM_COMP_SECTION_ALIASES);
    bool found = false;
    for (auto section: sections) {
        ASSERT_EQ(section->id, WASM_COMP_SECTION_ALIASES);

        WASMComponentAliasSection *alias_section = section->parsed.alias_section;
        for (uint32_t id = 0; id < alias_section->count; id++) {
            WASMComponentAliasDefinition* alias_def = &alias_section->aliases[id];

            if (alias_def->alias_target_type == WASM_COMP_ALIAS_TARGET_CORE_EXPORT) {
                if (std::string{alias_def->target.core_exported.name->name} == check_against) {
                    found = true;
                }
            }
        }
    }

    ASSERT_TRUE(found);
}

TEST_F(BinaryParserTest, TestRuntimeLoadComponent)
{
    bool ret = helper->read_wasm_file("logging_service.component.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "runtime-load-component";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_EQ(wasm_runtime_get_module_package_type(module),
              Wasm_Module_Component);

    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestRuntimeInstantiateAndDeinstantiateComponent)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "runtime-instantiate-component";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;

    wasm_module_inst_t module_inst =
        instantiate_component_with_default_wasi(module, helper.get());
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;
    ASSERT_EQ(wasm_runtime_get_module(module_inst), module);

    wasm_runtime_deinstantiate(module_inst);

    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestRuntimeLookupComponentFunctionNotSupportedYet)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "runtime-lookup-component";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;

    wasm_module_inst_t module_inst =
        instantiate_component_with_default_wasi(module, helper.get());
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    wasm_function_inst_t func = wasm_runtime_lookup_function(module_inst, "main");
    ASSERT_EQ(func, nullptr);
    ASSERT_STREQ(wasm_runtime_get_exception(module_inst),
                 "component function lookup is not supported yet");

    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestPublicComponentExportDiscoveryAndLookup)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "public-component-export-discovery";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_TRUE(append_component_export_alias_sections(
        (WASMComponentModule *)module));

    wasm_module_inst_t module_inst =
        instantiate_component_with_default_wasi(module, helper.get());
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    const int32_t export_count =
        wasm_runtime_get_component_export_count(module_inst);
    ASSERT_EQ(export_count, 3);

    wasm_component_export_t export_type = {};
    ASSERT_TRUE(
        wasm_runtime_get_component_export_type(module_inst, 1, &export_type));
    ASSERT_EQ(std::string(export_type.name), "aliased-add");
    ASSERT_EQ(export_type.kind, WASM_COMPONENT_EXTERN_KIND_FUNC);
    ASSERT_EQ(export_type.value.function,
              wasm_runtime_lookup_component_function(module_inst, "aliased-add"));

    ASSERT_TRUE(
        wasm_runtime_get_component_export_type(module_inst, 2, &export_type));
    ASSERT_EQ(std::string(export_type.name), "aliased-instance");
    ASSERT_EQ(export_type.kind, WASM_COMPONENT_EXTERN_KIND_INSTANCE);
    ASSERT_EQ(export_type.value.instance,
              wasm_runtime_lookup_component_instance(module_inst,
                                                    "aliased-instance"));
    ASSERT_EQ(wasm_component_instance_get_export_count(export_type.value.instance),
              1);

    wasm_component_export_t nested_export = {};
    ASSERT_TRUE(wasm_component_instance_get_export_type(export_type.value.instance,
                                                        0, &nested_export));
    ASSERT_EQ(std::string(nested_export.name), "add");
    ASSERT_EQ(nested_export.kind, WASM_COMPONENT_EXTERN_KIND_FUNC);

    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestPublicComponentCallInvokesScalarCanonLift)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "public-component-call";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_TRUE(append_component_export_alias_sections(
        (WASMComponentModule *)module));

    wasm_module_inst_t module_inst =
        instantiate_component_with_default_wasi(module, helper.get());
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    wasm_component_func_t func =
        wasm_runtime_lookup_component_function(module_inst, "aliased-add");
    ASSERT_NE(func, nullptr);

    wasm_val_t args[2] = {};
    args[0].kind = WASM_I32;
    args[0].of.i32 = 7;
    args[1].kind = WASM_I32;
    args[1].of.i32 = 35;
    wasm_val_t results[1] = {};

    ASSERT_TRUE(
        wasm_runtime_call_component(module_inst, func, 1, results, 2, args))
        << wasm_runtime_get_exception(module_inst);
    ASSERT_EQ(results[0].kind, WASM_I32);
    ASSERT_EQ(results[0].of.i32, 42);
    ASSERT_EQ(wasm_runtime_get_exception(module_inst), nullptr);

    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestPublicComponentValueExportLookupReturnsOwnedCopy)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "public-component-value-export";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_TRUE(append_public_top_level_value_export_sections(
        (WASMComponentModule *)module));

    wasm_module_inst_t module_inst =
        instantiate_component_with_default_wasi(module, helper.get());
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    auto *component_inst = (WASMComponentInstance *)module_inst;
    const void *internal_data = wasm_component_runtime_value_get_data(
        &component_inst->component_values[0]);
    wasm_component_export_t export_type = {};
    wasm_component_value_t lookup_value = {};
    wasm_component_value_t indexed_value = {};

    ASSERT_EQ(wasm_runtime_get_component_export_count(module_inst), 2);
    ASSERT_TRUE(
        wasm_runtime_get_component_export_type(module_inst, 1, &export_type));
    ASSERT_EQ(std::string(export_type.name), "exported-value");
    ASSERT_EQ(export_type.kind, WASM_COMPONENT_EXTERN_KIND_VALUE);

    ASSERT_TRUE(wasm_runtime_lookup_component_value(module_inst, "exported-value",
                                                    &lookup_value));
    ASSERT_EQ(lookup_value.type.kind, WASM_COMPONENT_VALUE_TYPE_PRIMITIVE);
    ASSERT_EQ(lookup_value.type.type.primitive_type,
              WASM_COMPONENT_PRIMITIVE_VALUE_S32);
    ASSERT_EQ(lookup_value.storage_kind, WASM_COMPONENT_VALUE_STORAGE_INLINE);
    ASSERT_EQ(lookup_value.byte_size, sizeof(runtime_value_section_i32_bytes));
    ASSERT_NE(wasm_component_value_get_data(&lookup_value), internal_data);
    ASSERT_EQ(memcmp(wasm_component_value_get_data(&lookup_value),
                     runtime_value_section_i32_bytes,
                     sizeof(runtime_value_section_i32_bytes)),
              0);

    ASSERT_TRUE(
        wasm_runtime_get_component_export_value(module_inst, 1, &indexed_value));
    ASSERT_EQ(indexed_value.type.kind, WASM_COMPONENT_VALUE_TYPE_PRIMITIVE);
    ASSERT_EQ(indexed_value.type.type.primitive_type,
              WASM_COMPONENT_PRIMITIVE_VALUE_S32);
    ASSERT_EQ(indexed_value.storage_kind, WASM_COMPONENT_VALUE_STORAGE_INLINE);
    ASSERT_NE(wasm_component_value_get_data(&indexed_value), internal_data);
    ASSERT_EQ(memcmp(wasm_component_value_get_data(&indexed_value),
                     runtime_value_section_i32_bytes,
                     sizeof(runtime_value_section_i32_bytes)),
              0);

    wasm_runtime_deinstantiate(module_inst);

    ASSERT_EQ(memcmp(wasm_component_value_get_data(&lookup_value),
                     runtime_value_section_i32_bytes,
                     sizeof(runtime_value_section_i32_bytes)),
              0);
    ASSERT_EQ(memcmp(wasm_component_value_get_data(&indexed_value),
                     runtime_value_section_i32_bytes,
                     sizeof(runtime_value_section_i32_bytes)),
              0);

    wasm_component_value_destroy(&lookup_value);
    wasm_component_value_destroy(&indexed_value);
    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestPublicComponentCallRejectsNestedCanonLiftHandle)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "public-component-call-nested";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;

    wasm_module_inst_t module_inst =
        instantiate_component_with_default_wasi(module, helper.get());
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    wasm_component_instance_t exported_instance =
        wasm_runtime_lookup_component_instance(module_inst,
                                              "test:project/my-interface@0.1.0");
    ASSERT_NE(exported_instance, nullptr);

    wasm_component_func_t func =
        wasm_component_instance_lookup_function(exported_instance, "add");
    ASSERT_NE(func, nullptr);

    wasm_val_t args[2] = {};
    args[0].kind = WASM_I32;
    args[0].of.i32 = 1;
    args[1].kind = WASM_I32;
    args[1].of.i32 = 2;
    wasm_val_t results[1] = {};

    ASSERT_FALSE(
        wasm_runtime_call_component(module_inst, func, 1, results, 2, args));
    ASSERT_STREQ(wasm_runtime_get_exception(module_inst),
                 "component call only supports top-level exported canon lift "
                 "functions");

    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestPublicComponentCallRejectsMemoryBackedCanonLift)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "public-component-call-memory-backed";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_TRUE(
        append_component_export_alias_sections((WASMComponentModule *)module));
    ASSERT_TRUE(configure_first_canon_lift_for_utf8_string(
        (WASMComponentModule *)module, true));

    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate(module, helper->stack_size, helper->heap_size,
                                 helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    wasm_component_func_t func =
        wasm_runtime_lookup_component_function(module_inst, "aliased-add");
    ASSERT_NE(func, nullptr);

    wasm_val_t arg = {};
    wasm_val_t results[1] = {};

    ASSERT_FALSE(
        wasm_runtime_call_component(module_inst, func, 1, results, 1, &arg));
    ASSERT_STREQ(wasm_runtime_get_exception(module_inst),
                 "component canon lift function uses string values; call "
                 "through the component value API");

    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestPublicComponentCallSupportsUtf8StringLift)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "public-component-call-utf8-string";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_TRUE(
        append_component_export_alias_sections((WASMComponentModule *)module));
    ASSERT_TRUE(configure_first_canon_lift_for_utf8_string(
        (WASMComponentModule *)module, true));

    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate(module, helper->stack_size, helper->heap_size,
                                 helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    wasm_component_func_t func =
        wasm_runtime_lookup_component_function(module_inst, "aliased-add");
    ASSERT_NE(func, nullptr);

    uint8_t *memory = func->canon_memory_ref.of.memory->memory_data;
    const char expected_bytes[] = { 0x05, 'h', 'e', 'l', 'l', 'o' };
    uint32_t payload_ptr = 64;
    uint32_t payload_len = 5;
    memcpy(memory, &payload_ptr, sizeof(payload_ptr));
    memcpy(memory + sizeof(payload_ptr), &payload_len, sizeof(payload_len));
    memcpy(memory + payload_ptr, "hello", payload_len);

    wasm_component_value_t args[1] = {
        make_component_string_value("")
    };
    wasm_component_value_t results[1] = {};

    ASSERT_TRUE(
        wasm_runtime_call_component_values(module_inst, func, 1, results, 1, args))
        << wasm_runtime_get_exception(module_inst);
    ASSERT_EQ(results[0].type.kind, WASM_COMPONENT_VALUE_TYPE_PRIMITIVE);
    ASSERT_EQ(results[0].type.type.primitive_type,
              WASM_COMPONENT_PRIMITIVE_VALUE_STRING);
    ASSERT_EQ(results[0].storage_kind, WASM_COMPONENT_VALUE_STORAGE_OWNED);

    const auto *result_bytes =
        (const uint8_t *)wasm_component_value_get_data(&results[0]);
    ASSERT_NE(result_bytes, nullptr);
    ASSERT_EQ(results[0].byte_size, sizeof(expected_bytes));
    ASSERT_EQ(memcmp(result_bytes, expected_bytes, sizeof(expected_bytes)),
              0);

    wasm_component_value_destroy(&results[0]);
    wasm_component_value_destroy(&args[0]);
    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestPublicComponentCallRejectsInvalidUtf8StringArg)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "public-component-call-invalid-utf8";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_TRUE(
        append_component_export_alias_sections((WASMComponentModule *)module));
    ASSERT_TRUE(configure_first_canon_lift_for_utf8_string(
        (WASMComponentModule *)module, true));

    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate(module, helper->stack_size, helper->heap_size,
                                 helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    wasm_component_func_t func =
        wasm_runtime_lookup_component_function(module_inst, "aliased-add");
    ASSERT_NE(func, nullptr);

    uint8_t invalid_bytes[] = { 0x02, 0xC3, 0x28 };
    wasm_component_value_t arg = {};
    arg.type.kind = WASM_COMPONENT_VALUE_TYPE_PRIMITIVE;
    arg.type.type.primitive_type = WASM_COMPONENT_PRIMITIVE_VALUE_STRING;
    arg.storage_kind = WASM_COMPONENT_VALUE_STORAGE_INLINE;
    arg.byte_size = (uint32_t)sizeof(invalid_bytes);
    memcpy(arg.storage.inline_storage, invalid_bytes, sizeof(invalid_bytes));

    wasm_component_value_t result = {};
    ASSERT_FALSE(wasm_runtime_call_component_values(module_inst, func, 1, &result,
                                                    1, &arg));
    ASSERT_STREQ(wasm_runtime_get_exception(module_inst),
                 "component canon lift function parameter 0 does not contain "
                 "valid UTF-8");

    wasm_component_value_destroy(&result);
    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestPublicComponentInstantiationBindsTopLevelImports)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs source_load_args = {};
    char source_module_name[] = "component-import-source";
    source_load_args.name = source_module_name;
    wasm_module_t source_module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &source_load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(source_module, nullptr) << helper->error_buf;
    ASSERT_TRUE(append_top_level_component_export_alias_sections(
        (WASMComponentModule *)source_module));

    wasm_module_inst_t source_inst =
        wasm_runtime_instantiate(source_module, helper->stack_size,
                                 helper->heap_size, helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(source_inst, nullptr) << helper->error_buf;

    wasm_component_component_t imported_component =
        wasm_runtime_lookup_component_component(source_inst,
                                               "aliased-component");
    ASSERT_NE(imported_component, nullptr);

    uint32_t target_wasm_file_size = 0;
    auto *target_component_raw =
        (unsigned char *)bh_read_file_to_buffer("add.wasm", &target_wasm_file_size);
    ASSERT_NE(target_component_raw, nullptr);

    LoadArgs target_load_args = {};
    char target_module_name[] = "component-import-target";
    target_load_args.name = target_module_name;
    wasm_module_t target_module = wasm_runtime_load_ex(
        target_component_raw, target_wasm_file_size, &target_load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(target_module, nullptr) << helper->error_buf;
    ASSERT_TRUE(append_top_level_direct_component_import_sections(
        (WASMComponentModule *)target_module));

    struct InstantiationArgs2 *inst_args = nullptr;
    ASSERT_TRUE(wasm_runtime_instantiation_args_create(&inst_args));
    wasm_runtime_instantiation_args_set_default_stack_size(inst_args,
                                                           helper->stack_size);
    wasm_runtime_instantiation_args_set_host_managed_heap_size(
        inst_args, helper->heap_size);

    wasm_component_import_binding_t import_binding = {};
    import_binding.name = "source";
    import_binding.kind = WASM_COMPONENT_EXTERN_KIND_COMPONENT;
    import_binding.value.component = imported_component;
    wasm_runtime_instantiation_args_set_component_imports(inst_args,
                                                          &import_binding, 1);

    wasm_module_inst_t target_inst =
        wasm_runtime_instantiate_ex2(target_module, inst_args, helper->error_buf,
                                     (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(target_inst, nullptr) << helper->error_buf;

    wasm_component_component_t forwarded_component =
        wasm_runtime_lookup_component_component(target_inst,
                                               "forwarded-source");
    ASSERT_NE(forwarded_component, nullptr);
    ASSERT_EQ(forwarded_component->component, imported_component->component);
    ASSERT_EQ(forwarded_component->scope, imported_component->scope);

    wasm_runtime_instantiation_args_destroy(inst_args);
    wasm_runtime_deinstantiate(target_inst);
    wasm_runtime_unload(target_module);
    BH_FREE(target_component_raw);
    wasm_runtime_deinstantiate(source_inst);
    wasm_runtime_unload(source_module);
}

TEST_F(BinaryParserTest, TestPublicComponentInstantiationBindsTopLevelValueImports)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "public-component-value-import";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_TRUE(append_top_level_public_value_import_sections(
        (WASMComponentModule *)module));

    struct InstantiationArgs2 *inst_args = nullptr;
    ASSERT_TRUE(wasm_runtime_instantiation_args_create(&inst_args));
    wasm_runtime_instantiation_args_set_default_stack_size(inst_args,
                                                           helper->stack_size);
    wasm_runtime_instantiation_args_set_host_managed_heap_size(
        inst_args, helper->heap_size);

    wasm_component_value_t import_value = {};
    import_value.type.kind = WASM_COMPONENT_VALUE_TYPE_PRIMITIVE;
    import_value.type.type.primitive_type = WASM_COMPONENT_PRIMITIVE_VALUE_S32;
    import_value.storage_kind = WASM_COMPONENT_VALUE_STORAGE_OWNED;
    import_value.byte_size = sizeof(runtime_value_section_i32_bytes);
    import_value.storage.owned_data =
        wasm_runtime_malloc(import_value.byte_size);
    ASSERT_NE(import_value.storage.owned_data, nullptr);
    memcpy(import_value.storage.owned_data, runtime_value_section_i32_bytes,
           import_value.byte_size);
    const void *source_data = import_value.storage.owned_data;

    wasm_component_import_binding_t import_binding = {};
    import_binding.name = "source";
    import_binding.kind = WASM_COMPONENT_EXTERN_KIND_VALUE;
    import_binding.value.value = &import_value;
    wasm_runtime_instantiation_args_set_component_imports(inst_args,
                                                          &import_binding, 1);

    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate_ex2(module, inst_args, helper->error_buf,
                                     (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    wasm_component_value_destroy(&import_value);
    wasm_runtime_instantiation_args_destroy(inst_args);

    auto *component_inst = (WASMComponentInstance *)module_inst;
    ASSERT_EQ(component_inst->component_value_count, 1u);
    ASSERT_EQ(component_inst->component_values[0].storage_kind,
              WASM_COMP_RUNTIME_VALUE_STORAGE_INLINE);
    ASSERT_NE(wasm_component_runtime_value_get_data(
                  &component_inst->component_values[0]),
              source_data);
    ASSERT_EQ(memcmp(wasm_component_runtime_value_get_data(
                         &component_inst->component_values[0]),
                     runtime_value_section_i32_bytes,
                     sizeof(runtime_value_section_i32_bytes)),
              0);

    wasm_component_value_t lookup_value = {};
    ASSERT_TRUE(wasm_runtime_lookup_component_value(module_inst, "forwarded-value",
                                                    &lookup_value));
    ASSERT_EQ(lookup_value.type.kind, WASM_COMPONENT_VALUE_TYPE_PRIMITIVE);
    ASSERT_EQ(lookup_value.type.type.primitive_type,
              WASM_COMPONENT_PRIMITIVE_VALUE_S32);
    ASSERT_EQ(memcmp(wasm_component_value_get_data(&lookup_value),
                     runtime_value_section_i32_bytes,
                     sizeof(runtime_value_section_i32_bytes)),
              0);

    wasm_component_value_destroy(&lookup_value);
    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestRuntimeBuildsCoreInstanceGraphAndAliases)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "runtime-component-graph";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;

    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate(module, helper->stack_size, helper->heap_size,
                                 helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    auto *component_inst = (WASMComponentInstance *)module_inst;
    ASSERT_GT(component_inst->core_module_count, 0u);
    ASSERT_GT(component_inst->core_instance_count, 0u);
    ASSERT_GT(component_inst->core_func_count, 0u);
    ASSERT_GT(component_inst->resolved_alias_count, 0u);

    bool found_add_alias = false;
    for (uint32_t i = 0; i < component_inst->resolved_alias_count; i++) {
        const WASMComponentResolvedAlias &alias =
            component_inst->resolved_aliases[i];

        if (std::string(alias.name) == "test:project/my-interface@0.1.0#add") {
            found_add_alias = true;
            ASSERT_EQ(alias.ref.type, WASM_COMP_CORE_RUNTIME_REF_FUNC);
            ASSERT_NE(alias.ref.of.function, nullptr);
            break;
        }
    }

    ASSERT_TRUE(found_add_alias);

    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestRuntimeResolvesCanonLiftedExports)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "runtime-component-exports";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;

    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate(module, helper->stack_size, helper->heap_size,
                                 helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    auto *component_inst = (WASMComponentInstance *)module_inst;
    ASSERT_EQ(component_inst->component_func_count, 1u);
    ASSERT_EQ(component_inst->component_instance_count, 1u);
    ASSERT_EQ(component_inst->component_export_count, 1u);

    const WASMComponentRuntimeFunc &lifted_func =
        component_inst->component_funcs[0];
    ASSERT_EQ(lifted_func.kind, WASM_COMP_RUNTIME_FUNC_LIFT);
    ASSERT_EQ(lifted_func.core_func_ref.type, WASM_COMP_CORE_RUNTIME_REF_FUNC);
    ASSERT_NE(lifted_func.core_func_ref.of.function, nullptr);

    const WASMComponentNamedExport &top_export =
        component_inst->component_exports[0];
    ASSERT_EQ(std::string(top_export.name),
              "test:project/my-interface@0.1.0");
    ASSERT_EQ(top_export.ref.type, WASM_COMP_RUNTIME_REF_INSTANCE);
    ASSERT_NE(top_export.ref.of.instance, nullptr);
    ASSERT_EQ(top_export.ref.of.instance->export_count, 1u);

    const WASMComponentNamedExport &nested_export =
        top_export.ref.of.instance->exports[0];
    ASSERT_EQ(std::string(nested_export.name), "add");
    ASSERT_EQ(nested_export.ref.type, WASM_COMP_RUNTIME_REF_FUNC);
    ASSERT_EQ(nested_export.ref.of.function, &component_inst->component_funcs[0]);

    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestRuntimeResolvesComponentExportAliases)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "runtime-component-aliases";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_TRUE(append_component_export_alias_sections(
        (WASMComponentModule *)module));

    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate(module, helper->stack_size, helper->heap_size,
                                 helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    auto *component_inst = (WASMComponentInstance *)module_inst;
    ASSERT_EQ(component_inst->component_func_count, 2u);
    ASSERT_EQ(component_inst->component_instance_count, 3u);
    ASSERT_EQ(component_inst->component_export_count, 3u);

    const WASMComponentRuntimeFunc &lifted_func =
        component_inst->component_funcs[0];
    const WASMComponentRuntimeFunc &aliased_func =
        component_inst->component_funcs[1];
    ASSERT_EQ(lifted_func.kind, WASM_COMP_RUNTIME_FUNC_LIFT);
    ASSERT_EQ(aliased_func.kind, WASM_COMP_RUNTIME_FUNC_LIFT);
    ASSERT_EQ(aliased_func.core_func_ref.type, WASM_COMP_CORE_RUNTIME_REF_FUNC);
    ASSERT_EQ(aliased_func.core_func_ref.of.function,
              lifted_func.core_func_ref.of.function);

    const WASMComponentNamedExport &aliased_func_export =
        component_inst->component_exports[1];
    ASSERT_EQ(std::string(aliased_func_export.name), "aliased-add");
    ASSERT_EQ(aliased_func_export.ref.type, WASM_COMP_RUNTIME_REF_FUNC);
    ASSERT_EQ(aliased_func_export.ref.of.function, &component_inst->component_funcs[1]);

    const WASMComponentNamedExport &aliased_instance_export =
        component_inst->component_exports[2];
    ASSERT_EQ(std::string(aliased_instance_export.name), "aliased-instance");
    ASSERT_EQ(aliased_instance_export.ref.type, WASM_COMP_RUNTIME_REF_INSTANCE);
    ASSERT_NE(aliased_instance_export.ref.of.instance, nullptr);
    ASSERT_EQ(aliased_instance_export.ref.of.instance->export_count, 1u);
    ASSERT_EQ(std::string(aliased_instance_export.ref.of.instance->exports[0].name),
              "add");
    ASSERT_EQ(aliased_instance_export.ref.of.instance->exports[0].ref.type,
              WASM_COMP_RUNTIME_REF_FUNC);
    ASSERT_EQ(aliased_instance_export.ref.of.instance->exports[0].ref.of.function,
              &component_inst->component_funcs[0]);

    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestRuntimeResolvesTopLevelComponentExportAliases)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "runtime-top-level-component-aliases";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_TRUE(append_top_level_component_export_alias_sections(
        (WASMComponentModule *)module));

    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate(module, helper->stack_size, helper->heap_size,
                                 helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    auto *component_inst = (WASMComponentInstance *)module_inst;
    ASSERT_EQ(component_inst->component_count, 3u);
    ASSERT_EQ(component_inst->component_instance_count, 2u);
    ASSERT_EQ(component_inst->component_export_count, 2u);

    const WASMComponentNamedExport &top_export =
        component_inst->component_exports[1];
    ASSERT_EQ(std::string(top_export.name), "aliased-component");
    ASSERT_EQ(top_export.ref.type, WASM_COMP_RUNTIME_REF_COMPONENT);
    ASSERT_EQ(top_export.ref.of.component->component,
              component_inst->components[1].component);
    ASSERT_EQ(top_export.ref.of.component->scope,
              component_inst->components[1].scope);
    ASSERT_EQ(component_inst->components[2].component,
              component_inst->components[1].component);
    ASSERT_EQ(component_inst->components[2].scope,
              component_inst->components[1].scope);

    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestRuntimeBindsNestedComponentInstanceImports)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "runtime-nested-instance-imports";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_TRUE(append_nested_component_instance_reexport_sections(
        (WASMComponentModule *)module));

    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate(module, helper->stack_size, helper->heap_size,
                                 helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    auto *component_inst = (WASMComponentInstance *)module_inst;
    auto *component_module = (WASMComponentModule *)module;
    const WASMComponent *nested_component_def =
        component_module->component
            .sections[component_module->component.section_count - 3]
            .parsed.component;
    const WASMComponent *expected_component =
        nested_component_def->sections[0].parsed.component;
    ASSERT_EQ(component_inst->component_instance_count, 2u);
    ASSERT_EQ(component_inst->component_export_count, 2u);

    const WASMComponentRuntimeInstance &forwarding_instance =
        component_inst->component_instances[1];
    ASSERT_EQ(forwarding_instance.export_count, 1u);
    ASSERT_EQ(std::string(forwarding_instance.exports[0].name), "forwarded");
    ASSERT_EQ(forwarding_instance.exports[0].ref.type,
              WASM_COMP_RUNTIME_REF_INSTANCE);
    ASSERT_EQ(forwarding_instance.exports[0].ref.of.instance,
              &component_inst->component_instances[0]);

    const WASMComponentNamedExport &top_export =
        component_inst->component_exports[1];
    ASSERT_EQ(std::string(top_export.name), "forwarded-instance");
    ASSERT_EQ(top_export.ref.type, WASM_COMP_RUNTIME_REF_INSTANCE);
    ASSERT_EQ(top_export.ref.of.instance, &component_inst->component_instances[1]);

    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestRuntimeResolvesNestedComponentAliasExports)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "runtime-nested-alias-exports";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_TRUE(append_nested_component_alias_export_sections(
        (WASMComponentModule *)module));

    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate(module, helper->stack_size, helper->heap_size,
                                 helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    auto *component_inst = (WASMComponentInstance *)module_inst;
    auto *component_module = (WASMComponentModule *)module;
    const WASMComponent *nested_component_def =
        component_module->component
            .sections[component_module->component.section_count - 3]
            .parsed.component;
    const WASMComponent *expected_component =
        nested_component_def->sections[0].parsed.component;
    ASSERT_EQ(component_inst->component_instance_count, 2u);
    ASSERT_EQ(component_inst->component_export_count, 2u);

    const WASMComponentRuntimeInstance &aliased_instance =
        component_inst->component_instances[1];
    ASSERT_EQ(aliased_instance.export_count, 1u);
    ASSERT_EQ(std::string(aliased_instance.exports[0].name), "forwarded-add");
    ASSERT_EQ(aliased_instance.exports[0].ref.type, WASM_COMP_RUNTIME_REF_FUNC);
    ASSERT_EQ(aliased_instance.exports[0].ref.of.function,
              &component_inst->component_funcs[0]);

    const WASMComponentNamedExport &top_export =
        component_inst->component_exports[1];
    ASSERT_EQ(std::string(top_export.name), "aliased-nested-add");
    ASSERT_EQ(top_export.ref.type, WASM_COMP_RUNTIME_REF_INSTANCE);
    ASSERT_EQ(top_export.ref.of.instance, &component_inst->component_instances[1]);

    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestRuntimeResolvesNestedComponentAliasExportsOfComponents)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "runtime-nested-component-aliases";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_TRUE(append_nested_component_component_alias_sections(
        (WASMComponentModule *)module));

    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate(module, helper->stack_size, helper->heap_size,
                                 helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    auto *component_inst = (WASMComponentInstance *)module_inst;
    auto *component_module = (WASMComponentModule *)module;
    const WASMComponent *nested_component_def =
        component_module->component
            .sections[component_module->component.section_count - 3]
            .parsed.component;
    const WASMComponent *expected_component =
        nested_component_def->sections[0].parsed.component;
    ASSERT_EQ(component_inst->component_instance_count, 2u);
    ASSERT_EQ(component_inst->component_export_count, 2u);

    const WASMComponentRuntimeInstance &nested_instance =
        component_inst->component_instances[1];
    ASSERT_EQ(nested_instance.export_count, 1u);
    ASSERT_EQ(std::string(nested_instance.exports[0].name), "aliased-component");
    ASSERT_EQ(nested_instance.exports[0].ref.type, WASM_COMP_RUNTIME_REF_COMPONENT);

    const WASMComponentNamedExport &top_export =
        component_inst->component_exports[1];
    ASSERT_EQ(std::string(top_export.name), "nested-component-alias");
    ASSERT_EQ(top_export.ref.type, WASM_COMP_RUNTIME_REF_INSTANCE);
    ASSERT_EQ(top_export.ref.of.instance, &component_inst->component_instances[1]);
    ASSERT_EQ(nested_instance.exports[0].ref.of.component->component,
              expected_component);

    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestRuntimeResolvesNestedOuterComponentAliasesInCurrentScope)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "runtime-nested-outer-component-aliases";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_TRUE(append_nested_component_outer_alias_sections(
        (WASMComponentModule *)module));

    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate(module, helper->stack_size, helper->heap_size,
                                 helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    auto *component_inst = (WASMComponentInstance *)module_inst;
    auto *component_module = (WASMComponentModule *)module;
    const WASMComponent *outer_component =
        component_module->component
            .sections[component_module->component.section_count - 3]
            .parsed.component;
    const WASMComponent *expected_component =
        outer_component->sections[0].parsed.component;
    const WASMComponentNamedExport &top_export =
        component_inst->component_exports[1];
    const WASMComponentRuntimeInstance &outer_instance =
        component_inst->component_instances[1];
    const WASMComponentRuntimeInstance &aliased_instance =
        outer_instance.owned_instances[0];

    ASSERT_EQ(std::string(top_export.name), "ct0-outer-alias");
    ASSERT_EQ(top_export.ref.type, WASM_COMP_RUNTIME_REF_INSTANCE);
    ASSERT_EQ(top_export.ref.of.instance, &component_inst->component_instances[1]);
    ASSERT_EQ(outer_instance.owned_instance_count, 1u);
    ASSERT_EQ(aliased_instance.export_count, 1u);
    ASSERT_EQ(std::string(aliased_instance.exports[0].name), "outer-component");
    ASSERT_EQ(aliased_instance.exports[0].ref.type,
              WASM_COMP_RUNTIME_REF_COMPONENT);
    ASSERT_EQ(aliased_instance.exports[0].ref.of.component->component,
              expected_component);

    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestRuntimeResolvesNestedOuterComponentAliasesInParentScope)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "runtime-nested-parent-outer-component-aliases";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_TRUE(append_nested_component_outer_alias_parent_sections(
        (WASMComponentModule *)module));

    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate(module, helper->stack_size, helper->heap_size,
                                 helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    auto *component_inst = (WASMComponentInstance *)module_inst;
    auto *component_module = (WASMComponentModule *)module;
    const WASMComponent *outer_component =
        component_module->component
            .sections[component_module->component.section_count - 3]
            .parsed.component;
    const WASMComponent *expected_component =
        outer_component->sections[0].parsed.component;
    const WASMComponentNamedExport &top_export =
        component_inst->component_exports[1];
    const WASMComponentRuntimeInstance &outer_instance =
        component_inst->component_instances[1];
    const WASMComponentRuntimeInstance &middle_instance =
        outer_instance.owned_instances[0];
    const WASMComponentRuntimeInstance &inner_instance =
        middle_instance.owned_instances[0];

    ASSERT_EQ(std::string(top_export.name), "ct1-top-level-outer-alias");
    ASSERT_EQ(top_export.ref.type, WASM_COMP_RUNTIME_REF_INSTANCE);
    ASSERT_EQ(top_export.ref.of.instance, &component_inst->component_instances[1]);
    ASSERT_EQ(outer_instance.owned_instance_count, 1u);
    ASSERT_EQ(middle_instance.owned_instance_count, 1u);
    ASSERT_EQ(inner_instance.export_count, 1u);
    ASSERT_EQ(std::string(inner_instance.exports[0].name), "outer-component");
    ASSERT_EQ(inner_instance.exports[0].ref.type,
              WASM_COMP_RUNTIME_REF_COMPONENT);
    ASSERT_EQ(inner_instance.exports[0].ref.of.component->component,
              expected_component);

    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestRuntimeExecutesTopLevelScalarStartSections)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "runtime-top-level-start-sections";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_TRUE(append_top_level_s32_value_section(
        (WASMComponentModule *)module, runtime_value_section_i32_two_bytes,
        (uint32_t)sizeof(runtime_value_section_i32_two_bytes)));
    ASSERT_TRUE(append_top_level_s32_value_section(
        (WASMComponentModule *)module, runtime_value_section_i32_three_bytes,
        (uint32_t)sizeof(runtime_value_section_i32_three_bytes)));

    uint32_t start_args[] = { 0, 1 };
    ASSERT_TRUE(append_top_level_start_section((WASMComponentModule *)module, 0,
                                               start_args, 2, 1));
    ASSERT_TRUE(append_top_level_value_export_section(
        (WASMComponentModule *)module, "start-result", 2));

    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate(module, helper->stack_size, helper->heap_size,
                                 helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    wasm_component_value_t value = {};
    ASSERT_TRUE(
        wasm_runtime_lookup_component_value(module_inst, "start-result", &value));
    ASSERT_EQ(value.type.kind, WASM_COMPONENT_VALUE_TYPE_PRIMITIVE);
    ASSERT_EQ(value.type.type.primitive_type, WASM_COMPONENT_PRIMITIVE_VALUE_S32);
    ASSERT_EQ(value.storage_kind, WASM_COMPONENT_VALUE_STORAGE_INLINE);
    ASSERT_EQ(value.byte_size, sizeof(runtime_value_section_i32_five_bytes));
    ASSERT_EQ(memcmp(wasm_component_value_get_data(&value),
                     runtime_value_section_i32_five_bytes,
                     sizeof(runtime_value_section_i32_five_bytes)),
              0);

    wasm_component_value_destroy(&value);
    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestRuntimeRejectsUnsupportedTopLevelStartSections)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "runtime-top-level-start-sections-unsupported";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_TRUE(append_top_level_s32_value_section(
        (WASMComponentModule *)module, runtime_value_section_i32_two_bytes,
        (uint32_t)sizeof(runtime_value_section_i32_two_bytes)));
    ASSERT_TRUE(append_top_level_s32_value_section(
        (WASMComponentModule *)module, runtime_value_section_i32_three_bytes,
        (uint32_t)sizeof(runtime_value_section_i32_three_bytes)));

    uint32_t start_args[] = { 0, 1 };
    ASSERT_TRUE(append_top_level_start_section((WASMComponentModule *)module, 0,
                                               start_args, 2, 1));

    WASMComponentCanon *canon =
        find_first_canon_lift((WASMComponentModule *)module);
    ASSERT_NE(canon, nullptr);
    WASMComponentFuncType *func_type = lookup_local_component_func_type(
        (WASMComponentModule *)module, canon->canon_data.lift.type_idx);
    ASSERT_NE(func_type, nullptr);
    ASSERT_NE(func_type->params, nullptr);
    ASSERT_GT(func_type->params->count, 0u);
    ASSERT_NE(func_type->params->params[0].value_type, nullptr);
    func_type->params->params[0].value_type->type = WASM_COMP_VAL_TYPE_PRIMVAL;
    func_type->params->params[0].value_type->type_specific.primval_type =
        WASM_COMP_PRIMVAL_STRING;

    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate(module, helper->stack_size, helper->heap_size,
                                 helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
    ASSERT_EQ(module_inst, nullptr);
    ASSERT_NE(strstr(helper->error_buf,
                     "component canon lift function only supports UTF-8 "
                     "strings"),
              nullptr)
        << helper->error_buf;

    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestRuntimeExecutesNestedScalarStartSections)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "runtime-nested-start-sections";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_TRUE(append_nested_start_section((WASMComponentModule *)module, 1));

    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate(module, helper->stack_size, helper->heap_size,
                                 helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    auto *component_inst = (WASMComponentInstance *)module_inst;
    wasm_component_instance_t exported_instance =
        wasm_runtime_lookup_component_instance(module_inst,
                                              "nested-start-instance");
    ASSERT_NE(exported_instance, nullptr);
    ASSERT_EQ(exported_instance,
              &component_inst
                   ->component_instances[component_inst->component_instance_count
                                         - 1]);
    ASSERT_EQ(exported_instance->export_count, 1u);
    ASSERT_EQ(std::string(exported_instance->exports[0].name), "start-result");
    ASSERT_EQ(exported_instance->exports[0].ref.type, WASM_COMP_RUNTIME_REF_VALUE);
    ASSERT_EQ(exported_instance->owned_value_count, 3u);
    ASSERT_EQ(exported_instance->exports[0].ref.of.value,
              &exported_instance->owned_values[2]);
    ASSERT_EQ(exported_instance->owned_values[2].type.kind,
              WASM_COMP_RUNTIME_VALUE_TYPE_PRIMITIVE);
    ASSERT_EQ(exported_instance->owned_values[2].type.type.primitive_type,
              WASM_COMP_PRIMVAL_S32);
    ASSERT_EQ(exported_instance->owned_values[2].storage_kind,
              WASM_COMP_RUNTIME_VALUE_STORAGE_INLINE);
    ASSERT_EQ(exported_instance->owned_values[2].byte_size,
              sizeof(runtime_value_section_i32_five_bytes));
    ASSERT_EQ(memcmp(wasm_component_runtime_value_get_data(
                         &exported_instance->owned_values[2]),
                     runtime_value_section_i32_five_bytes,
                     sizeof(runtime_value_section_i32_five_bytes)),
              0);

    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestRuntimeRejectsUnsupportedNestedStartSectionTypes)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "runtime-nested-start-sections-unsupported-type";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_TRUE(append_nested_start_section((WASMComponentModule *)module, 1));

    WASMComponentCanon *canon =
        find_first_canon_lift((WASMComponentModule *)module);
    ASSERT_NE(canon, nullptr);
    WASMComponentFuncType *func_type = lookup_local_component_func_type(
        (WASMComponentModule *)module, canon->canon_data.lift.type_idx);
    ASSERT_NE(func_type, nullptr);
    ASSERT_NE(func_type->params, nullptr);
    ASSERT_GT(func_type->params->count, 0u);
    ASSERT_NE(func_type->params->params[0].value_type, nullptr);
    func_type->params->params[0].value_type->type = WASM_COMP_VAL_TYPE_PRIMVAL;
    func_type->params->params[0].value_type->type_specific.primval_type =
        WASM_COMP_PRIMVAL_STRING;

    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate(module, helper->stack_size, helper->heap_size,
                                 helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
    ASSERT_EQ(module_inst, nullptr);
    ASSERT_NE(strstr(helper->error_buf,
                     "component canon lift function only supports UTF-8 "
                     "strings"),
              nullptr)
        << helper->error_buf;

    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestRuntimeRejectsUnsupportedNestedStartSectionShapes)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "runtime-nested-start-sections-unsupported-shape";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_TRUE(append_nested_start_section((WASMComponentModule *)module, 2));

    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate(module, helper->stack_size, helper->heap_size,
                                 helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
    ASSERT_EQ(module_inst, nullptr);
    ASSERT_NE(strstr(helper->error_buf,
                     "nested component start section failed: expects 1 "
                     "results but received 2"),
              nullptr)
        << helper->error_buf;

    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestRuntimeValueHelpersResolveAndTrackOwnership)
{
    uint8_t inline_bytes[] = { 0x01, 0x00, 0x00, 0x00 };
    WASMComponentValueType inline_type = {};
    WASMComponentRuntimeValue inline_value = {};
    WASMComponentRuntimeValue inline_clone = {};

    inline_type.type = WASM_COMP_VAL_TYPE_PRIMVAL;
    inline_type.type_specific.primval_type = WASM_COMP_PRIMVAL_S32;
    ASSERT_TRUE(wasm_component_runtime_value_init_inline(
        &inline_value, nullptr, &inline_type, inline_bytes, sizeof(inline_bytes),
        helper->error_buf, (uint32_t)sizeof(helper->error_buf)));
    ASSERT_EQ(inline_value.type.kind, WASM_COMP_RUNTIME_VALUE_TYPE_PRIMITIVE);
    ASSERT_EQ(inline_value.type.type.primitive_type, WASM_COMP_PRIMVAL_S32);
    ASSERT_EQ(inline_value.storage_kind, WASM_COMP_RUNTIME_VALUE_STORAGE_INLINE);
    ASSERT_TRUE(wasm_component_runtime_value_clone_borrowed(
        &inline_clone, &inline_value, helper->error_buf,
        (uint32_t)sizeof(helper->error_buf)));
    ASSERT_EQ(inline_clone.storage_kind, WASM_COMP_RUNTIME_VALUE_STORAGE_INLINE);
    ASSERT_EQ(memcmp(wasm_component_runtime_value_get_data(&inline_clone),
                     inline_bytes, sizeof(inline_bytes)),
              0);

    WASMComponentBorrowType borrow_type = {};
    WASMComponentDefValType def_val_type = {};
    WASMComponentTypes type_entry = {};
    WASMComponentTypeSection type_section = {};
    WASMComponentSection section = {};
    WASMComponent component = {};
    WASMComponentValueType type_idx = {};
    uint8_t borrowed_bytes[] = { 0xaa, 0xbb, 0xcc, 0xdd };
    WASMComponentRuntimeValue borrowed_value = {};
    RuntimeValueFinalizerState finalizer_state = {};
    WASMComponentRuntimeValue owned_value = {};
    WASMComponentRuntimeValue owned_alias = {};
    auto *owned_bytes = (uint8_t *)wasm_runtime_malloc(sizeof(borrowed_bytes));

    ASSERT_NE(owned_bytes, nullptr);
    memcpy(owned_bytes, borrowed_bytes, sizeof(borrowed_bytes));

    borrow_type.type_idx = 7;
    def_val_type.tag = WASM_COMP_DEF_VAL_BORROW;
    def_val_type.def_val.borrow = &borrow_type;
    type_entry.tag = WASM_COMP_DEF_TYPE;
    type_entry.type.def_val_type = &def_val_type;
    type_section.count = 1;
    type_section.types = &type_entry;
    section.id = WASM_COMP_SECTION_TYPE;
    section.parsed.type_section = &type_section;
    component.section_count = 1;
    component.sections = &section;
    type_idx.type = WASM_COMP_VAL_TYPE_IDX;
    type_idx.type_specific.type_idx = 0;

    ASSERT_TRUE(wasm_component_runtime_value_init_borrowed(
        &borrowed_value, &component, &type_idx, borrowed_bytes,
        sizeof(borrowed_bytes), helper->error_buf,
        (uint32_t)sizeof(helper->error_buf)));
    ASSERT_EQ(borrowed_value.type.kind, WASM_COMP_RUNTIME_VALUE_TYPE_DEFINED);
    ASSERT_EQ(borrowed_value.type.type.defined_type->tag, WASM_COMP_DEF_VAL_BORROW);
    ASSERT_EQ(borrowed_value.storage_kind, WASM_COMP_RUNTIME_VALUE_STORAGE_BORROWED);
    ASSERT_EQ(wasm_component_runtime_value_get_data(&borrowed_value),
              borrowed_bytes);

    ASSERT_TRUE(wasm_component_runtime_value_init_owned(
        &owned_value, &component, &type_idx, owned_bytes, sizeof(borrowed_bytes),
        runtime_value_test_finalizer, &finalizer_state, helper->error_buf,
        (uint32_t)sizeof(helper->error_buf)));
    ASSERT_TRUE(wasm_component_runtime_value_clone_borrowed(
        &owned_alias, &owned_value, helper->error_buf,
        (uint32_t)sizeof(helper->error_buf)));
    ASSERT_EQ(owned_alias.storage_kind, WASM_COMP_RUNTIME_VALUE_STORAGE_BORROWED);
    ASSERT_EQ(wasm_component_runtime_value_get_data(&owned_alias), owned_bytes);

    wasm_component_runtime_value_clear(&owned_value);
    ASSERT_EQ(finalizer_state.call_count, 1u);
    wasm_component_runtime_value_clear(&owned_alias);
    ASSERT_EQ(finalizer_state.call_count, 1u);
    wasm_component_runtime_value_clear(&borrowed_value);
    wasm_component_runtime_value_clear(&inline_clone);
    wasm_component_runtime_value_clear(&inline_value);
}

TEST_F(BinaryParserTest, TestRuntimeResourceStateTracksLocalImportedAndAliasTypes)
{
    WASMComponentTypeSection type_section = {};
    WASMComponentImportSection import_section = {};
    WASMComponentExportSection export_section = {};
    WASMComponentSection sections[3] = {};
    WASMComponent component = {};

    ASSERT_TRUE(init_resource_type_section(&type_section, 1, false));

    import_section.count = 2;
    import_section.imports =
        (WASMComponentImport *)wasm_runtime_malloc(sizeof(WASMComponentImport) * 2);
    ASSERT_NE(import_section.imports, nullptr);
    memset(import_section.imports, 0, sizeof(WASMComponentImport) * 2);

    import_section.imports[0].import_name = create_import_name("sub-resource");
    import_section.imports[0].extern_desc = create_extern_desc(WASM_COMP_EXTERN_TYPE);
    ASSERT_NE(import_section.imports[0].import_name, nullptr);
    ASSERT_NE(import_section.imports[0].extern_desc, nullptr);
    import_section.imports[0].extern_desc->extern_desc.type.type_bound =
        create_type_bound(WASM_COMP_TYPEBOUND_TYPE, 0);
    ASSERT_NE(import_section.imports[0].extern_desc->extern_desc.type.type_bound,
              nullptr);

    import_section.imports[1].import_name = create_import_name("eq-resource");
    import_section.imports[1].extern_desc = create_extern_desc(WASM_COMP_EXTERN_TYPE);
    ASSERT_NE(import_section.imports[1].import_name, nullptr);
    ASSERT_NE(import_section.imports[1].extern_desc, nullptr);
    import_section.imports[1].extern_desc->extern_desc.type.type_bound =
        create_type_bound(WASM_COMP_TYPEBOUND_EQ, 0);
    ASSERT_NE(import_section.imports[1].extern_desc->extern_desc.type.type_bound,
              nullptr);

    export_section.count = 1;
    export_section.exports =
        (WASMComponentExport *)wasm_runtime_malloc(sizeof(WASMComponentExport));
    ASSERT_NE(export_section.exports, nullptr);
    memset(export_section.exports, 0, sizeof(WASMComponentExport));
    export_section.exports[0].export_name = create_export_name("resource-export");
    export_section.exports[0].sort_idx = create_sort_idx(WASM_COMP_SORT_TYPE, 0);
    ASSERT_NE(export_section.exports[0].export_name, nullptr);
    ASSERT_NE(export_section.exports[0].sort_idx, nullptr);

    sections[0].id = WASM_COMP_SECTION_TYPE;
    sections[0].parsed.type_section = &type_section;
    sections[1].id = WASM_COMP_SECTION_IMPORTS;
    sections[1].parsed.import_section = &import_section;
    sections[2].id = WASM_COMP_SECTION_EXPORTS;
    sections[2].parsed.export_section = &export_section;
    component.section_count = 3;
    component.sections = sections;

    WASMComponentRuntimeResourceState *resource_state =
        wasm_component_resource_state_create(&component, helper->error_buf,
                                            (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(resource_state, nullptr) << helper->error_buf;
    ASSERT_EQ(resource_state->type_count, 4u);

    const auto *local_type =
        wasm_component_resource_lookup_runtime_type_const(resource_state, 0);
    const auto *imported_type =
        wasm_component_resource_lookup_runtime_type_const(resource_state, 1);
    const auto *eq_alias_type =
        wasm_component_resource_lookup_runtime_type_const(resource_state, 2);
    const auto *export_alias_type =
        wasm_component_resource_lookup_runtime_type_const(resource_state, 3);

    ASSERT_NE(local_type, nullptr);
    ASSERT_NE(imported_type, nullptr);
    ASSERT_NE(eq_alias_type, nullptr);
    ASSERT_NE(export_alias_type, nullptr);
    ASSERT_EQ(local_type->kind, WASM_COMP_RUNTIME_RESOURCE_TYPE_LOCAL);
    ASSERT_EQ(local_type->canonical_type_idx, 0u);
    ASSERT_EQ(imported_type->kind, WASM_COMP_RUNTIME_RESOURCE_TYPE_IMPORTED);
    ASSERT_EQ(imported_type->canonical_type_idx, 1u);
    ASSERT_EQ(eq_alias_type->kind, WASM_COMP_RUNTIME_RESOURCE_TYPE_ALIAS);
    ASSERT_EQ(eq_alias_type->canonical_type_idx, 0u);
    ASSERT_EQ(eq_alias_type->source_type_idx, 0u);
    ASSERT_EQ(export_alias_type->kind, WASM_COMP_RUNTIME_RESOURCE_TYPE_ALIAS);
    ASSERT_EQ(export_alias_type->canonical_type_idx, 0u);
    ASSERT_EQ(export_alias_type->source_type_idx, 0u);

    wasm_component_resource_state_destroy(resource_state);
}

TEST_F(BinaryParserTest, TestRuntimeInstantiatesTopLevelValueSections)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "runtime-top-level-value-sections";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_TRUE(append_top_level_value_section((WASMComponentModule *)module));

    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate(module, helper->stack_size, helper->heap_size,
                                 helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    auto *component_inst = (WASMComponentInstance *)module_inst;
    ASSERT_EQ(component_inst->component_value_count, 1u);
    ASSERT_EQ(component_inst->component_values[0].type.kind,
              WASM_COMP_RUNTIME_VALUE_TYPE_PRIMITIVE);
    ASSERT_EQ(component_inst->component_values[0].type.type.primitive_type,
              WASM_COMP_PRIMVAL_S32);
    ASSERT_EQ(component_inst->component_values[0].storage_kind,
              WASM_COMP_RUNTIME_VALUE_STORAGE_BORROWED);
    ASSERT_EQ(component_inst->component_values[0].byte_size,
              sizeof(runtime_value_section_i32_bytes));
    ASSERT_EQ(memcmp(wasm_component_runtime_value_get_data(
                         &component_inst->component_values[0]),
                     runtime_value_section_i32_bytes,
                     sizeof(runtime_value_section_i32_bytes)),
              0);

    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestRuntimeInstantiatesNestedValueSections)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "runtime-nested-value-sections";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_TRUE(append_nested_value_section((WASMComponentModule *)module));

    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate(module, helper->stack_size, helper->heap_size,
                                 helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    auto *component_inst = (WASMComponentInstance *)module_inst;
    ASSERT_GE(component_inst->component_instance_count, 1u);
    const WASMComponentRuntimeInstance &nested_instance =
        component_inst->component_instances[component_inst->component_instance_count
                                            - 1];
    ASSERT_EQ(nested_instance.owned_value_count, 1u);
    ASSERT_EQ(nested_instance.owned_values[0].type.kind,
              WASM_COMP_RUNTIME_VALUE_TYPE_PRIMITIVE);
    ASSERT_EQ(nested_instance.owned_values[0].type.type.primitive_type,
              WASM_COMP_PRIMVAL_S32);
    ASSERT_EQ(nested_instance.owned_values[0].storage_kind,
              WASM_COMP_RUNTIME_VALUE_STORAGE_BORROWED);
    ASSERT_EQ(nested_instance.owned_values[0].byte_size,
              sizeof(runtime_value_section_i32_bytes));

    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestRuntimeCleansOwnedResourceHandlesOnDeinstantiate)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "runtime-resource-foundation";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_TRUE(append_top_level_resource_type_section((WASMComponentModule *)module));
    ASSERT_TRUE(append_nested_resource_component_instance_sections(
        (WASMComponentModule *)module));

    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate(module, helper->stack_size, helper->heap_size,
                                 helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    auto *component_inst = (WASMComponentInstance *)module_inst;
    ASSERT_NE(component_inst->resource_state, nullptr);
    ASSERT_GT(component_inst->resource_state->type_count, 0u);
    const uint32_t top_level_type_idx = component_inst->resource_state->type_count - 1;
    const auto *top_level_resource =
        wasm_component_resource_lookup_runtime_type_const(
            component_inst->resource_state, top_level_type_idx);
    ASSERT_NE(top_level_resource, nullptr);
    ASSERT_EQ(top_level_resource->kind, WASM_COMP_RUNTIME_RESOURCE_TYPE_LOCAL);

    ASSERT_GE(component_inst->component_instance_count, 1u);
    auto *nested_inst =
        &component_inst->component_instances[component_inst->component_instance_count
                                            - 1];
    ASSERT_NE(nested_inst->resource_state, nullptr);
    ASSERT_GT(nested_inst->resource_state->type_count, 0u);
    const auto *nested_resource =
        wasm_component_resource_lookup_runtime_type_const(
            nested_inst->resource_state, nested_inst->resource_state->type_count - 1);
    ASSERT_NE(nested_resource, nullptr);
    ASSERT_EQ(nested_resource->kind, WASM_COMP_RUNTIME_RESOURCE_TYPE_LOCAL);

    RuntimeValueFinalizerState finalizer_state = {};
    auto *root_payload = (uint8_t *)wasm_runtime_malloc(4);
    auto *nested_payload = (uint8_t *)wasm_runtime_malloc(4);
    uint32_t root_handle = 0, nested_handle = 0;
    ASSERT_NE(root_payload, nullptr);
    ASSERT_NE(nested_payload, nullptr);

    ASSERT_TRUE(wasm_component_resource_create_owned_handle(
        component_inst->resource_state, top_level_type_idx, root_payload,
        resource_handle_test_finalizer, &finalizer_state, &root_handle,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf)));
    ASSERT_TRUE(wasm_component_resource_create_owned_handle(
        nested_inst->resource_state, nested_inst->resource_state->type_count - 1,
        nested_payload,
        resource_handle_test_finalizer, &finalizer_state, &nested_handle,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf)));
    ASSERT_EQ(root_handle, 1u);
    ASSERT_EQ(nested_handle, 1u);
    ASSERT_EQ(top_level_resource->handle_table.owned_handle_count, 1u);
    ASSERT_EQ(nested_resource->handle_table.owned_handle_count, 1u);

    wasm_runtime_deinstantiate(module_inst);
    ASSERT_EQ(finalizer_state.call_count, 2u);
    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestRuntimeResolvesTopLevelValueImportAliases)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "runtime-top-level-value-import-aliases";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_TRUE(append_top_level_value_alias_sections(
        (WASMComponentModule *)module));

    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate(module, helper->stack_size, helper->heap_size,
                                 helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    auto *component_inst = (WASMComponentInstance *)module_inst;
    const WASMComponentRuntimeInstance &value_instance =
        component_inst->component_instances[1];
    const WASMComponentNamedExport &top_instance_export =
        component_inst->component_exports[1];
    const WASMComponentNamedExport &top_value_export =
        component_inst->component_exports[2];

    ASSERT_EQ(std::string(top_instance_export.name), "value-import-instance");
    ASSERT_EQ(top_instance_export.ref.type, WASM_COMP_RUNTIME_REF_INSTANCE);
    ASSERT_EQ(top_instance_export.ref.of.instance, &component_inst->component_instances[1]);
    ASSERT_EQ(value_instance.export_count, 1u);
    ASSERT_EQ(std::string(value_instance.exports[0].name), "forwarded-value");
    ASSERT_EQ(value_instance.exports[0].ref.type, WASM_COMP_RUNTIME_REF_VALUE);
    ASSERT_EQ(value_instance.exports[0].ref.of.value, &component_inst->component_values[0]);
    ASSERT_EQ(value_instance.exports[0].ref.of.value->storage_kind,
              WASM_COMP_RUNTIME_VALUE_STORAGE_BORROWED);
    ASSERT_EQ(memcmp(wasm_component_runtime_value_get_data(
                         value_instance.exports[0].ref.of.value),
                     runtime_value_section_i32_bytes,
                     sizeof(runtime_value_section_i32_bytes)),
              0);

    ASSERT_EQ(std::string(top_value_export.name), "aliased-value");
    ASSERT_EQ(top_value_export.ref.type, WASM_COMP_RUNTIME_REF_VALUE);
    ASSERT_EQ(top_value_export.ref.of.value, &component_inst->component_values[1]);
    ASSERT_EQ(top_value_export.ref.of.value->storage_kind,
              WASM_COMP_RUNTIME_VALUE_STORAGE_BORROWED);
    ASSERT_EQ(memcmp(wasm_component_runtime_value_get_data(top_value_export.ref.of.value),
                     runtime_value_section_i32_bytes,
                     sizeof(runtime_value_section_i32_bytes)),
              0);

    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestRuntimeBindsTopLevelComponentImports)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "runtime-top-level-component-imports";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_TRUE(append_top_level_component_import_sections(
        (WASMComponentModule *)module));

    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate(module, helper->stack_size, helper->heap_size,
                                 helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    auto *component_inst = (WASMComponentInstance *)module_inst;
    auto *component_module = (WASMComponentModule *)module;
    const WASMComponent *expected_component =
        component_module->component
            .sections[component_module->component.section_count - 4]
            .parsed.component;
    const WASMComponentRuntimeInstance &import_instance =
        component_inst->component_instances[1];

    ASSERT_EQ(import_instance.export_count, 1u);
    ASSERT_EQ(std::string(import_instance.exports[0].name), "forwarded-component");
    ASSERT_EQ(import_instance.exports[0].ref.type, WASM_COMP_RUNTIME_REF_COMPONENT);
    ASSERT_EQ(import_instance.exports[0].ref.of.component->component,
              expected_component);

    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestRuntimeResolvesNestedValueImportAliases)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "runtime-nested-value-import-aliases";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_TRUE(append_nested_value_import_alias_sections(
        (WASMComponentModule *)module));

    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate(module, helper->stack_size, helper->heap_size,
                                 helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    auto *component_inst = (WASMComponentInstance *)module_inst;
    const WASMComponentNamedExport &top_export =
        component_inst->component_exports[1];
    const WASMComponentRuntimeInstance &outer_instance =
        component_inst->component_instances[1];
    const WASMComponentRuntimeInstance &forwarded_instance =
        outer_instance.owned_instances[0];
    const WASMComponentRuntimeInstance &aliased_instance =
        outer_instance.owned_instances[1];

    ASSERT_EQ(std::string(top_export.name), "nested-value-alias-instance");
    ASSERT_EQ(top_export.ref.type, WASM_COMP_RUNTIME_REF_INSTANCE);
    ASSERT_EQ(top_export.ref.of.instance, &component_inst->component_instances[1]);
    ASSERT_EQ(outer_instance.owned_value_count, 1u);
    ASSERT_EQ(outer_instance.owned_instance_count, 2u);
    ASSERT_EQ(forwarded_instance.export_count, 1u);
    ASSERT_EQ(std::string(forwarded_instance.exports[0].name), "forwarded-value");
    ASSERT_EQ(forwarded_instance.exports[0].ref.type, WASM_COMP_RUNTIME_REF_VALUE);
    ASSERT_EQ(forwarded_instance.exports[0].ref.of.value,
              &outer_instance.owned_values[0]);

    ASSERT_EQ(outer_instance.export_count, 1u);
    ASSERT_EQ(std::string(outer_instance.exports[0].name), "value-alias-instance");
    ASSERT_EQ(outer_instance.exports[0].ref.type, WASM_COMP_RUNTIME_REF_INSTANCE);
    ASSERT_EQ(outer_instance.exports[0].ref.of.instance, &outer_instance.owned_instances[1]);
    ASSERT_EQ(aliased_instance.export_count, 1u);
    ASSERT_EQ(std::string(aliased_instance.exports[0].name), "wrapped");
    ASSERT_EQ(aliased_instance.exports[0].ref.type, WASM_COMP_RUNTIME_REF_VALUE);
    ASSERT_EQ(aliased_instance.exports[0].ref.of.value, &outer_instance.owned_values[0]);
    ASSERT_EQ(memcmp(wasm_component_runtime_value_get_data(
                         aliased_instance.exports[0].ref.of.value),
                     runtime_value_section_i32_bytes,
                     sizeof(runtime_value_section_i32_bytes)),
              0);

    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestRuntimeBindsNestedComponentImports)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "runtime-nested-component-imports";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_TRUE(append_nested_component_import_sections(
        (WASMComponentModule *)module));

    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate(module, helper->stack_size, helper->heap_size,
                                 helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    auto *component_inst = (WASMComponentInstance *)module_inst;
    auto *component_module = (WASMComponentModule *)module;
    const WASMComponent *outer_component =
        component_module->component
            .sections[component_module->component.section_count - 3]
            .parsed.component;
    const WASMComponent *expected_component =
        outer_component->sections[0].parsed.component;
    const WASMComponentRuntimeInstance &outer_instance =
        component_inst->component_instances[1];
    const WASMComponentRuntimeInstance &nested_instance =
        outer_instance.owned_instances[0];

    ASSERT_EQ(outer_instance.owned_instance_count, 1u);
    ASSERT_EQ(nested_instance.export_count, 1u);
    ASSERT_EQ(std::string(nested_instance.exports[0].name), "forwarded-component");
    ASSERT_EQ(nested_instance.exports[0].ref.type, WASM_COMP_RUNTIME_REF_COMPONENT);
    ASSERT_EQ(nested_instance.exports[0].ref.of.component->component,
              expected_component);

    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestRuntimeBindsNestedCoreModuleImports)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "runtime-nested-core-module-imports";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_TRUE(append_nested_core_module_import_sections(
        (WASMComponentModule *)module));

    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate(module, helper->stack_size, helper->heap_size,
                                 helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    auto *component_inst = (WASMComponentInstance *)module_inst;
    const WASMComponentRuntimeInstance &nested_instance =
        component_inst->component_instances[1];

    ASSERT_EQ(nested_instance.export_count, 1u);
    ASSERT_EQ(std::string(nested_instance.exports[0].name),
              "forwarded-core-module");
    ASSERT_EQ(nested_instance.exports[0].ref.type,
              WASM_COMP_RUNTIME_REF_CORE_MODULE);
    ASSERT_EQ(nested_instance.exports[0].ref.of.core_module,
              component_inst->core_modules[0]);

    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestRuntimeBuildsTopLevelInlineComponentInstances)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "runtime-top-level-inline-component-instances";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_TRUE(append_top_level_inline_component_instance_sections(
        (WASMComponentModule *)module));

    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate(module, helper->stack_size, helper->heap_size,
                                 helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    auto *component_inst = (WASMComponentInstance *)module_inst;
    const WASMComponentRuntimeInstance &inline_instance =
        component_inst->component_instances[1];
    const WASMComponentNamedExport &top_export =
        component_inst->component_exports[1];

    ASSERT_EQ(inline_instance.export_count, 1u);
    ASSERT_EQ(std::string(inline_instance.exports[0].name), "wrapped-component");
    ASSERT_EQ(inline_instance.exports[0].ref.type, WASM_COMP_RUNTIME_REF_COMPONENT);
    ASSERT_EQ(inline_instance.exports[0].ref.of.component->component,
              component_inst->components[0].component);
    ASSERT_EQ(std::string(top_export.name), "top-level-inline-component-instance");
    ASSERT_EQ(top_export.ref.type, WASM_COMP_RUNTIME_REF_INSTANCE);
    ASSERT_EQ(top_export.ref.of.instance, &component_inst->component_instances[1]);

    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestRuntimeExportsTopLevelCoreModules)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "runtime-top-level-core-module-exports";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_TRUE(
        append_top_level_core_module_export_sections((WASMComponentModule *)module));

    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate(module, helper->stack_size, helper->heap_size,
                                 helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    auto *component_inst = (WASMComponentInstance *)module_inst;
    const WASMComponentNamedExport &top_export =
        component_inst->component_exports[1];

    ASSERT_EQ(std::string(top_export.name), "top-level-core-module");
    ASSERT_EQ(top_export.ref.type, WASM_COMP_RUNTIME_REF_CORE_MODULE);
    ASSERT_EQ(top_export.ref.of.core_module, component_inst->core_modules[0]);

    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestRuntimeBuildsTopLevelInlineCoreModuleInstances)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "runtime-top-level-inline-core-module-instances";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_TRUE(append_top_level_inline_core_module_instance_sections(
        (WASMComponentModule *)module));

    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate(module, helper->stack_size, helper->heap_size,
                                 helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    auto *component_inst = (WASMComponentInstance *)module_inst;
    const WASMComponentRuntimeInstance &inline_instance =
        component_inst->component_instances[1];
    const WASMComponentNamedExport &top_export =
        component_inst->component_exports[1];

    ASSERT_EQ(inline_instance.export_count, 1u);
    ASSERT_EQ(std::string(inline_instance.exports[0].name), "wrapped-core-module");
    ASSERT_EQ(inline_instance.exports[0].ref.type, WASM_COMP_RUNTIME_REF_CORE_MODULE);
    ASSERT_EQ(inline_instance.exports[0].ref.of.core_module,
              component_inst->core_modules[0]);
    ASSERT_EQ(std::string(top_export.name), "top-level-inline-core-module-instance");
    ASSERT_EQ(top_export.ref.type, WASM_COMP_RUNTIME_REF_INSTANCE);
    ASSERT_EQ(top_export.ref.of.instance, &component_inst->component_instances[1]);

    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestRuntimeBuildsNestedInlineComponentInstancesOfComponents)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "runtime-nested-inline-component-instances";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_TRUE(append_nested_component_inline_component_instance_sections(
        (WASMComponentModule *)module));

    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate(module, helper->stack_size, helper->heap_size,
                                 helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    auto *component_inst = (WASMComponentInstance *)module_inst;
    const WASMComponentRuntimeInstance &nested_instance =
        component_inst->component_instances[1];
    const WASMComponentRuntimeInstance &child_instance =
        nested_instance.owned_instances[0];

    ASSERT_EQ(nested_instance.export_count, 1u);
    ASSERT_EQ(nested_instance.owned_instance_count, 1u);
    ASSERT_EQ(std::string(nested_instance.exports[0].name), "wrapped-instance");
    ASSERT_EQ(nested_instance.exports[0].ref.type, WASM_COMP_RUNTIME_REF_INSTANCE);
    ASSERT_EQ(child_instance.export_count, 1u);
    ASSERT_EQ(std::string(child_instance.exports[0].name), "wrapped");
    ASSERT_EQ(child_instance.exports[0].ref.type, WASM_COMP_RUNTIME_REF_COMPONENT);
    ASSERT_EQ(child_instance.exports[0].ref.of.component->component,
              component_inst->components[0].component);

    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestRuntimeBuildsNestedInlineComponentInstances)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "runtime-nested-inline-instances";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_TRUE(append_nested_component_inline_instance_sections(
        (WASMComponentModule *)module));

    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate(module, helper->stack_size, helper->heap_size,
                                 helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    auto *component_inst = (WASMComponentInstance *)module_inst;
    ASSERT_EQ(component_inst->component_instance_count, 2u);
    ASSERT_EQ(component_inst->component_export_count, 2u);

    const WASMComponentRuntimeInstance &nested_instance =
        component_inst->component_instances[1];
    ASSERT_EQ(nested_instance.export_count, 1u);
    ASSERT_EQ(nested_instance.owned_instance_count, 1u);
    ASSERT_EQ(std::string(nested_instance.exports[0].name), "wrapped-instance");
    ASSERT_EQ(nested_instance.exports[0].ref.type,
              WASM_COMP_RUNTIME_REF_INSTANCE);
    ASSERT_EQ(nested_instance.exports[0].ref.of.instance,
              &nested_instance.owned_instances[0]);

    const WASMComponentRuntimeInstance &child_instance =
        nested_instance.owned_instances[0];
    ASSERT_EQ(child_instance.export_count, 1u);
    ASSERT_EQ(std::string(child_instance.exports[0].name), "wrapped");
    ASSERT_EQ(child_instance.exports[0].ref.type, WASM_COMP_RUNTIME_REF_FUNC);
    ASSERT_EQ(child_instance.exports[0].ref.of.function,
              &component_inst->component_funcs[0]);

    const WASMComponentNamedExport &top_export =
        component_inst->component_exports[1];
    ASSERT_EQ(std::string(top_export.name), "nested-inline-instance");
    ASSERT_EQ(top_export.ref.type, WASM_COMP_RUNTIME_REF_INSTANCE);
    ASSERT_EQ(top_export.ref.of.instance, &component_inst->component_instances[1]);

    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}

TEST_F(BinaryParserTest, TestRuntimeBuildsNestedSubcomponentInstances)
{
    bool ret = helper->read_wasm_file("add.wasm");
    ASSERT_TRUE(ret);

    LoadArgs load_args = {};
    char module_name[] = "runtime-nested-subcomponent-instances";
    load_args.name = module_name;

    wasm_module_t module = wasm_runtime_load_ex(
        helper->component_raw, helper->wasm_file_size, &load_args,
        helper->error_buf, (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module, nullptr) << helper->error_buf;
    ASSERT_TRUE(append_nested_subcomponent_instance_sections(
        (WASMComponentModule *)module));

    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate(module, helper->stack_size, helper->heap_size,
                                 helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    auto *component_inst = (WASMComponentInstance *)module_inst;
    ASSERT_EQ(component_inst->component_instance_count, 2u);
    ASSERT_EQ(component_inst->component_export_count, 2u);

    const WASMComponentRuntimeInstance &nested_instance =
        component_inst->component_instances[1];
    ASSERT_EQ(nested_instance.export_count, 1u);
    ASSERT_EQ(nested_instance.owned_instance_count, 1u);
    ASSERT_EQ(std::string(nested_instance.exports[0].name), "wrapped-instance");
    ASSERT_EQ(nested_instance.exports[0].ref.type,
              WASM_COMP_RUNTIME_REF_INSTANCE);
    ASSERT_EQ(nested_instance.exports[0].ref.of.instance,
              &nested_instance.owned_instances[0]);

    const WASMComponentRuntimeInstance &child_instance =
        nested_instance.owned_instances[0];
    ASSERT_EQ(child_instance.export_count, 1u);
    ASSERT_EQ(std::string(child_instance.exports[0].name), "wrapped");
    ASSERT_EQ(child_instance.exports[0].ref.type, WASM_COMP_RUNTIME_REF_FUNC);
    ASSERT_EQ(child_instance.exports[0].ref.of.function,
              &component_inst->component_funcs[0]);

    const WASMComponentNamedExport &top_export =
        component_inst->component_exports[1];
    ASSERT_EQ(std::string(top_export.name), "nested-subcomponent-instance");
    ASSERT_EQ(top_export.ref.type, WASM_COMP_RUNTIME_REF_INSTANCE);
    ASSERT_EQ(top_export.ref.of.instance, &component_inst->component_instances[1]);

    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}
