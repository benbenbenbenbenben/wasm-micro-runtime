/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef WASM_COMPONENT_ASYNC_H
#define WASM_COMPONENT_ASYNC_H

#include "wasm_component.h"
#include "wasm_component_runtime.h"
#include "wasm_export.h"
#include "bh_platform.h"
#include "platform_api_extension.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WASM_COMPONENT_ASYNC_INVALID_TASK_ID UINT32_MAX
#define WASM_COMPONENT_ASYNC_INVALID_STREAM_ID UINT32_MAX
#define WASM_COMPONENT_ASYNC_INVALID_FUTURE_ID UINT32_MAX
#define WASM_COMPONENT_ASYNC_INVALID_WAITABLE_SET_ID UINT32_MAX
#define WASM_COMPONENT_ASYNC_INVALID_ERROR_CONTEXT_HANDLE UINT32_MAX
#define WASM_COMPONENT_ASYNC_INVALID_THREAD_ID UINT32_MAX

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
    uint32 *context_values;
    uint32 context_count;
    bool cancellation_requested;
} WASMComponentAsyncTask;

typedef struct WASMComponentAsyncStream {
    uint32 stream_id;
    bool readable_closed;
    bool writable_closed;
    uint8 *buffer;
    uint32 buffer_capacity;
    uint32 buffer_size;
    uint32 read_offset;
} WASMComponentAsyncStream;

typedef struct WASMComponentAsyncFuture {
    uint32 future_id;
    bool readable_closed;
    bool writable_closed;
    uint8 *value;
    uint32 value_size;
    bool value_present;
} WASMComponentAsyncFuture;

typedef struct WASMComponentAsyncErrorContext {
    uint32 handle;
    uint8 *message;
    uint32 message_len;
} WASMComponentAsyncErrorContext;

typedef struct WASMComponentAsyncWaitableSetItem {
    uint32 item_id;
    uint8 item_type; /* 0=task, 1=stream, 2=future */
} WASMComponentAsyncWaitableSetItem;

typedef struct WASMComponentAsyncWaitableSet {
    uint32 set_id;
    uint32 item_count;
    uint32 item_capacity;
    WASMComponentAsyncWaitableSetItem *items;
} WASMComponentAsyncWaitableSet;

typedef struct WASMComponentAsyncThreadEntry {
    uint32 thread_id;
    korp_tid os_thread;
    WASMExecEnv *exec_env;
    WASMComponentInstance *component_inst;
    WASMComponentRuntimeFunc *function;
    volatile bool running;
    volatile bool finished;
} WASMComponentAsyncThreadEntry;

typedef struct WASMComponentAsyncEngine {
    WASMComponentAsyncTask *tasks;
    uint32 task_capacity;
    uint32 task_count;
    uint32 next_task_id;
    uint32 *pending_queue;
    uint32 queue_head;
    uint32 queue_tail;
    uint32 queue_capacity;
    WASMComponentAsyncStream *streams;
    uint32 stream_capacity;
    uint32 stream_count;
    uint32 next_stream_id;
    WASMComponentAsyncFuture *futures;
    uint32 future_capacity;
    uint32 future_count;
    uint32 next_future_id;
    WASMComponentAsyncErrorContext *error_contexts;
    uint32 error_context_capacity;
    uint32 error_context_count;
    uint32 next_error_context_handle;
    WASMComponentAsyncWaitableSet *waitable_sets;
    uint32 waitable_set_capacity;
    uint32 waitable_set_count;
    uint32 next_waitable_set_id;
    bool dispatching_callback;
    bool backpressure_enabled;
    korp_mutex lock;
    WASMComponentAsyncThreadEntry *threads;
    uint32 thread_capacity;
    uint32 thread_count;
    uint32 next_thread_id;
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

bool
wasm_component_async_dispatch_callback(
    WASMComponentAsyncEngine *engine,
    WASMComponentInstance *inst,
    WASMComponentAsyncTask *task);

uint32
wasm_component_async_get_context_value(
    WASMComponentAsyncEngine *engine,
    uint32 ctx_idx);

void
wasm_component_async_set_context_value(
    WASMComponentAsyncEngine *engine,
    uint32 ctx_idx,
    uint32 value);

bool
wasm_component_async_is_task_cancelled(
    WASMComponentAsyncEngine *engine);

/* Stream operations */

uint32
wasm_component_async_stream_create(
    WASMComponentAsyncEngine *engine);

bool
wasm_component_async_stream_write(
    WASMComponentAsyncEngine *engine,
    uint32 stream_id,
    const uint8 *data,
    uint32 data_size);

uint32
wasm_component_async_stream_read(
    WASMComponentAsyncEngine *engine,
    uint32 stream_id,
    uint8 *buffer,
    uint32 buffer_size);

bool
wasm_component_async_stream_cancel_read(
    WASMComponentAsyncEngine *engine,
    uint32 stream_id);

bool
wasm_component_async_stream_cancel_write(
    WASMComponentAsyncEngine *engine,
    uint32 stream_id);

bool
wasm_component_async_stream_drop_readable(
    WASMComponentAsyncEngine *engine,
    uint32 stream_id);

bool
wasm_component_async_stream_drop_writable(
    WASMComponentAsyncEngine *engine,
    uint32 stream_id);

/* Future operations */

uint32
wasm_component_async_future_create(
    WASMComponentAsyncEngine *engine);

bool
wasm_component_async_future_write(
    WASMComponentAsyncEngine *engine,
    uint32 future_id,
    const uint8 *data,
    uint32 data_size);

uint32
wasm_component_async_future_read(
    WASMComponentAsyncEngine *engine,
    uint32 future_id,
    uint8 *buffer,
    uint32 buffer_size);

bool
wasm_component_async_future_cancel_read(
    WASMComponentAsyncEngine *engine,
    uint32 future_id);

bool
wasm_component_async_future_cancel_write(
    WASMComponentAsyncEngine *engine,
    uint32 future_id);

bool
wasm_component_async_future_drop_readable(
    WASMComponentAsyncEngine *engine,
    uint32 future_id);

bool
wasm_component_async_future_drop_writable(
    WASMComponentAsyncEngine *engine,
    uint32 future_id);

/* Error-context operations */

uint32
wasm_component_resource_create_error_context_handle(
    WASMComponentAsyncEngine *engine,
    const uint8 *message,
    uint32 message_len);

uint32
wasm_component_resource_read_error_context(
    WASMComponentAsyncEngine *engine,
    uint32 handle,
    uint8 *buffer,
    uint32 buffer_size);

void
wasm_component_resource_drop_error_context(
    WASMComponentAsyncEngine *engine,
    uint32 handle);

/* Waitable set operations */

uint32
wasm_component_async_waitable_set_create(
    WASMComponentAsyncEngine *engine);

uint32
wasm_component_async_waitable_set_wait(
    WASMComponentAsyncEngine *engine,
    uint32 set_id,
    uint32 timeout_ms);

uint32
wasm_component_async_waitable_set_poll(
    WASMComponentAsyncEngine *engine,
    uint32 set_id);

void
wasm_component_async_waitable_set_drop(
    WASMComponentAsyncEngine *engine,
    uint32 set_id);

bool
wasm_component_async_waitable_join(
    WASMComponentAsyncEngine *engine,
    uint32 set_id,
    uint32 waitable_idx,
    uint32 waitable_id);

/* Thread operations */

uint32
wasm_component_async_spawn_thread(
    WASMComponentAsyncEngine *engine,
    WASMComponentInstance *component_inst,
    WASMComponentRuntimeFunc *function);

bool
wasm_component_async_join_thread(
    WASMComponentAsyncEngine *engine,
    uint32 thread_id);

bool
wasm_component_async_detach_thread(
    WASMComponentAsyncEngine *engine,
    uint32 thread_id);

#ifdef __cplusplus
}
#endif

#endif /* WASM_COMPONENT_ASYNC_H */
