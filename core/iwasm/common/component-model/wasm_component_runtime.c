/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wasm_component_runtime.h"
#include <string.h>

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
    return inst;
}

void
wasm_component_module_deinstantiate(WASMComponentInstance *inst)
{
    if (!inst)
        return;

    wasm_runtime_free(inst);
}
