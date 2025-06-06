#include "lua_timer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lauxlib.h"
#include "lua.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/queue.h>

// 日志标签
static const char* TAG = "lua_timer";

typedef struct timer_task {
    lua_State* co;
    int64_t wakeup_us;
    TAILQ_ENTRY(timer_task)
    entries;
} timer_task_t;

static TAILQ_HEAD(timer_queue_head, timer_task) timer_queue = TAILQ_HEAD_INITIALIZER(timer_queue);
static bool timer_initialized = false;

// 获取当前时间（us）
static int64_t get_now_us(void)
{
    return esp_timer_get_time();
}

// 初始化定时器系统
static void timer_system_init(void)
{
    if (!timer_initialized) {
        TAILQ_INIT(&timer_queue);
        timer_initialized = true;
    }
}

// 插入一个定时任务（按唤醒时间升序插入）
static void insert_timer_task(lua_State* co, int64_t wakeup_us)
{
    timer_task_t* task = malloc(sizeof(timer_task_t));
    if (!task) {
        ESP_LOGE(TAG, "Failed to allocate memory for timer task");
        return;
    }

    task->co = co;
    task->wakeup_us = wakeup_us;

    timer_task_t* it;
    TAILQ_FOREACH(it, &timer_queue, entries)
    {
        if (wakeup_us < it->wakeup_us) {
            TAILQ_INSERT_BEFORE(it, task, entries);
            return;
        }
    }

    TAILQ_INSERT_TAIL(&timer_queue, task, entries);
}

// 删除与指定协程相关的所有定时任务
static void remove_timer_tasks_by_co(lua_State* co)
{
    timer_task_t* task = NULL;
    timer_task_t* tmp = NULL;

    TAILQ_FOREACH_SAFE(task, &timer_queue, entries, tmp)
    {
        if (task->co == co) {
            TAILQ_REMOVE(&timer_queue, task, entries);
            free(task);
        }
    }
}

// 清理所有定时器任务
void timer_cleanup(void)
{
    timer_task_t* task = NULL;
    timer_task_t* tmp = NULL;

    TAILQ_FOREACH_SAFE(task, &timer_queue, entries, tmp)
    {
        TAILQ_REMOVE(&timer_queue, task, entries);
        free(task);
    }

    timer_initialized = false;
}

// delay(ms) — Lua函数，协程挂起并设置定时器
static int l_delay(lua_State* L)
{
    int ms = luaL_checkinteger(L, 1);

    if (ms < 0) {
        return luaL_error(L, "delay time cannot be negative");
    }

    lua_State* co = lua_tothread(L, lua_upvalueindex(1));
    int64_t wakeup_time = get_now_us() + ms * 1000;

    insert_timer_task(co, wakeup_time);
    return lua_yield(L, 0);
}

// settimeout(ms, fn) — 启动一个定时执行的函数（fn 作为 coroutine 启动）
static int l_settimeout(lua_State* L)
{
    int ms = luaL_checkinteger(L, 1);

    if (ms < 0) {
        return luaL_error(L, "timeout cannot be negative");
    }

    luaL_checktype(L, 2, LUA_TFUNCTION);

    lua_State* new_co = lua_newthread(L); // 创建新线程
    if (!new_co) {
        return luaL_error(L, "failed to create new coroutine");
    }

    lua_pushvalue(L, 2); // 拷贝函数到顶部
    lua_xmove(L, new_co, 1); // 移入新线程

    int64_t wakeup_time = get_now_us() + ms * 1000;
    insert_timer_task(new_co, wakeup_time);

    return 0; // 主线程不挂起
}

// 每个 tick 调用一次，处理到期的定时器
void timer_process(lua_State* L)
{
    if (!timer_initialized) {
        return;
    }

    int64_t now = get_now_us();

    while (!TAILQ_EMPTY(&timer_queue)) {
        timer_task_t* task = TAILQ_FIRST(&timer_queue);
        if (task->wakeup_us > now)
            break;

        TAILQ_REMOVE(&timer_queue, task, entries);

        lua_State* co = task->co;
        int nresults = 0;
        int status = lua_resume(co, NULL, 0, &nresults);

        if (status != LUA_OK && status != LUA_YIELD) {
            const char* err = lua_tostring(co, -1);
            ESP_LOGE(TAG, "Coroutine error: %s", err);
            lua_pop(co, 1);

            // 协程执行出错，可能需要清理该协程的其他任务
            remove_timer_tasks_by_co(co);
        }

        // 如果协程执行完毕（非挂起状态），清理可能存在的其他任务
        if (status == LUA_OK) {
            remove_timer_tasks_by_co(co);
        }

        free(task);
    }
}

// 注册 delay / settimeout 到 Lua
void register_lua_timer(lua_State* L)
{
    timer_system_init();

    lua_State* main_co = L; // 假设 delay 是主线程调用

    // delay 函数包装成 closure，携带主线程
    lua_pushthread(main_co); // push 主线程
    lua_pushcclosure(main_co, l_delay, 1);
    lua_setglobal(main_co, "delay");

    // settimeout 不需要携带
    lua_register(main_co, "setTimeout", l_settimeout);

    ESP_LOGI(TAG, "Lua timer functions registered successfully");
}