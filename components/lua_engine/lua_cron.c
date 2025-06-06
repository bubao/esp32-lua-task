#include "lua_cron.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lauxlib.h"
#include "lua.h"
#include "lua_cron.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 配置参数
#define MAX_CRON_JOBS 30
#define MAX_PENDING_EVENTS 10
#define CRON_WORKER_STACK_SIZE 4096
#define CRON_WORKER_PRIORITY 5
#define CRON_TAG "CRON"

// 任务状态枚举
typedef enum {
    JOB_STATE_INIT,
    JOB_STATE_RUNNING,
    JOB_STATE_STOPPED,
    JOB_STATE_ERROR
} cron_job_state_t;

// Lua cron 任务结构
typedef struct {
    cron_job* job; // cron 任务指针
    char* rule_id; // 任务ID字符串
    int callback_ref; // Lua回调函数引用
    int config_ref; // Lua配置表引用
    lua_State* L; // Lua状态机指针
    cron_job_state_t state; // 任务状态
    uint32_t create_time; // 创建时间戳
} lua_cron_job_t;

// 全局任务管理数组
static lua_cron_job_t* cron_jobs[MAX_CRON_JOBS] = { NULL };

// 事件队列（环形缓冲区）
static lua_cron_job_t* pending_events[MAX_PENDING_EVENTS];
static int pending_head = 0;
static int pending_tail = 0;

// 事件队列互斥锁
static SemaphoreHandle_t job_lock = NULL;
static SemaphoreHandle_t event_lock = NULL;

// 事件信号量
static SemaphoreHandle_t cron_event_sem = NULL;

// 任务句柄
static TaskHandle_t cron_worker_task_handle = NULL;

// 私有函数声明
static lua_cron_job_t* create_job_entry(lua_State* L, const char* rule_id, const char* schedule);
static void destroy_job_entry(lua_cron_job_t* entry);
static int find_empty_slot(void);
static void log_cron_event(const char* level, const char* rule_id, const char* message);

// 查找已注册任务
static lua_cron_job_t* cron_get_job_by_id(const char* rule_id)
{
    if (!rule_id)
        return NULL;

    xSemaphoreTake(job_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_CRON_JOBS; ++i) {
        if (cron_jobs[i] && cron_jobs[i]->rule_id && strcmp(cron_jobs[i]->rule_id, rule_id) == 0) {
            xSemaphoreGive(job_lock);
            return cron_jobs[i];
        }
    }
    xSemaphoreGive(job_lock);
    return NULL;
}

// 创建新的任务条目
static lua_cron_job_t* create_job_entry(lua_State* L, const char* rule_id, const char* schedule)
{
    lua_cron_job_t* entry = (lua_cron_job_t*)calloc(1, sizeof(lua_cron_job_t));
    if (!entry) {
        log_cron_event("ERROR", rule_id, "内存分配失败");
        return NULL;
    }

    entry->rule_id = strdup(rule_id);
    if (!entry->rule_id) {
        free(entry);
        log_cron_event("ERROR", rule_id, "内存分配失败");
        return NULL;
    }

    entry->L = L;
    entry->state = JOB_STATE_INIT;
    entry->create_time = xTaskGetTickCount();

    return entry;
}

// 销毁任务条目
static void destroy_job_entry(lua_cron_job_t* entry)
{
    if (!entry)
        return;

    if (entry->rule_id) {
        free(entry->rule_id);
        entry->rule_id = NULL;
    }

    if (entry->job) {
        cron_job_destroy(entry->job);
        entry->job = NULL;
    }

    if (entry->callback_ref != LUA_NOREF) {
        luaL_unref(entry->L, LUA_REGISTRYINDEX, entry->callback_ref);
        entry->callback_ref = LUA_NOREF;
    }

    if (entry->config_ref != LUA_NOREF) {
        luaL_unref(entry->L, LUA_REGISTRYINDEX, entry->config_ref);
        entry->config_ref = LUA_NOREF;
    }

    free(entry);
}

// 查找空闲插槽
static int find_empty_slot(void)
{
    xSemaphoreTake(job_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_CRON_JOBS; ++i) {
        if (!cron_jobs[i]) {
            xSemaphoreGive(job_lock);
            return i;
        }
    }
    xSemaphoreGive(job_lock);
    return -1;
}

// 使用esp_log的日志记录函数
static void log_cron_event(const char* level, const char* rule_id, const char* message)
{
    const char* full_message = rule_id ? (message ? strdup(message) : "") : (message ? strdup(message) : "");

    if (!strcmp(level, "INFO")) {
        ESP_LOGI(CRON_TAG, "[%s] %s", rule_id ? rule_id : "system", full_message);
    } else if (!strcmp(level, "ERROR")) {
        ESP_LOGE(CRON_TAG, "[%s] %s", rule_id ? rule_id : "system", full_message);
    } else if (!strcmp(level, "WARNING")) {
        ESP_LOGW(CRON_TAG, "[%s] %s", rule_id ? rule_id : "system", full_message);
    } else {
        ESP_LOGD(CRON_TAG, "[%s] %s", rule_id ? rule_id : "system", full_message);
    }

    if (full_message)
        free((void*)full_message);
}

// cron_callback仅放事件并释放信号量（中断安全）
static void cron_callback(cron_job* job)
{
    if (!job || !job->data)
        return;

    lua_cron_job_t* entry = (lua_cron_job_t*)job->data;

    xSemaphoreTake(event_lock, portMAX_DELAY);
    int next_tail = (pending_tail + 1) % MAX_PENDING_EVENTS;
    if (next_tail != pending_head) { // 队列未满
        pending_events[pending_tail] = entry;
        pending_tail = next_tail;
        xSemaphoreGive(event_lock);

        // 释放信号量通知工作线程
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xSemaphoreGiveFromISR(cron_event_sem, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken) {
            portYIELD_FROM_ISR();
        }
    } else {
        xSemaphoreGive(event_lock);
        log_cron_event("WARNING", entry->rule_id, "事件队列已满，丢弃事件");
    }
}

// cron_worker_task等待信号量，处理事件
static void cron_worker_task(void* arg)
{
    (void)arg;

    log_cron_event("INFO", NULL, "工作线程已启动");

    while (1) {
        // 等待事件信号量
        if (xSemaphoreTake(cron_event_sem, portMAX_DELAY) == pdTRUE) {
            xSemaphoreTake(event_lock, portMAX_DELAY);

            // 处理所有待处理的事件
            while (pending_head != pending_tail) {
                lua_cron_job_t* job = pending_events[pending_head];
                pending_head = (pending_head + 1) % MAX_PENDING_EVENTS;

                xSemaphoreGive(event_lock);

                if (!job || !job->L)
                    continue;

                // 确保Lua状态机有效
                lua_State* L = job->L;
                int top = lua_gettop(L);

                // 调用Lua回调函数
                lua_rawgeti(L, LUA_REGISTRYINDEX, job->callback_ref);
                if (!lua_isfunction(L, -1)) {
                    log_cron_event("ERROR", job->rule_id, "回调不是函数");
                    lua_settop(L, top);
                    continue;
                }

                // 准备参数: rule_id, config
                lua_pushstring(L, job->rule_id);
                if (job->config_ref != LUA_NOREF) {
                    lua_rawgeti(L, LUA_REGISTRYINDEX, job->config_ref);
                } else {
                    lua_pushnil(L);
                }

                // 调用函数并处理错误
                if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
                    const char* err = lua_tostring(L, -1);
                    log_cron_event("ERROR", job->rule_id, err);
                    lua_pop(L, 1);
                }

                lua_settop(L, top);

                xSemaphoreTake(event_lock, portMAX_DELAY);
            }

            xSemaphoreGive(event_lock);
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

    // 检查是否已注册
    if (cron_get_job_by_id(rule_id)) {
        return luaL_error(L, "rule_id '%s' 已注册", rule_id);
    }

    // 查找空闲插槽
    int slot = find_empty_slot();
    if (slot == -1) {
        return luaL_error(L, "cron 任务槽已满，最多支持 %d 个任务", MAX_CRON_JOBS);
    }

    // 创建任务条目
    lua_cron_job_t* entry = create_job_entry(L, rule_id, schedule);
    if (!entry) {
        return luaL_error(L, "创建任务失败");
    }

    // 保存回调和配置引用
    lua_pushvalue(L, 3);
    entry->callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    if (has_config) {
        lua_pushvalue(L, 4);
        entry->config_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    // 创建cron任务
    entry->job = cron_job_create(schedule, cron_callback, entry);
    if (!entry->job) {
        destroy_job_entry(entry);
        return luaL_error(L, "创建cron任务失败");
    }

    // 调度任务
    int schedule_ret = cron_job_schedule(entry->job);
    if (schedule_ret != 0) {
        cron_job_destroy(entry->job);
        destroy_job_entry(entry);
        log_cron_event("ERROR", rule_id, "调度失败");
        return luaL_error(L, "调度cron任务失败，错误码: %d", schedule_ret);
    }

    // 更新状态并保存到全局数组
    entry->state = JOB_STATE_RUNNING;

    xSemaphoreTake(job_lock, portMAX_DELAY);
    cron_jobs[slot] = entry;
    xSemaphoreGive(job_lock);

    log_cron_event("INFO", rule_id, "已注册");
    lua_pushinteger(L, entry->job->id);
    return 1;
}

// Lua接口: cron.unregister_cron(rule_id)
static int l_unregister_cron(lua_State* L)
{
    const char* rule_id = luaL_checkstring(L, 1);

    // 查找任务
    lua_cron_job_t* entry = cron_get_job_by_id(rule_id);
    if (!entry) {
        lua_pushboolean(L, 0);
        return 1;
    }

    // 更新状态
    entry->state = JOB_STATE_STOPPED;

    // 取消调度并清理资源
    cron_job_unschedule(entry->job);

    // 从全局数组中移除
    xSemaphoreTake(job_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_CRON_JOBS; ++i) {
        if (cron_jobs[i] == entry) {
            cron_jobs[i] = NULL;
            break;
        }
    }
    xSemaphoreGive(job_lock);

    // 销毁任务条目
    destroy_job_entry(entry);

    log_cron_event("INFO", rule_id, "已注销");
    lua_pushboolean(L, 1);
    return 1;
}

// Lua接口: cron.list()
static int l_list_cron_jobs(lua_State* L)
{
    lua_newtable(L);
    int index = 1;

    xSemaphoreTake(job_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_CRON_JOBS; ++i) {
        if (cron_jobs[i]) {
            lua_newtable(L);

            lua_pushstring(L, cron_jobs[i]->rule_id);
            lua_setfield(L, -2, "rule_id");

            lua_pushinteger(L, cron_jobs[i]->job ? cron_jobs[i]->job->id : -1);
            lua_setfield(L, -2, "job_id");

            lua_pushinteger(L, cron_jobs[i]->state);
            lua_setfield(L, -2, "state");

            lua_pushinteger(L, cron_jobs[i]->create_time);
            lua_setfield(L, -2, "create_time");

            lua_rawseti(L, -2, index++);
        }
    }
    xSemaphoreGive(job_lock);

    return 1;
}

// 清理所有资源
static void cleanup_all_resources(void)
{
    if (!cron_event_sem)
        return;

    // 停止工作线程
    if (cron_worker_task_handle) {
        vTaskDelete(cron_worker_task_handle);
        cron_worker_task_handle = NULL;
    }

    // 清理所有任务
    xSemaphoreTake(job_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_CRON_JOBS; ++i) {
        if (cron_jobs[i]) {
            if (cron_jobs[i]->job) {
                cron_job_unschedule(cron_jobs[i]->job);
                cron_job_destroy(cron_jobs[i]->job);
            }
            destroy_job_entry(cron_jobs[i]);
            cron_jobs[i] = NULL;
        }
    }
    xSemaphoreGive(job_lock);

    // 清理信号量
    if (cron_event_sem) {
        vSemaphoreDelete(cron_event_sem);
        cron_event_sem = NULL;
    }

    if (job_lock) {
        vSemaphoreDelete(job_lock);
        job_lock = NULL;
    }

    if (event_lock) {
        vSemaphoreDelete(event_lock);
        event_lock = NULL;
    }

    log_cron_event("INFO", NULL, "已清理所有资源");
}

static const struct luaL_Reg cronlib[] = {
    { "register_cron", l_register_cron },
    { "unregister_cron", l_unregister_cron },
    { "list", l_list_cron_jobs },
    { NULL, NULL }
};

void register_lua_cron(lua_State* L)
{
    // 创建信号量和锁
    if (!job_lock) {
        job_lock = xSemaphoreCreateMutex();
        if (!job_lock) {
            log_cron_event("ERROR", NULL, "创建作业锁失败");
            return;
        }
    }

    if (!event_lock) {
        event_lock = xSemaphoreCreateMutex();
        if (!event_lock) {
            log_cron_event("ERROR", NULL, "创建事件锁失败");
            return;
        }
    }

    if (!cron_event_sem) {
        cron_event_sem = xSemaphoreCreateBinary();
        if (!cron_event_sem) {
            log_cron_event("ERROR", NULL, "创建事件信号量失败");
            return;
        }
    }

    // 创建工作线程
    if (!cron_worker_task_handle) {
        BaseType_t ret = xTaskCreatePinnedToCore(
            cron_worker_task,
            "cron_worker",
            CRON_WORKER_STACK_SIZE,
            NULL,
            CRON_WORKER_PRIORITY,
            &cron_worker_task_handle,
            0);

        if (ret != pdPASS) {
            log_cron_event("ERROR", NULL, "创建工作线程失败");
            return;
        }
    }

    // 注册Lua库
    luaL_newlib(L, cronlib);
    lua_setglobal(L, "cron");

    log_cron_event("INFO", NULL, "Lua cron库已注册");
}

// 新增: 用于清理资源的函数
void unregister_lua_cron(void)
{
    cleanup_all_resources();
}