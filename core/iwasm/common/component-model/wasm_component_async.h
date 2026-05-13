/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef WASM_COMPONENT_ASYNC_H
#define WASM_COMPONENT_ASYNC_H

#include "wasm_component.h"
#include "wasm_component_runtime.h"
#include "wasm_export.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WASM_COMPONENT_ASYNC_INVALID_TASK_ID UINT32_MAX

typedef enum WASMComponentAsyncTaskState {
    WASM_COMP_ASYNC_TASK_PENDING = 0,
    WASM_COMP_ASYNC_TASK_RUNNING,
    WASM_COMP_ASYNC_TASK_COMPLETED,
    WASM_COMP_ASYNC_TASK_CANCELLED,
    WASM_COMP_ASYNC_TASK_FAILED
} WASMComponentAsyncTaskState;

typedef struct WASMComponentAsyncTask {
    uint32 task_id;
    WASMComponentAsyncTaskState state;
    WASMComponentRuntimeFunc *function;
    wasm_component_value_t *args;
    uint32 num_args;
    wasm_component_value_t *results;
    uint32 num_results;
    bool owns_args;
    bool owns_results;
} WASMComponentAsyncTask;

typedef struct WASMComponentAsyncEngine {
    WASMComponentAsyncTask *tasks;
    uint32 task_capacity;
    uint32 task_count;
    uint32 next_task_id;
    uint32 *pending_queue;
    uint32 queue_head;
    uint32 queue_tail;
    uint32 queue_capacity;
} WASMComponentAsyncEngine;

bool
wasm_component_async_engine_create(
    WASMComponentAsyncEngine **engine_out,
    uint32 initial_task_capacity);

void
wasm_component_async_engine_destroy(
    WASMComponentAsyncEngine *engine);

uint32
wasm_component_async_create_task(
    WASMComponentAsyncEngine *engine,
    WASMComponentRuntimeFunc *function,
    wasm_component_value_t *args,
    uint32 num_args,
    uint32 num_results);

bool
wasm_component_async_poll_task(
    WASMComponentAsyncEngine *engine,
    WASMComponentInstance *inst,
    uint32 *completed_task_id_out);

bool
wasm_component_async_wait_task(
    WASMComponentAsyncEngine *engine,
    WASMComponentInstance *inst,
    uint32 task_id);

bool
wasm_component_async_cancel_task(
    WASMComponentAsyncEngine *engine,
    uint32 task_id);

bool
wasm_component_async_get_result(
    WASMComponentAsyncEngine *engine,
    uint32 task_id,
    wasm_component_value_t *results,
    uint32 num_results);

#ifdef __cplusplus
}
#endif

#endif /* WASM_COMPONENT_ASYNC_H */
