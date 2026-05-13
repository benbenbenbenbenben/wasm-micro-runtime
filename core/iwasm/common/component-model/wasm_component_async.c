/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wasm_component_async.h"
#include "wasm_component_runtime.h"
#include "wasm_component_value.h"
#include "wasm_export.h"
#include <string.h>

bool
wasm_component_async_engine_create(
    WASMComponentAsyncEngine **engine_out,
    uint32 initial_task_capacity)
{
    WASMComponentAsyncEngine *engine;

    if (!engine_out)
        return false;

    if (initial_task_capacity < 4)
        initial_task_capacity = 4;

    engine = wasm_runtime_malloc(sizeof(WASMComponentAsyncEngine));
    if (!engine)
        return false;

    memset(engine, 0, sizeof(*engine));
    engine->task_capacity = initial_task_capacity;
    engine->tasks = wasm_runtime_malloc(
        sizeof(WASMComponentAsyncTask) * initial_task_capacity);
    if (!engine->tasks) {
        wasm_runtime_free(engine);
        return false;
    }
    memset(engine->tasks, 0,
           sizeof(WASMComponentAsyncTask) * initial_task_capacity);

    engine->queue_capacity = initial_task_capacity;
    engine->pending_queue = wasm_runtime_malloc(
        sizeof(uint32) * initial_task_capacity);
    if (!engine->pending_queue) {
        wasm_runtime_free(engine->tasks);
        wasm_runtime_free(engine);
        return false;
    }
    memset(engine->pending_queue, 0, sizeof(uint32) * initial_task_capacity);

    engine->stream_capacity = 4;
    engine->streams = wasm_runtime_malloc(
        sizeof(WASMComponentAsyncStream) * engine->stream_capacity);
    if (!engine->streams) {
        wasm_runtime_free(engine->pending_queue);
        wasm_runtime_free(engine->tasks);
        wasm_runtime_free(engine);
        return false;
    }
    memset(engine->streams, 0,
           sizeof(WASMComponentAsyncStream) * engine->stream_capacity);

    engine->future_capacity = 4;
    engine->futures = wasm_runtime_malloc(
        sizeof(WASMComponentAsyncFuture) * engine->future_capacity);
    if (!engine->futures) {
        wasm_runtime_free(engine->streams);
        wasm_runtime_free(engine->pending_queue);
        wasm_runtime_free(engine->tasks);
        wasm_runtime_free(engine);
        return false;
    }
    memset(engine->futures, 0,
           sizeof(WASMComponentAsyncFuture) * engine->future_capacity);

    engine->error_context_capacity = 4;
    engine->error_contexts = wasm_runtime_malloc(
        sizeof(WASMComponentAsyncErrorContext) * engine->error_context_capacity);
    if (!engine->error_contexts) {
        wasm_runtime_free(engine->futures);
        wasm_runtime_free(engine->streams);
        wasm_runtime_free(engine->pending_queue);
        wasm_runtime_free(engine->tasks);
        wasm_runtime_free(engine);
        return false;
    }
    memset(engine->error_contexts, 0,
           sizeof(WASMComponentAsyncErrorContext) * engine->error_context_capacity);

    engine->waitable_set_capacity = 4;
    engine->waitable_sets = wasm_runtime_malloc(
        sizeof(WASMComponentAsyncWaitableSet) * engine->waitable_set_capacity);
    if (!engine->waitable_sets) {
        wasm_runtime_free(engine->error_contexts);
        wasm_runtime_free(engine->futures);
        wasm_runtime_free(engine->streams);
        wasm_runtime_free(engine->pending_queue);
        wasm_runtime_free(engine->tasks);
        wasm_runtime_free(engine);
        return false;
    }
    memset(engine->waitable_sets, 0,
           sizeof(WASMComponentAsyncWaitableSet) * engine->waitable_set_capacity);

    engine->next_task_id = 1;
    engine->next_stream_id = 1;
    engine->next_future_id = 1;
    engine->next_error_context_handle = 1;
    engine->next_waitable_set_id = 1;
    *engine_out = engine;
    return true;
}

void
wasm_component_async_engine_destroy(
    WASMComponentAsyncEngine *engine)
{
    uint32 i;

    if (!engine)
        return;

    for (i = 0; i < engine->task_count; i++) {
        WASMComponentAsyncTask *task = &engine->tasks[i];
        if (task->task_id == 0)
            continue;
        if (task->args && task->owns_args) {
            for (uint32 j = 0; j < task->num_args; j++)
                wasm_component_value_destroy(&task->args[j]);
            wasm_runtime_free(task->args);
        }
        if (task->results && task->owns_results) {
            for (uint32 j = 0; j < task->num_results; j++)
                wasm_component_value_destroy(&task->results[j]);
            wasm_runtime_free(task->results);
        }
        if (task->context_values)
            wasm_runtime_free(task->context_values);
    }

    if (engine->tasks)
        wasm_runtime_free(engine->tasks);

    for (i = 0; i < engine->stream_count; i++) {
        WASMComponentAsyncStream *s = &engine->streams[i];
        if (s->stream_id != 0 && s->buffer)
            wasm_runtime_free(s->buffer);
    }
    if (engine->streams)
        wasm_runtime_free(engine->streams);

    for (i = 0; i < engine->future_count; i++) {
        WASMComponentAsyncFuture *f = &engine->futures[i];
        if (f->future_id != 0 && f->value)
            wasm_runtime_free(f->value);
    }
    if (engine->futures)
        wasm_runtime_free(engine->futures);

    for (i = 0; i < engine->error_context_count; i++) {
        WASMComponentAsyncErrorContext *ec = &engine->error_contexts[i];
        if (ec->handle != 0 && ec->message)
            wasm_runtime_free(ec->message);
    }
    if (engine->error_contexts)
        wasm_runtime_free(engine->error_contexts);

    for (i = 0; i < engine->waitable_set_count; i++) {
        WASMComponentAsyncWaitableSet *ws = &engine->waitable_sets[i];
        if (ws->set_id != 0 && ws->items)
            wasm_runtime_free(ws->items);
    }
    if (engine->waitable_sets)
        wasm_runtime_free(engine->waitable_sets);

    if (engine->pending_queue)
        wasm_runtime_free(engine->pending_queue);
    wasm_runtime_free(engine);
}

static bool
enqueue_pending_task(WASMComponentAsyncEngine *engine, uint32 task_id)
{
    uint32 new_capacity;

    if (engine->queue_tail - engine->queue_head >= engine->queue_capacity) {
        new_capacity = engine->queue_capacity * 2;
        uint32 *new_queue = wasm_runtime_realloc(
            engine->pending_queue, sizeof(uint32) * new_capacity);
        if (!new_queue)
            return false;
        engine->pending_queue = new_queue;
        engine->queue_capacity = new_capacity;
    }

    engine->pending_queue[engine->queue_tail %
                          engine->queue_capacity] = task_id;
    engine->queue_tail++;
    return true;
}

static uint32
dequeue_pending_task(WASMComponentAsyncEngine *engine)
{
    uint32 task_id;

    if (engine->queue_head >= engine->queue_tail)
        return WASM_COMPONENT_ASYNC_INVALID_TASK_ID;

    task_id = engine->pending_queue[engine->queue_head %
                                     engine->queue_capacity];
    engine->queue_head++;
    return task_id;
}

static int32
find_task_index(WASMComponentAsyncEngine *engine, uint32 task_id)
{
    for (uint32 i = 0; i < engine->task_count; i++) {
        if (engine->tasks[i].task_id == task_id)
            return (int32)i;
    }
    return -1;
}

uint32
wasm_component_async_create_task(
    WASMComponentAsyncEngine *engine,
    WASMComponentRuntimeFunc *function,
    wasm_component_value_t *args,
    uint32 num_args,
    uint32 num_results)
{
    uint32 task_id;
    int32 task_idx;
    WASMComponentAsyncTask *task;

    if (!engine || !function)
        return WASM_COMPONENT_ASYNC_INVALID_TASK_ID;

    task_id = engine->next_task_id++;
    if (task_id == 0)
        task_id = engine->next_task_id++;

    /* Find a free slot or grow */
    task_idx = find_task_index(engine, 0);
    if (task_idx < 0) {
        uint32 new_capacity = engine->task_capacity * 2;
        WASMComponentAsyncTask *new_tasks = wasm_runtime_realloc(
            engine->tasks, sizeof(WASMComponentAsyncTask) * new_capacity);
        if (!new_tasks)
            return WASM_COMPONENT_ASYNC_INVALID_TASK_ID;
        engine->tasks = new_tasks;
        memset(&engine->tasks[engine->task_capacity], 0,
               sizeof(WASMComponentAsyncTask) * (new_capacity - engine->task_capacity));
        engine->task_capacity = new_capacity;
        task_idx = (int32)engine->task_count;
    }

    task = &engine->tasks[task_idx];
    memset(task, 0, sizeof(*task));
    task->task_id = task_id;
    task->state = WASM_COMP_ASYNC_TASK_PENDING;
    task->function = function;
    task->num_args = num_args;
    task->num_results = num_results;

    task->context_values = wasm_runtime_malloc(sizeof(uint32) * 4);
    if (task->context_values) {
        memset(task->context_values, 0, sizeof(uint32) * 4);
        task->context_count = 4;
    }

    if (args && num_args > 0) {
        task->args = wasm_runtime_malloc(
            sizeof(wasm_component_value_t) * num_args);
        if (task->args) {
            memcpy(task->args, args,
                   sizeof(wasm_component_value_t) * num_args);
            task->owns_args = true;
        }
    }

    task->results = wasm_runtime_malloc(
        sizeof(wasm_component_value_t) * num_results);
    if (task->results) {
        memset(task->results, 0,
               sizeof(wasm_component_value_t) * num_results);
        task->owns_results = true;
    }

    engine->task_count++;
    enqueue_pending_task(engine, task_id);
    return task_id;
}

bool
wasm_component_async_poll_task(
    WASMComponentAsyncEngine *engine,
    WASMComponentInstance *inst,
    uint32 *completed_task_id_out)
{
    uint32 task_id;
    int32 task_idx;
    WASMComponentAsyncTask *task;
    bool call_ok;

    if (!engine)
        return false;

    task_id = dequeue_pending_task(engine);
    if (task_id == WASM_COMPONENT_ASYNC_INVALID_TASK_ID)
        return true;

    task_idx = find_task_index(engine, task_id);
    if (task_idx < 0)
        return true;

    task = &engine->tasks[task_idx];
    task->state = WASM_COMP_ASYNC_TASK_RUNNING;
    engine->current_task_id = task_id;

    call_ok = wasm_runtime_call_component_values(
        (wasm_module_inst_t)inst, (wasm_component_func_t)task->function,
        task->num_results, task->results,
        task->num_args, task->args);

    engine->current_task_id = UINT32_MAX;

    task->state = call_ok ? WASM_COMP_ASYNC_TASK_COMPLETED
                          : WASM_COMP_ASYNC_TASK_FAILED;

    if (call_ok && !engine->dispatching_callback
        && task->function
        && task->function->callback_func_idx != UINT32_MAX
        && task->function->callback_func_idx < inst->core_func_count) {
        engine->dispatching_callback = true;
        wasm_component_async_dispatch_callback(engine, inst, task);
        engine->dispatching_callback = false;
    }

    if (completed_task_id_out)
        *completed_task_id_out = task_id;

    return true;
}

bool
wasm_component_async_wait_task(
    WASMComponentAsyncEngine *engine,
    WASMComponentInstance *inst,
    uint32 task_id)
{
    /* In this implementation, tasks execute synchronously on poll.
       Wait is a no-op if the task is already completed. */
    int32 task_idx = find_task_index(engine, task_id);
    if (task_idx < 0)
        return false;

    if (engine->tasks[task_idx].state == WASM_COMP_ASYNC_TASK_COMPLETED)
        return true;

    /* Poll until the task completes */
    while (engine->tasks[task_idx].state == WASM_COMP_ASYNC_TASK_PENDING
           || engine->tasks[task_idx].state == WASM_COMP_ASYNC_TASK_RUNNING) {
        uint32 completed_id;
        if (!wasm_component_async_poll_task(engine, inst, &completed_id))
            return false;
        if (completed_id == task_id)
            return true;
    }

    return engine->tasks[task_idx].state == WASM_COMP_ASYNC_TASK_COMPLETED;
}

bool
wasm_component_async_cancel_task(
    WASMComponentAsyncEngine *engine,
    uint32 task_id)
{
    int32 task_idx;

    if (!engine)
        return false;

    task_idx = find_task_index(engine, task_id);
    if (task_idx < 0)
        return false;

    if (engine->tasks[task_idx].state == WASM_COMP_ASYNC_TASK_COMPLETED
        || engine->tasks[task_idx].state == WASM_COMP_ASYNC_TASK_CANCELLED
        || engine->tasks[task_idx].state == WASM_COMP_ASYNC_TASK_FAILED)
        return false;

    if (engine->tasks[task_idx].state == WASM_COMP_ASYNC_TASK_RUNNING) {
        engine->tasks[task_idx].cancellation_requested = true;
        return true;
    }

    engine->tasks[task_idx].state = WASM_COMP_ASYNC_TASK_CANCELLED;
    return true;
}

bool
wasm_component_async_get_result(
    WASMComponentAsyncEngine *engine,
    uint32 task_id,
    wasm_component_value_t *results,
    uint32 num_results)
{
    int32 task_idx;
    WASMComponentAsyncTask *task;

    if (!engine || !results)
        return false;

    task_idx = find_task_index(engine, task_id);
    if (task_idx < 0)
        return false;

    task = &engine->tasks[task_idx];
    if (task->state != WASM_COMP_ASYNC_TASK_COMPLETED)
        return false;

    if (num_results > task->num_results)
        num_results = task->num_results;

    for (uint32 i = 0; i < num_results; i++)
        results[i] = task->results[i];

    return true;
}

static int32
find_stream_index(WASMComponentAsyncEngine *engine, uint32 stream_id)
{
    if (stream_id == 0) return -1;
    for (uint32 i = 0; i < engine->stream_count; i++)
        if (engine->streams[i].stream_id == stream_id)
            return (int32)i;
    return -1;
}

static int32
find_future_index(WASMComponentAsyncEngine *engine, uint32 future_id)
{
    if (future_id == 0) return -1;
    for (uint32 i = 0; i < engine->future_count; i++)
        if (engine->futures[i].future_id == future_id)
            return (int32)i;
    return -1;
}

uint32
wasm_component_async_stream_create(
    WASMComponentAsyncEngine *engine)
{
    int32 idx;
    WASMComponentAsyncStream *s;

    if (!engine)
        return WASM_COMPONENT_ASYNC_INVALID_STREAM_ID;

    idx = find_stream_index(engine, 0);
    if (idx < 0) {
        uint32 new_cap = engine->stream_capacity * 2;
        WASMComponentAsyncStream *new_s = wasm_runtime_realloc(
            engine->streams, sizeof(WASMComponentAsyncStream) * new_cap);
        if (!new_s)
            return WASM_COMPONENT_ASYNC_INVALID_STREAM_ID;
        engine->streams = new_s;
        memset(&engine->streams[engine->stream_capacity], 0,
               sizeof(WASMComponentAsyncStream) * (new_cap - engine->stream_capacity));
        engine->stream_capacity = new_cap;
        idx = (int32)engine->stream_count;
    }

    s = &engine->streams[idx];
    memset(s, 0, sizeof(*s));
    s->stream_id = engine->next_stream_id++;
    s->buffer_capacity = 4096;
    s->buffer = wasm_runtime_malloc(s->buffer_capacity);
    if (!s->buffer)
        return WASM_COMPONENT_ASYNC_INVALID_STREAM_ID;
    engine->stream_count++;
    return s->stream_id;
}

bool
wasm_component_async_stream_write(
    WASMComponentAsyncEngine *engine,
    uint32 stream_id, const uint8 *data, uint32 data_size)
{
    int32 idx = find_stream_index(engine, stream_id);
    WASMComponentAsyncStream *s;
    uint32 new_size, new_cap;
    uint8 *new_buf;

    if (idx < 0) return false;
    s = &engine->streams[idx];
    if (s->writable_closed) return false;
    if (data_size == 0) return true;

    new_size = s->buffer_size + data_size;
    if (new_size > s->buffer_capacity) {
        new_cap = s->buffer_capacity * 2;
        while (new_cap < new_size) new_cap *= 2;
        new_buf = wasm_runtime_realloc(s->buffer, new_cap);
        if (!new_buf) return false;
        s->buffer = new_buf;
        s->buffer_capacity = new_cap;
    }

    memcpy(s->buffer + s->buffer_size, data, data_size);
    s->buffer_size = new_size;
    return true;
}

uint32
wasm_component_async_stream_read(
    WASMComponentAsyncEngine *engine,
    uint32 stream_id, uint8 *buffer, uint32 buffer_size)
{
    int32 idx = find_stream_index(engine, stream_id);
    WASMComponentAsyncStream *s;
    uint32 available, to_read;

    if (idx < 0) return 0;
    s = &engine->streams[idx];
    if (s->read_offset >= s->buffer_size) return 0;

    available = s->buffer_size - s->read_offset;
    to_read = buffer_size < available ? buffer_size : available;
    memcpy(buffer, s->buffer + s->read_offset, to_read);
    s->read_offset += to_read;

    if (s->read_offset == s->buffer_size) {
        s->buffer_size = 0;
        s->read_offset = 0;
    }
    return to_read;
}

bool
wasm_component_async_stream_cancel_read(
    WASMComponentAsyncEngine *engine, uint32 stream_id)
{
    (void)engine; (void)stream_id;
    return true;
}

bool
wasm_component_async_stream_cancel_write(
    WASMComponentAsyncEngine *engine, uint32 stream_id)
{
    (void)engine; (void)stream_id;
    return true;
}

bool
wasm_component_async_stream_drop_readable(
    WASMComponentAsyncEngine *engine, uint32 stream_id)
{
    int32 idx = find_stream_index(engine, stream_id);
    if (idx < 0) return false;
    engine->streams[idx].readable_closed = true;
    return true;
}

bool
wasm_component_async_stream_drop_writable(
    WASMComponentAsyncEngine *engine, uint32 stream_id)
{
    int32 idx = find_stream_index(engine, stream_id);
    if (idx < 0) return false;
    engine->streams[idx].writable_closed = true;
    return true;
}

uint32
wasm_component_async_future_create(
    WASMComponentAsyncEngine *engine)
{
    int32 idx;
    WASMComponentAsyncFuture *f;

    if (!engine)
        return WASM_COMPONENT_ASYNC_INVALID_FUTURE_ID;

    idx = find_future_index(engine, 0);
    if (idx < 0) {
        uint32 new_cap = engine->future_capacity * 2;
        WASMComponentAsyncFuture *new_f = wasm_runtime_realloc(
            engine->futures, sizeof(WASMComponentAsyncFuture) * new_cap);
        if (!new_f)
            return WASM_COMPONENT_ASYNC_INVALID_FUTURE_ID;
        engine->futures = new_f;
        memset(&engine->futures[engine->future_capacity], 0,
               sizeof(WASMComponentAsyncFuture) * (new_cap - engine->future_capacity));
        engine->future_capacity = new_cap;
        idx = (int32)engine->future_count;
    }

    f = &engine->futures[idx];
    memset(f, 0, sizeof(*f));
    f->future_id = engine->next_future_id++;
    engine->future_count++;
    return f->future_id;
}

bool
wasm_component_async_future_write(
    WASMComponentAsyncEngine *engine,
    uint32 future_id, const uint8 *data, uint32 data_size)
{
    int32 idx = find_future_index(engine, future_id);
    WASMComponentAsyncFuture *f;

    if (idx < 0) return false;
    f = &engine->futures[idx];
    if (f->writable_closed || f->value_present) return false;

    if (data_size > 0) {
        f->value = wasm_runtime_malloc(data_size);
        if (!f->value) return false;
        memcpy(f->value, data, data_size);
        f->value_size = data_size;
    }
    f->value_present = true;
    return true;
}

uint32
wasm_component_async_future_read(
    WASMComponentAsyncEngine *engine,
    uint32 future_id, uint8 *buffer, uint32 buffer_size)
{
    int32 idx = find_future_index(engine, future_id);
    WASMComponentAsyncFuture *f;
    uint32 to_read;

    if (idx < 0) return 0;
    f = &engine->futures[idx];
    if (!f->value_present || f->readable_closed) return 0;

    to_read = buffer_size < f->value_size ? buffer_size : f->value_size;
    memcpy(buffer, f->value, to_read);
    return to_read;
}

bool
wasm_component_async_future_cancel_read(
    WASMComponentAsyncEngine *engine, uint32 future_id)
{
    (void)engine; (void)future_id;
    return true;
}

bool
wasm_component_async_future_cancel_write(
    WASMComponentAsyncEngine *engine, uint32 future_id)
{
    (void)engine; (void)future_id;
    return true;
}

bool
wasm_component_async_future_drop_readable(
    WASMComponentAsyncEngine *engine, uint32 future_id)
{
    int32 idx = find_future_index(engine, future_id);
    if (idx < 0) return false;
    engine->futures[idx].readable_closed = true;
    return true;
}

bool
wasm_component_async_future_drop_writable(
    WASMComponentAsyncEngine *engine, uint32 future_id)
{
    int32 idx = find_future_index(engine, future_id);
    if (idx < 0) return false;
    engine->futures[idx].writable_closed = true;
    return true;
}

/* Error-context helpers */

static int32
find_error_context_index(WASMComponentAsyncEngine *engine, uint32 handle)
{
    if (handle == 0) return -1;
    for (uint32 i = 0; i < engine->error_context_count; i++)
        if (engine->error_contexts[i].handle == handle)
            return (int32)i;
    return -1;
}

uint32
wasm_component_resource_create_error_context_handle(
    WASMComponentAsyncEngine *engine, const uint8 *message, uint32 message_len)
{
    int32 idx;
    WASMComponentAsyncErrorContext *ec;

    idx = find_error_context_index(engine, 0);
    if (idx < 0) {
        uint32 new_cap = engine->error_context_capacity * 2;
        WASMComponentAsyncErrorContext *new_ec = wasm_runtime_realloc(
            engine->error_contexts,
            sizeof(WASMComponentAsyncErrorContext) * new_cap);
        if (!new_ec)
            return WASM_COMPONENT_ASYNC_INVALID_ERROR_CONTEXT_HANDLE;
        engine->error_contexts = new_ec;
        memset(&engine->error_contexts[engine->error_context_capacity], 0,
               sizeof(WASMComponentAsyncErrorContext)
                   * (new_cap - engine->error_context_capacity));
        engine->error_context_capacity = new_cap;
        idx = (int32)engine->error_context_count;
    }

    ec = &engine->error_contexts[idx];
    memset(ec, 0, sizeof(*ec));
    ec->handle = engine->next_error_context_handle++;
    if (message_len > 0 && message) {
        ec->message = wasm_runtime_malloc(message_len);
        if (ec->message) {
            memcpy(ec->message, message, message_len);
            ec->message_len = message_len;
        }
    }
    engine->error_context_count++;
    return ec->handle;
}

uint32
wasm_component_resource_read_error_context(
    WASMComponentAsyncEngine *engine, uint32 handle,
    uint8 *buffer, uint32 buffer_size)
{
    int32 idx = find_error_context_index(engine, handle);
    WASMComponentAsyncErrorContext *ec;
    uint32 to_copy;

    if (idx < 0) return 0;
    ec = &engine->error_contexts[idx];
    if (!ec->message || ec->message_len == 0) return 0;

    to_copy = buffer_size < ec->message_len ? buffer_size : ec->message_len;
    if (buffer && to_copy > 0)
        memcpy(buffer, ec->message, to_copy);
    return ec->message_len;
}

void
wasm_component_resource_drop_error_context(
    WASMComponentAsyncEngine *engine, uint32 handle)
{
    int32 idx = find_error_context_index(engine, handle);
    WASMComponentAsyncErrorContext *ec;
    if (idx < 0) return;
    ec = &engine->error_contexts[idx];
    if (ec->message)
        wasm_runtime_free(ec->message);
    memset(ec, 0, sizeof(*ec));
}

/* Waitable set helpers */

static int32
find_waitable_set_index(WASMComponentAsyncEngine *engine, uint32 set_id)
{
    if (set_id == 0) return -1;
    for (uint32 i = 0; i < engine->waitable_set_count; i++)
        if (engine->waitable_sets[i].set_id == set_id)
            return (int32)i;
    return -1;
}

uint32
wasm_component_async_waitable_set_create(
    WASMComponentAsyncEngine *engine)
{
    int32 idx;
    WASMComponentAsyncWaitableSet *ws;

    if (!engine)
        return WASM_COMPONENT_ASYNC_INVALID_WAITABLE_SET_ID;

    idx = find_waitable_set_index(engine, 0);
    if (idx < 0) {
        uint32 new_cap = engine->waitable_set_capacity * 2;
        WASMComponentAsyncWaitableSet *new_ws = wasm_runtime_realloc(
            engine->waitable_sets,
            sizeof(WASMComponentAsyncWaitableSet) * new_cap);
        if (!new_ws)
            return WASM_COMPONENT_ASYNC_INVALID_WAITABLE_SET_ID;
        engine->waitable_sets = new_ws;
        memset(&engine->waitable_sets[engine->waitable_set_capacity], 0,
               sizeof(WASMComponentAsyncWaitableSet)
                   * (new_cap - engine->waitable_set_capacity));
        engine->waitable_set_capacity = new_cap;
        idx = (int32)engine->waitable_set_count;
    }

    ws = &engine->waitable_sets[idx];
    memset(ws, 0, sizeof(*ws));
    ws->set_id = engine->next_waitable_set_id++;
    ws->item_capacity = 4;
    ws->items = wasm_runtime_malloc(
        sizeof(WASMComponentAsyncWaitableSetItem) * ws->item_capacity);
    if (!ws->items)
        return WASM_COMPONENT_ASYNC_INVALID_WAITABLE_SET_ID;
    engine->waitable_set_count++;
    return ws->set_id;
}

uint32
wasm_component_async_waitable_set_wait(
    WASMComponentAsyncEngine *engine, uint32 set_id, uint32 timeout_ms)
{
    int32 idx = find_waitable_set_index(engine, set_id);
    WASMComponentAsyncWaitableSet *ws;
    uint32 ready_mask = 0;

    if (idx < 0) return 0;
    ws = &engine->waitable_sets[idx];

    for (uint32 i = 0; i < ws->item_count; i++) {
        WASMComponentAsyncWaitableSetItem *item = &ws->items[i];
        bool ready = false;
        switch (item->item_type) {
            case 0: /* task */
            {
                for (uint32 t = 0; t < engine->task_count; t++) {
                    if (engine->tasks[t].task_id == item->item_id) {
                        ready = (engine->tasks[t].state
                                 == WASM_COMP_ASYNC_TASK_COMPLETED
                                 || engine->tasks[t].state
                                        == WASM_COMP_ASYNC_TASK_CANCELLED
                                 || engine->tasks[t].state
                                        == WASM_COMP_ASYNC_TASK_FAILED);
                        break;
                    }
                }
                break;
            }
            case 1: /* stream — always ready if data available */
            {
                for (uint32 s = 0; s < engine->stream_count; s++) {
                    if (engine->streams[s].stream_id == item->item_id) {
                        ready = (engine->streams[s].buffer_size
                                 > engine->streams[s].read_offset);
                        break;
                    }
                }
                break;
            }
            case 2: /* future — ready if value present */
            {
                for (uint32 f = 0; f < engine->future_count; f++) {
                    if (engine->futures[f].future_id == item->item_id) {
                        ready = engine->futures[f].value_present;
                        break;
                    }
                }
                break;
            }
        }
        if (ready)
            ready_mask |= (uint32)(1u << i);
    }

    (void)timeout_ms;
    return ready_mask;
}

uint32
wasm_component_async_waitable_set_poll(
    WASMComponentAsyncEngine *engine, uint32 set_id)
{
    return wasm_component_async_waitable_set_wait(engine, set_id, 0);
}

void
wasm_component_async_waitable_set_drop(
    WASMComponentAsyncEngine *engine, uint32 set_id)
{
    int32 idx = find_waitable_set_index(engine, set_id);
    WASMComponentAsyncWaitableSet *ws;
    if (idx < 0) return;
    ws = &engine->waitable_sets[idx];
    if (ws->items)
        wasm_runtime_free(ws->items);
    memset(ws, 0, sizeof(*ws));
}

bool
wasm_component_async_waitable_join(
    WASMComponentAsyncEngine *engine,
    uint32 set_id, uint32 waitable_idx, uint32 waitable_id)
{
    int32 idx = find_waitable_set_index(engine, set_id);
    WASMComponentAsyncWaitableSet *ws;
    WASMComponentAsyncWaitableSetItem *item;

    if (idx < 0) return false;
    ws = &engine->waitable_sets[idx];

    if (waitable_idx >= ws->item_capacity) {
        uint32 new_cap = ws->item_capacity * 2;
        WASMComponentAsyncWaitableSetItem *new_items = wasm_runtime_realloc(
            ws->items, sizeof(WASMComponentAsyncWaitableSetItem) * new_cap);
        if (!new_items) return false;
        ws->items = new_items;
        ws->item_capacity = new_cap;
    }
    if (waitable_idx >= ws->item_count)
        ws->item_count = waitable_idx + 1;

    item = &ws->items[waitable_idx];
    item->item_id = waitable_id;
    /* Detect waitable type from the ID */
    item->item_type = 0;
    for (uint32 i = 0; i < engine->task_count; i++) {
        if (engine->tasks[i].task_id == waitable_id) {
            item->item_type = 0;
            goto join_done;
        }
    }
    for (uint32 i = 0; i < engine->stream_count; i++) {
        if (engine->streams[i].stream_id == waitable_id) {
            item->item_type = 1;
            goto join_done;
        }
    }
    for (uint32 i = 0; i < engine->future_count; i++) {
        if (engine->futures[i].future_id == waitable_id) {
            item->item_type = 2;
            goto join_done;
        }
    }
join_done:
    return true;
}

bool
wasm_component_async_dispatch_callback(
    WASMComponentAsyncEngine *engine,
    WASMComponentInstance *inst,
    WASMComponentAsyncTask *task)
{
    WASMComponentCoreRuntimeRef *cb_ref;
    WASMModuleInstanceCommon *target_module_inst;
    WASMExecEnv *exec_env;
    wasm_val_t cb_args[16];
    uint32 num_cb_args = 0;
    bool ok;

    (void)engine;

    if (!inst || !task || !task->function)
        return false;

    if (task->function->callback_func_idx == UINT32_MAX
        || task->function->callback_func_idx >= inst->core_func_count)
        return false;

    cb_ref = &inst->core_funcs[task->function->callback_func_idx];
    if (cb_ref->type != WASM_COMP_CORE_RUNTIME_REF_FUNC || !cb_ref->of.function)
        return false;

    cb_args[num_cb_args].kind = WASM_I32;
    cb_args[num_cb_args].of.i32 = (int32)task->task_id;
    num_cb_args++;

    for (uint32 i = 0; i < task->num_results && num_cb_args < 16; i++) {
        const uint8 *data =
            wasm_component_value_get_data(&task->results[i]);
        if (data
            && task->results[i].type.kind
                   == WASM_COMPONENT_VALUE_TYPE_PRIMITIVE) {
            cb_args[num_cb_args].kind = WASM_I32;
            cb_args[num_cb_args].of.i32 = (int32) * (const uint32 *)data;
            num_cb_args++;
        }
    }

    target_module_inst =
        (WASMModuleInstanceCommon *)cb_ref->owner_instance->module_inst;
    exec_env = wasm_runtime_get_exec_env_tls();
    if (!exec_env)
        exec_env = wasm_runtime_get_exec_env_singleton(target_module_inst);
    if (!exec_env)
        return false;

    wasm_runtime_clear_exception(target_module_inst);
    ok = wasm_runtime_call_wasm_a(exec_env, cb_ref->of.function,
                                  0, NULL, num_cb_args, cb_args);
    if (!ok) {
        const char *cb_exception =
            wasm_runtime_get_exception(target_module_inst);
        if (cb_exception)
            wasm_runtime_set_exception(
                (WASMModuleInstanceCommon *)inst, cb_exception);
    }

    return ok;
}

uint32
wasm_component_async_get_context_value(
    WASMComponentAsyncEngine *engine,
    uint32 ctx_idx)
{
    if (!engine || engine->current_task_id == 0)
        return 0;
    int32 idx = find_task_index(engine, engine->current_task_id);
    if (idx < 0) return 0;
    WASMComponentAsyncTask *task = &engine->tasks[idx];
    if (ctx_idx < task->context_count)
        return task->context_values[ctx_idx];
    return 0;
}

void
wasm_component_async_set_context_value(
    WASMComponentAsyncEngine *engine,
    uint32 ctx_idx,
    uint32 value)
{
    if (!engine || engine->current_task_id == 0)
        return;
    int32 idx = find_task_index(engine, engine->current_task_id);
    if (idx < 0) return;
    WASMComponentAsyncTask *task = &engine->tasks[idx];
    if (ctx_idx >= task->context_count) {
        uint32 new_count = task->context_count;
        while (new_count <= ctx_idx)
            new_count = new_count ? new_count * 2 : 4;
        uint32 *new_vals = wasm_runtime_realloc(
            task->context_values, sizeof(uint32) * new_count);
        if (!new_vals) return;
        memset(new_vals + task->context_count, 0,
               sizeof(uint32) * (new_count - task->context_count));
        task->context_values = new_vals;
        task->context_count = new_count;
    }
    task->context_values[ctx_idx] = value;
}

bool
wasm_component_async_is_task_cancelled(
    WASMComponentAsyncEngine *engine)
{
    if (!engine || engine->current_task_id == 0)
        return false;
    int32 idx = find_task_index(engine, engine->current_task_id);
    if (idx < 0) return false;
    return engine->tasks[idx].cancellation_requested;
}
