// lua_timer.c
#include "lua_timer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lauxlib.h"
#include "lua.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/queue.h>

// 定义全局Lua状态机指针
lua_State* global_L = NULL;

// 日志标签
static const char* TAG = "lua_timer";

// 定时器任务结构体定义
typedef struct timer_task {
    uint32_t id; // 定时器ID
    int64_t wakeup_us; // 唤醒时间(微秒)
    int interval; // 重复间隔(毫秒)
    bool is_interval; // 是否为重复定时器
    int ref; // Lua回调函数引用
    TAILQ_ENTRY(timer_task)
    entries;
} timer_task_t;

// 定时器队列头
static TAILQ_HEAD(timer_queue_head, timer_task) timer_queue = TAILQ_HEAD_INITIALIZER(timer_queue);
static bool timer_initialized = false; // 定时器系统初始化标志
static uint32_t next_timer_id = 1; // 下一个定时器ID
static SemaphoreHandle_t timer_mutex = NULL; // 互斥锁保护队列

/**
 * @brief 获取当前系统时间(微秒)
 * @return 系统时间(微秒)
 */
static int64_t get_now_us(void)
{
    return esp_timer_get_time();
}

/**
 * @brief 初始化定时器系统
 */
void timer_system_init(void)
{
    if (!timer_initialized) {
        TAILQ_INIT(&timer_queue);
        timer_mutex = xSemaphoreCreateMutex();
        timer_initialized = true;
        ESP_LOGI(TAG, "Timer system initialized");
    }
}

/**
 * @brief 插入一个定时任务到队列
 * @param L Lua状态机
 * @param ms 定时时间(毫秒)
 * @param is_interval 是否为重复定时器
 * @return 定时器ID，0表示失败
 */
static uint32_t insert_timer_task(lua_State* L, int ms, bool is_interval)
{
    if (!timer_initialized) {
        timer_system_init();
    }

    xSemaphoreTake(timer_mutex, portMAX_DELAY);

    // 验证参数有效性
    luaL_checktype(L, 1, LUA_TFUNCTION);

    // 保存回调函数引用
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    // 分配任务内存
    timer_task_t* task = malloc(sizeof(timer_task_t));
    if (!task) {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        xSemaphoreGive(timer_mutex);
        return 0;
    }

    // 初始化任务参数
    uint32_t timer_id = next_timer_id++;
    task->id = timer_id;
    task->wakeup_us = get_now_us() + ms * 1000;
    task->interval = is_interval ? ms : 0;
    task->is_interval = is_interval;
    task->ref = ref;

    // 按唤醒时间排序插入队列
    timer_task_t* it;
    bool inserted = false;
    TAILQ_FOREACH(it, &timer_queue, entries)
    {
        if (task->wakeup_us < it->wakeup_us) {
            TAILQ_INSERT_BEFORE(it, task, entries);
            inserted = true;
            break;
        }
    }

    if (!inserted) {
        TAILQ_INSERT_TAIL(&timer_queue, task, entries);
    }

    xSemaphoreGive(timer_mutex);
    return timer_id;
}

/**
 * @brief 删除指定ID的定时器任务
 * @param timer_id 定时器ID
 */
static void remove_timer_task(uint32_t timer_id)
{
    if (!timer_initialized) {
        return;
    }

    xSemaphoreTake(timer_mutex, portMAX_DELAY);

    timer_task_t* task = NULL;
    timer_task_t* tmp = NULL;

    TAILQ_FOREACH_SAFE(task, &timer_queue, entries, tmp)
    {
        if (task->id == timer_id) {
            TAILQ_REMOVE(&timer_queue, task, entries);
            if (global_L) {
                luaL_unref(global_L, LUA_REGISTRYINDEX, task->ref);
            }
            free(task);
            break;
        }
    }

    xSemaphoreGive(timer_mutex);
}

/**
 * @brief 清理所有定时器任务
 */
void timer_cleanup(void)
{
    if (!timer_initialized) {
        return;
    }

    xSemaphoreTake(timer_mutex, portMAX_DELAY);

    timer_task_t* task = NULL;
    timer_task_t* tmp = NULL;

    TAILQ_FOREACH_SAFE(task, &timer_queue, entries, tmp)
    {
        TAILQ_REMOVE(&timer_queue, task, entries);
        if (global_L) {
            luaL_unref(global_L, LUA_REGISTRYINDEX, task->ref);
        }
        free(task);
    }

    timer_initialized = false;
    xSemaphoreGive(timer_mutex);
}

/**
 * @brief 关闭定时器模块，释放所有资源
 */
void timer_shutdown(void)
{
    timer_cleanup();
    if (timer_mutex) {
        vSemaphoreDelete(timer_mutex);
        timer_mutex = NULL;
    }
    ESP_LOGI(TAG, "Timer system shutdown");
}

/**
 * @brief Lua接口：创建一次性定时器
 * @param L Lua状态机
 * @return 定时器ID
 */
static int l_settimeout(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    int ms = luaL_checkinteger(L, 2);

    if (ms < 0) {
        return luaL_error(L, "timeout cannot be negative");
    }

    uint32_t timer_id = insert_timer_task(L, ms, false);
    lua_pushinteger(L, timer_id);
    return 1;
}

/**
 * @brief Lua接口：创建重复定时器
 * @param L Lua状态机
 * @return 定时器ID
 */
static int l_setinterval(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    int ms = luaL_checkinteger(L, 2);

    if (ms < 0) {
        return luaL_error(L, "interval cannot be negative");
    }

    uint32_t timer_id = insert_timer_task(L, ms, true);
    lua_pushinteger(L, timer_id);
    return 1;
}

/**
 * @brief Lua接口：删除一次性定时器
 * @param L Lua状态机
 * @return 是否删除成功
 */
static int l_cleartimeout(lua_State* L)
{
    uint32_t timer_id = luaL_checkinteger(L, 1);
    remove_timer_task(timer_id);
    lua_pushboolean(L, true);
    return 1;
}

/**
 * @brief Lua接口：删除重复定时器
 * @param L Lua状态机
 * @return 是否删除成功
 */
static int l_clearinterval(lua_State* L)
{
    uint32_t timer_id = luaL_checkinteger(L, 1);
    remove_timer_task(timer_id);
    lua_pushboolean(L, true);
    return 1;
}

/**
 * @brief 处理所有到期的定时器任务
 * @param L Lua状态机
 */
void timer_process(lua_State* L)
{
    if (!timer_initialized || !L) {
        return;
    }

    xSemaphoreTake(timer_mutex, portMAX_DELAY);

    int64_t now = get_now_us();

    while (!TAILQ_EMPTY(&timer_queue)) {
        timer_task_t* task = TAILQ_FIRST(&timer_queue);
        if (task->wakeup_us > now) {
            break;
        }

        TAILQ_REMOVE(&timer_queue, task, entries);

        // 获取回调函数
        lua_rawgeti(L, LUA_REGISTRYINDEX, task->ref);

        // 调用回调函数
        int status = lua_pcall(L, 0, 0, 0);
        if (status != LUA_OK) {
            const char* err = lua_tostring(L, -1);
            ESP_LOGE(TAG, "Timer callback error (ID=%" PRIu32 "): %s", task->id, err);
            lua_pop(L, 1);

            // 回调出错时删除interval定时器
            if (task->is_interval) {
                luaL_unref(L, LUA_REGISTRYINDEX, task->ref);
                free(task);
                xSemaphoreGive(timer_mutex);
                continue;
            }
        }

        // 处理重复定时器
        if (task->is_interval) {
            task->wakeup_us = now + task->interval * 1000;

            // 重新插入队列
            timer_task_t* it;
            bool inserted = false;
            TAILQ_FOREACH(it, &timer_queue, entries)
            {
                if (task->wakeup_us < it->wakeup_us) {
                    TAILQ_INSERT_BEFORE(it, task, entries);
                    inserted = true;
                    break;
                }
            }

            if (!inserted) {
                TAILQ_INSERT_TAIL(&timer_queue, task, entries);
            }
        } else {
            // 释放一次性定时器资源
            luaL_unref(L, LUA_REGISTRYINDEX, task->ref);
            free(task);
        }
    }

    xSemaphoreGive(timer_mutex);
}

/**
 * @brief 注册timer模块到Lua环境
 * @param L Lua状态机
 * @return 模块句柄
 */
int luaopen_timer(lua_State* L)
{
    timer_system_init();

    // 定义Lua接口函数映射表
    luaL_Reg timer_funcs[] = {
        { "setTimeout", l_settimeout },
        { "setInterval", l_setinterval },
        { "clearTimeout", l_cleartimeout },
        { "clearInterval", l_clearinterval },
        { NULL, NULL }
    };

    luaL_newlib(L, timer_funcs);
    return 1;
}

/**
 * @brief 注册定时器模块到Lua
 * @param L Lua状态机
 */
void register_lua_timer(lua_State* L)
{
    timer_system_init();
    global_L = L;

    luaL_requiref(L, "timer", luaopen_timer, 1);
    lua_pop(L, 1); // 弹出模块

    ESP_LOGI(TAG, "Timer module registered to Lua");
}