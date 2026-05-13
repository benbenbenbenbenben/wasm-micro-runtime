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

    engine->next_task_id = 1;
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
    }

    if (engine->tasks)
        wasm_runtime_free(engine->tasks);
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

    call_ok = wasm_runtime_call_component_values(
        (wasm_module_inst_t)inst, (wasm_component_func_t)task->function,
        task->num_results, task->results,
        task->num_args, task->args);

    task->state = call_ok ? WASM_COMP_ASYNC_TASK_COMPLETED
                          : WASM_COMP_ASYNC_TASK_FAILED;

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

    if (engine->tasks[task_idx].state != WASM_COMP_ASYNC_TASK_PENDING)
        return false;

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
