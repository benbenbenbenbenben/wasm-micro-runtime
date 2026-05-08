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
        wasm_runtime_instantiate(module, helper->stack_size, helper->heap_size,
                                 helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
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
        wasm_runtime_instantiate(module, helper->stack_size, helper->heap_size,
                                 helper->error_buf,
                                 (uint32_t)sizeof(helper->error_buf));
    ASSERT_NE(module_inst, nullptr) << helper->error_buf;

    wasm_function_inst_t func = wasm_runtime_lookup_function(module_inst, "main");
    ASSERT_EQ(func, nullptr);
    ASSERT_STREQ(wasm_runtime_get_exception(module_inst),
                 "component function lookup is not supported yet");

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
