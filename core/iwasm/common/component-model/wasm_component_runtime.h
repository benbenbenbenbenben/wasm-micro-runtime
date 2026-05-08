/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef WASM_COMPONENT_RUNTIME_H
#define WASM_COMPONENT_RUNTIME_H

#include "wasm_component.h"

typedef struct WASMComponentModule {
    uint32 module_type;
    WASMComponent component;
    uint8 *binary;
    uint32 binary_size;
    bool is_binary_freeable;
} WASMComponentModule;

struct WASMComponentInstance {
    uint32 module_type;
    WASMComponentModule *module;
    char cur_exception[128];
};

WASMComponentModule *
wasm_component_module_load(uint8 *buf, uint32 size, const LoadArgs *args,
                           char *error_buf, uint32 error_buf_size);

void
wasm_component_module_unload(WASMComponentModule *module);

WASMComponentInstance *
wasm_component_module_instantiate(WASMComponentModule *module,
                                  char *error_buf, uint32 error_buf_size);

void
wasm_component_module_deinstantiate(WASMComponentInstance *inst);

#endif
