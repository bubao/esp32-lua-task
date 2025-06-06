#include "lua_http.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lauxlib.h"
#include "lua.h"
#include <stdlib.h>
#include <string.h>

// 日志标签
static const char* TAG = "lua_http";

// HTTP 请求完成消息
typedef struct http_complete_msg {
    lua_State* L; // Lua 状态机
    int callback_ref; // 回调函数引用
    int status_code; // HTTP 状态码
    char* response; // 响应数据
    size_t response_size; // 响应大小
} http_complete_msg_t;

// 函数前向声明
static void http_message_task(void* arg);
static void http_task(void* arg);
static esp_err_t http_module_init(void);
static void process_http_message(lua_State* L, http_complete_msg_t* msg);
static esp_err_t http_event_handler(esp_http_client_event_t* evt);

// HTTP 请求上下文
typedef struct http_context {
    lua_State* L; // Lua 状态机
    int callback_ref; // 回调函数引用
} http_context_t;

// 全局消息队列
static QueueHandle_t global_http_msg_queue = NULL;
static SemaphoreHandle_t http_mutex = NULL;
static bool http_initialized = false;

// 初始化HTTP模块
static esp_err_t http_module_init(void)
{
    if (http_initialized) {
        return ESP_OK;
    }

    // 使用互斥锁保护初始化
    if (http_mutex == NULL) {
        http_mutex = xSemaphoreCreateMutex();
        if (!http_mutex) {
            ESP_LOGE(TAG, "Failed to create HTTP mutex");
            return ESP_FAIL;
        }
    }

    xSemaphoreTake(http_mutex, portMAX_DELAY);

    if (!http_initialized) {
        // 创建全局消息队列
        global_http_msg_queue = xQueueCreate(10, sizeof(http_complete_msg_t));
        if (!global_http_msg_queue) {
            ESP_LOGE(TAG, "Failed to create global HTTP message queue");
            xSemaphoreGive(http_mutex);
            return ESP_FAIL;
        }

        // 创建消息处理任务
        if (xTaskCreatePinnedToCore(http_message_task, "http_msg_task", 4096, NULL, 5, NULL, 0) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create HTTP message task");
            vQueueDelete(global_http_msg_queue);
            global_http_msg_queue = NULL;
            xSemaphoreGive(http_mutex);
            return ESP_FAIL;
        }

        http_initialized = true;
    }

    xSemaphoreGive(http_mutex);
    return ESP_OK;
}

// HTTP 事件处理函数
static esp_err_t http_event_handler(esp_http_client_event_t* evt)
{
    // 可以在这里处理HTTP事件，如接收头信息、数据等
    switch (evt->event_id) {
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "Header: %s: %s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "Data received, length: %d", evt->data_len);
        break;
    default:
        break;
    }
    return ESP_OK;
}

// 处理 HTTP 响应消息
static void process_http_message(lua_State* L, http_complete_msg_t* msg)
{
    if (!msg || !msg->callback_ref || !L) {
        return;
    }

    // 使用互斥锁保护Lua状态机访问
    if (http_mutex) {
        xSemaphoreTake(http_mutex, portMAX_DELAY);
    }

    // 检查 Lua 状态机是否有效
    if (lua_checkstack(L, 4)) {
        // 获取回调函数
        lua_rawgeti(L, LUA_REGISTRYINDEX, msg->callback_ref);

        // 压入状态码
        lua_pushinteger(L, msg->status_code);

        // 压入响应数据
        if (msg->response && msg->response_size > 0) {
            lua_pushlstring(L, msg->response, msg->response_size);
            free(msg->response); // 释放响应数据
        } else {
            lua_pushnil(L);
        }

        // 调用回调函数 (2 个参数, 0 个返回值)
        int status = lua_pcall(L, 2, 0, 0);
        if (status != LUA_OK) {
            const char* err = lua_tostring(L, -1);
            ESP_LOGE(TAG, "Error in HTTP callback: %s", err);
            lua_pop(L, 1);
        }
    } else {
        ESP_LOGE(TAG, "Lua stack overflow when processing HTTP response");
        if (msg->response) {
            free(msg->response);
        }
    }

    // 释放回调引用
    luaL_unref(L, LUA_REGISTRYINDEX, msg->callback_ref);

    if (http_mutex) {
        xSemaphoreGive(http_mutex);
    }
}

// HTTP 消息处理任务
static void http_message_task(void* arg)
{
    http_complete_msg_t msg;

    while (1) {
        // 从队列接收消息，阻塞等待
        if (xQueueReceive(global_http_msg_queue, &msg, portMAX_DELAY) == pdTRUE) {
            // 处理消息
            process_http_message(msg.L, &msg);
        }
    }
}

// HTTP 请求任务
static void http_task(void* arg)
{
    esp_http_client_handle_t client = (esp_http_client_handle_t)arg;
    http_context_t* ctx = NULL;
    void* user_data = NULL;

    // 获取用户数据
    esp_err_t err = esp_http_client_get_user_data(client, &user_data);
    if (err != ESP_OK || !user_data) {
        ESP_LOGE(TAG, "Failed to get user data from HTTP client: %d", err);
        esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }

    ctx = (http_context_t*)user_data;

    // 执行同步请求
    err = esp_http_client_perform(client);

    // 获取响应信息
    int status_code = esp_http_client_get_status_code(client);
    int content_length = esp_http_client_get_content_length(client);

    char* response = NULL;
    size_t response_size = 0;

    // 读取响应数据
    if (content_length > 0) {
        response = malloc(content_length + 1);
        if (response) {
            response_size = esp_http_client_read(client, response, content_length);
            response[response_size] = '\0';
        } else {
            ESP_LOGE(TAG, "Failed to allocate memory for response");
        }
    }

    // 创建完成消息
    http_complete_msg_t msg = {
        .L = ctx->L,
        .callback_ref = ctx->callback_ref,
        .status_code = (err == ESP_OK) ? status_code : -1,
        .response = response,
        .response_size = response_size
    };

    // 发送消息到全局队列
    if (xQueueSend(global_http_msg_queue, &msg, 1000 / portTICK_PERIOD_MS) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to send HTTP response message to queue");
        if (response) {
            free(response);
        }
        if (ctx->callback_ref != LUA_REFNIL) {
            luaL_unref(ctx->L, LUA_REGISTRYINDEX, ctx->callback_ref);
        }
    }

    // 清理资源
    free(ctx);
    esp_http_client_cleanup(client);
    vTaskDelete(NULL);
}

// 通用 HTTP 请求函数
static int l_http_request(lua_State* L)
{
    // 检查模块是否已初始化
    if (http_module_init() != ESP_OK) {
        return luaL_error(L, "Failed to initialize HTTP module");
    }

    // 检查参数是否为表
    luaL_checktype(L, 1, LUA_TTABLE);

    // 获取 URL
    lua_getfield(L, 1, "url");
    const char* url = luaL_checkstring(L, -1);
    lua_pop(L, 1);

    // 获取方法
    lua_getfield(L, 1, "method");
    const char* method = lua_tostring(L, -1);
    if (!method) {
        method = "GET";
    }
    lua_pop(L, 1);

    // 获取请求体
    lua_getfield(L, 1, "data");
    const char* data = lua_tostring(L, -1);
    size_t data_len = data ? strlen(data) : 0;
    lua_pop(L, 1);

    // 获取回调函数
    lua_getfield(L, 1, "callback");
    int callback_ref = LUA_REFNIL;
    if (lua_isfunction(L, -1)) {
        callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    } else {
        lua_pop(L, 1);
        return luaL_error(L, "Callback function is required");
    }

    // 创建 HTTP 上下文
    http_context_t* ctx = malloc(sizeof(http_context_t));
    if (!ctx) {
        if (callback_ref != LUA_REFNIL) {
            luaL_unref(L, LUA_REGISTRYINDEX, callback_ref);
        }
        return luaL_error(L, "Failed to allocate memory for HTTP context");
    }

    memset(ctx, 0, sizeof(http_context_t));
    ctx->L = L;
    ctx->callback_ref = callback_ref;

    // 配置 HTTP 客户端
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = http_event_handler,
        .user_data = ctx,
        .timeout_ms = 10000,
        .disable_auto_redirect = true,
        .is_async = false, // 使用同步模式，通过任务实现异步行为
    };

    // 设置 HTTP 方法
    if (strcmp(method, "POST") == 0) {
        config.method = HTTP_METHOD_POST;
    } else if (strcmp(method, "PUT") == 0) {
        config.method = HTTP_METHOD_PUT;
    } else if (strcmp(method, "DELETE") == 0) {
        config.method = HTTP_METHOD_DELETE;
    } else if (strcmp(method, "HEAD") == 0) {
        config.method = HTTP_METHOD_HEAD;
    } else if (strcmp(method, "OPTIONS") == 0) {
        config.method = HTTP_METHOD_OPTIONS;
    }

    // 创建 HTTP 客户端
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        if (callback_ref != LUA_REFNIL) {
            luaL_unref(L, LUA_REGISTRYINDEX, callback_ref);
        }
        free(ctx);
        return luaL_error(L, "Failed to initialize HTTP client");
    }

    // 设置请求头（如果有）
    lua_getfield(L, 1, "headers");
    if (lua_istable(L, -1)) {
        lua_pushnil(L); // 第一个键
        while (lua_next(L, -2) != 0) {
            // 键在索引 -2，值在索引 -1
            const char* key = lua_tostring(L, -2);
            const char* value = lua_tostring(L, -1);

            if (key && value) {
                esp_http_client_set_header(client, key, value);
            }

            // 弹出值，保留键用于下次迭代
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1); // 弹出 headers 表

    // 设置请求体（如果有）
    if (data_len > 0) {
        esp_http_client_set_post_field(client, data, data_len);
    }

    // 创建 HTTP 请求任务
    if (xTaskCreatePinnedToCore(http_task, "http_task", 4096, client, 5, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create HTTP task");

        // 释放资源
        if (callback_ref != LUA_REFNIL) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, callback_ref);
            lua_pushinteger(L, 0); // 错误状态码
            lua_pushstring(L, "Failed to create task"); // 错误信息
            lua_pcall(L, 2, 0, 0); // 调用回调
            luaL_unref(L, LUA_REGISTRYINDEX, callback_ref);
        }

        esp_http_client_cleanup(client);
        free(ctx);
        return 0;
    }

    return 0;
}

// HTTP GET 请求
static int l_http_get(lua_State* L)
{
    const char* url = luaL_checkstring(L, 1);

    // 创建请求选项表
    lua_newtable(L);

    // 设置 URL
    lua_pushstring(L, url);
    lua_setfield(L, -2, "url");

    // 设置方法
    lua_pushstring(L, "GET");
    lua_setfield(L, -2, "method");

    // 如果提供了 headers 参数
    if (lua_istable(L, 2)) {
        lua_pushvalue(L, 2);
        lua_setfield(L, -2, "headers");
    }

    // 如果提供了 callback 参数
    if (lua_isfunction(L, lua_gettop(L))) {
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, "callback");
    } else {
        return luaL_error(L, "Callback function is required");
    }

    // 调用通用请求函数
    return l_http_request(L);
}

// HTTP POST 请求
static int l_http_post(lua_State* L)
{
    const char* url = luaL_checkstring(L, 1);
    const char* data = luaL_checkstring(L, 2);

    // 创建请求选项表
    lua_newtable(L);

    // 设置 URL
    lua_pushstring(L, url);
    lua_setfield(L, -2, "url");

    // 设置方法
    lua_pushstring(L, "POST");
    lua_setfield(L, -2, "method");

    // 设置数据
    lua_pushstring(L, data);
    lua_setfield(L, -2, "data");

    // 如果提供了 headers 参数
    if (lua_istable(L, 3)) {
        lua_pushvalue(L, 3);
        lua_setfield(L, -2, "headers");
    }

    // 如果提供了 callback 参数
    if (lua_isfunction(L, lua_gettop(L))) {
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, "callback");
    } else {
        return luaL_error(L, "Callback function is required");
    }

    // 调用通用请求函数
    return l_http_request(L);
}

// 注册 HTTP 模块
int luaopen_http(lua_State* L)
{
    // 创建模块表
    lua_newtable(L);

    // 设置模块函数
    lua_pushcfunction(L, l_http_request);
    lua_setfield(L, -2, "request");

    lua_pushcfunction(L, l_http_get);
    lua_setfield(L, -2, "get");

    lua_pushcfunction(L, l_http_post);
    lua_setfield(L, -2, "post");

    return 1;
}

// 初始化 HTTP 模块
void register_lua_http(lua_State* L)
{
    // 确保模块初始化
    if (http_module_init() != ESP_OK) {
        return;
    }

    // 将 HTTP 模块注册到 package.preload
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "preload");

    lua_pushcfunction(L, luaopen_http);
    lua_setfield(L, -2, "http");

    // 弹出 package 和 preload 表
    lua_pop(L, 2);
}