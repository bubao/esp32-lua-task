#include "lua_engine.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lua_bindings.h"
#include "rom/gpio.h"
#include "soc/gpio_num.h"
#include <dirent.h>
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

static lua_State* L = NULL;
static const char* TAG = "lua_engine";

static int load_main_module(void)
{
    lua_getglobal(L, "require");
    lua_pushstring(L, "main");
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        ESP_LOGE(TAG, "Failed to load main module: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return 0;
    }
    return 1;
}

esp_err_t lua_engine_init(void)
{
    L = luaL_newstate();
    if (!L) {
        ESP_LOGE(TAG, "Failed to create Lua state");
        return ESP_FAIL;
    }

    luaL_openlibs(L);
    register_lua_bindings(L);

    // 设置模块搜索路径
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "path");
    const char* old_path = lua_tostring(L, -1);
    char new_path[512];
    snprintf(new_path, sizeof(new_path), "%s;/assets/?.lua;/assets/?/init.lua", old_path);
    lua_pop(L, 1);
    lua_pushstring(L, new_path);
    lua_setfield(L, -2, "path");
    lua_pop(L, 1);

    // 加载主模块
    if (!load_main_module()) {
        lua_close(L);
        L = NULL;
        return ESP_FAIL;
    }

    return ESP_OK;
}

// 在lua_engine.c文件中添加
lua_State* lua_engine_get_state(void)
{
    return L;
}

void lua_engine_deinit(void)
{
    if (L) {
        // 执行必要的清理操作
        lua_getglobal(L, "package");
        lua_getfield(L, -1, "loaded");
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            // 移除所有已加载的模块
            lua_pushnil(L);
            lua_setfield(L, -4, lua_tostring(L, -2));
            lua_pop(L, 1);
        }
        lua_pop(L, 2); // 弹出 package.loaded 和 package

        lua_close(L);
        L = NULL;
    }
}

void lua_engine_call_init(void)
{
    // 获取并调用 init 函数
    lua_getglobal(L, "require");
    lua_pushstring(L, "main");
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        ESP_LOGE(TAG, "Failed to require 'main': %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }

    lua_getfield(L, -1, "init");
    lua_remove(L, -2); // 移除模块 table

    if (!lua_isfunction(L, -1)) {
        ESP_LOGE(TAG, "init function not found");
        lua_pop(L, 1);
        return;
    }

    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        ESP_LOGE(TAG, "Error calling init: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

esp_err_t lua_engine_send_config(const char* lua_table_str)
{
    // 1. 编译配置字符串
    char wrapped_code[2048];
    snprintf(wrapped_code, sizeof(wrapped_code), "return %s", lua_table_str);

    if (luaL_loadstring(L, wrapped_code) != LUA_OK) {
        ESP_LOGE(TAG, "Failed to compile config string: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return ESP_FAIL;
    }

    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        ESP_LOGE(TAG, "Failed to eval config: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return ESP_FAIL;
    }

    // 此时栈上有一个元素：配置表

    // 2. 保存配置表引用
    int config_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    // 3. 清理栈，确保状态干净
    lua_settop(L, 0);

    // 4. 调用 trigger("config", config_table)
    lua_getglobal(L, "trigger");
    if (!lua_isfunction(L, -1)) {
        ESP_LOGE(TAG, "trigger is not a function! Type: %s", lua_typename(L, -1));
        lua_pop(L, 1);
        luaL_unref(L, LUA_REGISTRYINDEX, config_ref); // 释放引用
        return ESP_FAIL;
    }

    lua_pushstring(L, "config");

    // 从注册表中获取配置表
    lua_rawgeti(L, LUA_REGISTRYINDEX, config_ref);

    // 调用函数
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        ESP_LOGE(TAG, "Error calling trigger: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        luaL_unref(L, LUA_REGISTRYINDEX, config_ref); // 释放引用
        return ESP_FAIL; // 返回错误状态
    }

    // 5. 释放配置表引用（仅在调用成功后释放）
    luaL_unref(L, LUA_REGISTRYINDEX, config_ref);

    ESP_LOGI(TAG, "Device config successfully sent to Lua.");
    return ESP_OK;
}

void lua_engine_send_event(const char* device, const char* type, double value)
{
    // 1. 获取 trigger 函数
    ESP_LOGE(TAG, "lua_getglobal: lua_engine_send_event");
    lua_getglobal(L, "trigger");

    if (!lua_isfunction(L, -1)) {
        ESP_LOGE(TAG, "trigger function not found");
        lua_pop(L, 1);
        return;
    }

    // 2. 创建事件表
    lua_newtable(L);
    lua_pushstring(L, device);
    lua_setfield(L, -2, "device");
    lua_pushstring(L, type);
    lua_setfield(L, -2, "type");
    lua_pushnumber(L, value);
    lua_setfield(L, -2, "value");

    // 3. 调用 trigger("event", event_table)
    lua_pushstring(L, "event");
    lua_insert(L, -2); // 将事件类型移到参数列表前面

    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        ESP_LOGE(TAG, "Error calling trigger: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

void lua_engine_send_rules(const char** rules_code_arr, int count)
{
    // 1. 获取 trigger 函数
    lua_getglobal(L, "trigger");

    if (!lua_isfunction(L, -1)) {
        ESP_LOGE(TAG, "trigger function not found");
        lua_pop(L, 1);
        return;
    }

    // 2. 创建规则表
    lua_newtable(L);
    for (int i = 0; i < count; ++i) {
        lua_pushstring(L, rules_code_arr[i]);
        lua_rawseti(L, -2, i + 1);
    }

    // 3. 调用 trigger("rules", rules_table)
    lua_pushstring(L, "rules");
    lua_insert(L, -2);

    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        ESP_LOGE(TAG, "Error calling trigger: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

void lua_engine_cron_trigger(const char* rule_id)
{
    ESP_LOGE(TAG, "lua_getglobal: lua_engine_cron_trigger");

    // 1. 获取 trigger 函数
    lua_getglobal(L, "trigger");

    if (!lua_isfunction(L, -1)) {
        ESP_LOGE(TAG, "trigger function not found");
        lua_pop(L, 1);
        return;
    }

    // 2. 创建触发表
    lua_newtable(L);
    lua_pushstring(L, rule_id);
    lua_setfield(L, -2, "rule_id");

    // 3. 调用 trigger("cron", {rule_id=rule_id})
    lua_pushstring(L, "cron");
    lua_insert(L, -2);

    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        ESP_LOGE(TAG, "Error calling trigger: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

void lua_engine_add_rule(const char* rule_json)
{
    // 1. 解析 JSON
    lua_getglobal(L, "cjson");
    lua_getfield(L, -1, "decode");
    lua_remove(L, -2);
    lua_pushstring(L, rule_json);
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        ESP_LOGE(TAG, "cjson decode failed: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }

    // 2. 获取 trigger 函数
    ESP_LOGE(TAG, "lua_getglobal: lua_engine_add_rule");

    lua_getglobal(L, "trigger");

    if (!lua_isfunction(L, -1)) {
        ESP_LOGE(TAG, "trigger function not found");
        lua_pop(L, 2); // 弹出 trigger 和 rule_table
        return;
    }

    // 3. 调用 trigger("add_rule", rule_table)
    lua_pushstring(L, "add_rule");
    lua_insert(L, -2);

    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        ESP_LOGE(TAG, "Error calling trigger: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

void lua_engine_remove_rule(const char* rule_id)
{
    // 1. 获取 trigger 函数
    ESP_LOGE(TAG, "lua_getglobal: lua_engine_remove_rule");

    lua_getglobal(L, "trigger");

    if (!lua_isfunction(L, -1)) {
        ESP_LOGE(TAG, "trigger function not found");
        lua_pop(L, 1);
        return;
    }

    // 2. 创建规则 ID 表
    lua_newtable(L);
    lua_pushstring(L, rule_id);
    lua_setfield(L, -2, "rule_id");

    // 3. 调用 trigger("remove_rule", {rule_id=rule_id})
    lua_pushstring(L, "remove_rule");
    lua_insert(L, -2);

    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        ESP_LOGE(TAG, "Error calling trigger: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

// 添加内存释放函数
void lua_engine_free_string(char* str)
{
    free(str);
}

char* lua_engine_list_rules(void)
{
    // 1. 获取并调用 list_rules 函数
    lua_getglobal(L, "require");
    lua_pushstring(L, "main");
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        ESP_LOGE(TAG, "Failed to require 'main': %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return NULL;
    }

    lua_getfield(L, -1, "list_rules");
    lua_remove(L, -2); // 移除模块 table

    if (!lua_isfunction(L, -1)) {
        ESP_LOGE(TAG, "list_rules function not found");
        lua_pop(L, 1);
        return NULL;
    }

    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        ESP_LOGE(TAG, "Error calling list_rules: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return NULL;
    }

    // 2. 转换为 JSON 字符串
    lua_getglobal(L, "cjson");
    lua_getfield(L, -1, "encode");
    lua_remove(L, -2);
    lua_pushvalue(L, -2); // 复制 list_rules 的返回值

    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        ESP_LOGE(TAG, "cjson encode error: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return NULL;
    }

    // 3. 获取并复制 JSON 字符串
    const char* json = lua_tostring(L, -1);
    char* result = strdup(json);
    lua_pop(L, 1); // 清理栈
    return result;
}