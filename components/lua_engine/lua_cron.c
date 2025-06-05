#include "lua_cron.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lauxlib.h"
#include "lua.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CRON_JOBS 30
#define MAX_PENDING_EVENTS 10

typedef struct {
    cron_job* job;
    char* rule_id; // 任务ID字符串
    int callback_ref; // Lua回调函数引用
    int config_ref; // Lua config表引用，可能为LUA_NOREF
    lua_State* L; // lua状态机指针
} lua_cron_job_t;

// 全局任务管理数组
static lua_cron_job_t cron_jobs[MAX_CRON_JOBS] = { 0 };

// 事件队列（环形缓冲区）
static lua_cron_job_t* pending_events[MAX_PENDING_EVENTS];
static int pending_head = 0;
static int pending_tail = 0;

// 事件队列互斥锁
static portMUX_TYPE pending_lock = portMUX_INITIALIZER_UNLOCKED;

// 事件信号量，cron_callback触发，worker_task等待
static SemaphoreHandle_t cron_event_sem = NULL;

// 查找已注册任务
static lua_cron_job_t* cron_get_job_by_id(const char* rule_id)
{
    for (int i = 0; i < MAX_CRON_JOBS; ++i) {
        if (cron_jobs[i].job && cron_jobs[i].rule_id && strcmp(cron_jobs[i].rule_id, rule_id) == 0) {
            return &cron_jobs[i];
        }
    }
    return NULL;
}

// cron_callback仅放事件并释放信号量（中断安全）
static void cron_callback(cron_job* job)
{
    if (!job || !job->data)
        return;

    lua_cron_job_t* entry = (lua_cron_job_t*)job->data;

    portENTER_CRITICAL(&pending_lock);
    int next_tail = (pending_tail + 1) % MAX_PENDING_EVENTS;
    if (next_tail != pending_head) { // 队列未满
        pending_events[pending_tail] = entry;
        pending_tail = next_tail;
        portEXIT_CRITICAL(&pending_lock);
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xSemaphoreGiveFromISR(cron_event_sem, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken) {
            portYIELD_FROM_ISR();
        }
    } else {
        portEXIT_CRITICAL(&pending_lock);
        printf("[cron_callback] pending_events queue full, drop event for %s\n", entry->rule_id);
    }
}

// cron_worker_task等待信号量，处理事件
static void cron_worker_task(void* arg)
{
    (void)arg;
    while (1) {
        if (xSemaphoreTake(cron_event_sem, portMAX_DELAY) == pdTRUE) {
            while (1) {
                portENTER_CRITICAL(&pending_lock);
                if (pending_head == pending_tail) {
                    // 队列空
                    portEXIT_CRITICAL(&pending_lock);
                    break;
                }
                lua_cron_job_t* job = pending_events[pending_head];
                pending_head = (pending_head + 1) % MAX_PENDING_EVENTS;
                portEXIT_CRITICAL(&pending_lock);

                if (!job || !job->L)
                    continue;

                lua_State* L = job->L;
                int top = lua_gettop(L);

                lua_rawgeti(L, LUA_REGISTRYINDEX, job->callback_ref);
                if (!lua_isfunction(L, -1)) {
                    lua_settop(L, top);
                    printf("[cron_worker] Callback is not a function for '%s'\n", job->rule_id);
                    continue;
                }

                lua_pushstring(L, job->rule_id);
                if (job->config_ref != LUA_NOREF) {
                    lua_rawgeti(L, LUA_REGISTRYINDEX, job->config_ref);
                } else {
                    lua_pushnil(L);
                }

                if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
                    const char* err = lua_tostring(L, -1);
                    printf("[cron_worker] callback error: %s\n", err);
                    lua_pop(L, 1);
                }

                lua_settop(L, top);
            }
        }
    }
}

// Lua接口: cron.register_cron(rule_id, schedule, callback, config)
static int l_register_cron(lua_State* L)
{
    const char* rule_id = luaL_checkstring(L, 1);
    const char* schedule = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION); // callback
    int has_config = !lua_isnoneornil(L, 4);

    if (cron_get_job_by_id(rule_id)) {
        return luaL_error(L, "rule_id '%s' already registered", rule_id);
    }

    int slot = -1;
    for (int i = 0; i < MAX_CRON_JOBS; ++i) {
        if (cron_jobs[i].job == NULL) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        return luaL_error(L, "cron job slots full");
    }

    char* rule_id_copy = strdup(rule_id);
    if (!rule_id_copy) {
        return luaL_error(L, "malloc failed");
    }

    lua_pushvalue(L, 3);
    int callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    int config_ref = LUA_NOREF;
    if (has_config) {
        lua_pushvalue(L, 4);
        config_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    cron_job* job = cron_job_create(schedule, cron_callback, NULL);
    if (!job) {
        free(rule_id_copy);
        luaL_unref(L, LUA_REGISTRYINDEX, callback_ref);
        if (config_ref != LUA_NOREF)
            luaL_unref(L, LUA_REGISTRYINDEX, config_ref);
        return luaL_error(L, "cron_job_create failed");
    }
    job->data = &cron_jobs[slot];

    int schedule_ret = cron_job_schedule(job);
    if (schedule_ret != 0) {
        cron_job_destroy(job);
        free(rule_id_copy);
        luaL_unref(L, LUA_REGISTRYINDEX, callback_ref);
        if (config_ref != LUA_NOREF)
            luaL_unref(L, LUA_REGISTRYINDEX, config_ref);
        printf("[cron] cron_job_schedule failed, ret=%d\n", schedule_ret);
        return luaL_error(L, "cron_job_schedule failed");
    }

    cron_jobs[slot].job = job;
    cron_jobs[slot].rule_id = rule_id_copy;
    cron_jobs[slot].callback_ref = callback_ref;
    cron_jobs[slot].config_ref = config_ref;
    cron_jobs[slot].L = L;

    printf("[cron] Registered cron job '%s' with schedule '%s', job_id=%d\n",
        rule_id_copy, schedule, job->id);

    lua_pushinteger(L, job->id);
    return 1;
}

// Lua接口: cron.unregister_cron(rule_id)
static int l_unregister_cron(lua_State* L)
{
    const char* rule_id = luaL_checkstring(L, 1);

    lua_cron_job_t* entry = cron_get_job_by_id(rule_id);
    if (!entry) {
        lua_pushboolean(L, 0);
        return 1;
    }

    cron_job_unschedule(entry->job);

    free(entry->rule_id);
    entry->rule_id = NULL;

    luaL_unref(L, LUA_REGISTRYINDEX, entry->callback_ref);
    entry->callback_ref = LUA_NOREF;

    if (entry->config_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, entry->config_ref);
        entry->config_ref = LUA_NOREF;
    }

    cron_job_destroy(entry->job);
    entry->job = NULL;
    entry->L = NULL;

    lua_pushboolean(L, 1);
    return 1;
}

// Lua接口: cron.list()
static int l_list_cron_jobs(lua_State* L)
{
    lua_newtable(L);
    int index = 1;
    for (int i = 0; i < MAX_CRON_JOBS; ++i) {
        if (cron_jobs[i].job) {
            lua_newtable(L);

            lua_pushstring(L, cron_jobs[i].rule_id);
            lua_setfield(L, -2, "rule_id");

            lua_pushinteger(L, cron_jobs[i].job->id);
            lua_setfield(L, -2, "job_id");

            lua_pushboolean(L, 1);
            lua_setfield(L, -2, "active");

            lua_rawseti(L, -2, index++);
        }
    }
    return 1;
}

static const struct luaL_Reg cronlib[] = {
    { "register_cron", l_register_cron },
    { "unregister_cron", l_unregister_cron },
    { "list", l_list_cron_jobs },
    { NULL, NULL }
};

void register_lua_cron(lua_State* L)
{
    luaL_newlib(L, cronlib);
    lua_setglobal(L, "cron");

    if (!cron_event_sem) {
        cron_event_sem = xSemaphoreCreateBinary();
        if (cron_event_sem == NULL) {
            printf("[cron] Failed to create event semaphore\n");
            return;
        }
        BaseType_t ret = xTaskCreatePinnedToCore(cron_worker_task, "cron_worker", 4096, NULL, 5, NULL, 0);
        if (ret != pdPASS) {
            printf("[cron] Failed to create cron_worker_task\n");
        }
    }
}
