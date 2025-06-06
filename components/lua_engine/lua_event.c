#include "lua_event.h"
#include "esp_log.h"
#include "lauxlib.h"
#include "lua.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>

// 日志标签
static const char* TAG = "lua_event";

// 事件处理函数节点
typedef struct event_handler {
    lua_State* co; // 协程状态（Lua状态机）
    int func_ref; // 函数引用
    TAILQ_ENTRY(event_handler)
    entries;
} event_handler_t;

// 事件处理函数列表（按事件名称索引）
typedef struct event_handler_list {
    char* event_name;
    TAILQ_HEAD(, event_handler)
    handlers;
    TAILQ_ENTRY(event_handler_list)
    entries;
} event_handler_list_t;

// 全局事件处理函数注册表
static TAILQ_HEAD(, event_handler_list) event_registry = TAILQ_HEAD_INITIALIZER(event_registry);
static bool event_initialized = false;

// 初始化事件系统
static void event_system_init(void)
{
    if (!event_initialized) {
        TAILQ_INIT(&event_registry);
        event_initialized = true;
        ESP_LOGI(TAG, "Event system initialized");
    }
}

// 查找或创建事件处理函数列表
static event_handler_list_t* find_or_create_handler_list(const char* event_name)
{
    event_handler_list_t* list;

    TAILQ_FOREACH(list, &event_registry, entries)
    {
        if (strcmp(list->event_name, event_name) == 0) {
            ESP_LOGD(TAG, "Found existing handler list for event: %s", event_name);
            return list;
        }
    }

    // 创建新的事件处理函数列表
    list = malloc(sizeof(event_handler_list_t));
    if (!list) {
        ESP_LOGE(TAG, "Failed to allocate memory for event handler list");
        return NULL;
    }

    list->event_name = strdup(event_name);
    if (!list->event_name) {
        ESP_LOGE(TAG, "Failed to allocate memory for event name");
        free(list);
        return NULL;
    }

    TAILQ_INIT(&list->handlers);
    TAILQ_INSERT_TAIL(&event_registry, list, entries);

    ESP_LOGD(TAG, "Created new handler list for event: %s", event_name);
    return list;
}

// 注册事件处理函数
static int l_on(lua_State* L)
{
    const char* event_name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    event_system_init();

    // 保存函数引用
    lua_pushvalue(L, 2);
    int func_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    // 查找或创建事件处理函数列表
    event_handler_list_t* list = find_or_create_handler_list(event_name);
    if (!list) {
        luaL_unref(L, LUA_REGISTRYINDEX, func_ref);
        return luaL_error(L, "Failed to register event handler");
    }

    // 创建新的事件处理函数节点
    event_handler_t* handler = malloc(sizeof(event_handler_t));
    if (!handler) {
        luaL_unref(L, LUA_REGISTRYINDEX, func_ref);
        return luaL_error(L, "Failed to allocate memory for event handler");
    }

    handler->co = L; // 保存Lua状态机引用
    handler->func_ref = func_ref;

    // 添加到列表
    TAILQ_INSERT_TAIL(&list->handlers, handler, entries);

    ESP_LOGI(TAG, "Event handler registered for event: %s (ref: %d)", event_name, func_ref);
    return 0;
}

// 触发事件（内部函数）
// 在 trigger_event 函数中添加更健壮的错误处理
static void trigger_event(lua_State* L, const char* event_name, int argc)
{
    ESP_LOGI(TAG, "Triggering event: %s with %d arguments", event_name, argc);

    // 查找事件处理函数列表
    event_handler_list_t* list = NULL;
    TAILQ_FOREACH(list, &event_registry, entries)
    {
        if (strcmp(list->event_name, event_name) == 0) {
            break;
        }
    }

    if (list) {
        event_handler_t* handler = NULL;
        event_handler_t* tmp = NULL;

        // 调用所有处理函数
        TAILQ_FOREACH_SAFE(handler, &list->handlers, entries, tmp)
        {
            lua_State* handler_L = handler->co;

            // 获取处理函数
            lua_rawgeti(handler_L, LUA_REGISTRYINDEX, handler->func_ref);

            // 检查函数是否有效
            if (!lua_isfunction(handler_L, -1)) {
                ESP_LOGE(TAG, "Invalid handler for event %s (ref: %d)",
                    event_name, handler->func_ref);
                lua_pop(handler_L, 1);
                continue;
            }

            // 复制参数
            for (int i = 0; i < argc; i++) {
                lua_pushvalue(L, i + 2);
            }

            // 使用 pcall 保护调用
            int status = lua_pcall(handler_L, argc, 0, 0);
            if (status != LUA_OK) {
                // 获取错误信息并记录
                const char* err = lua_tostring(handler_L, -1);
                ESP_LOGE(TAG, "Error in event handler for %s: %s", event_name, err);

                // 打印堆栈跟踪
                lua_getglobal(handler_L, "debug");
                lua_getfield(handler_L, -1, "traceback");
                lua_call(handler_L, 0, 1);
                const char* traceback = lua_tostring(handler_L, -1);
                ESP_LOGE(TAG, "Stack traceback:\n%s", traceback);

                // 清理栈
                lua_pop(handler_L, 2); // 弹出错误信息和堆栈跟踪
            }
        }
    } else {
        ESP_LOGW(TAG, "No handlers registered for event: %s", event_name);
    }
}

// 触发事件（Lua接口）
static int l_trigger(lua_State* L)
{
    const char* event_name = luaL_checkstring(L, 1);
    int argc = lua_gettop(L) - 1;

    trigger_event(L, event_name, argc);
    return 0;
}

// 从C代码触发事件
void event_trigger(lua_State* L, const char* event_name, int argc, const char** argv)
{
    if (!event_initialized) {
        ESP_LOGW(TAG, "Event system not initialized, ignoring trigger for %s", event_name);
        return;
    }

    ESP_LOGI(TAG, "Triggering event from C: %s with %d arguments", event_name, argc);

    // 将参数压入Lua栈
    for (int i = 0; i < argc; i++) {
        if (argv[i]) {
            lua_pushstring(L, argv[i]);
        } else {
            lua_pushnil(L);
        }
    }

    // 触发事件
    trigger_event(L, event_name, argc);

    // 清理栈上的参数
    lua_pop(L, argc);
}

// 获取事件处理函数数量（用于调试）
int event_get_handler_count(const char* event_name)
{
    event_handler_list_t* list = NULL;
    TAILQ_FOREACH(list, &event_registry, entries)
    {
        if (strcmp(list->event_name, event_name) == 0) {
            int count = 0;
            event_handler_t* handler = NULL;
            TAILQ_FOREACH(handler, &list->handlers, entries)
            {
                count++;
            }
            return count;
        }
    }
    return 0;
}

// 注册Lua事件函数
void register_lua_event(lua_State* L)
{
    event_system_init();

    // 注册事件函数
    lua_register(L, "on", l_on);
    lua_register(L, "trigger", l_trigger);

    ESP_LOGI(TAG, "Lua event system registered successfully");
}

// 清理事件系统资源
void event_cleanup(void)
{
    if (!event_initialized) {
        return;
    }

    ESP_LOGI(TAG, "Cleaning up event system resources");

    // 清理事件处理函数注册表
    event_handler_list_t* list = NULL;
    event_handler_list_t* tmp_list = NULL;

    TAILQ_FOREACH_SAFE(list, &event_registry, entries, tmp_list)
    {
        event_handler_t* handler = NULL;
        event_handler_t* tmp_handler = NULL;

        TAILQ_FOREACH_SAFE(handler, &list->handlers, entries, tmp_handler)
        {
            TAILQ_REMOVE(&list->handlers, handler, entries);
            luaL_unref(handler->co, LUA_REGISTRYINDEX, handler->func_ref);
            free(handler);
        }

        TAILQ_REMOVE(&event_registry, list, entries);
        free(list->event_name);
        free(list);
    }

    event_initialized = false;
    ESP_LOGI(TAG, "Event system cleaned up successfully");
}