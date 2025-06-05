#include "lua_engine.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
        printf("Error loading main module: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return 0;
    }
    return 1;
}

void lua_engine_init(void)
{
    L = luaL_newstate();
    luaL_openlibs(L);

    lua_getglobal(L, "package");
    lua_getfield(L, -1, "path");
    const char* old_path = lua_tostring(L, -1);
    char new_path[512];
    snprintf(new_path, sizeof(new_path), "%s;/assets/?.lua;/assets/?/init.lua", old_path);
    lua_pop(L, 1);
    lua_pushstring(L, new_path);
    lua_setfield(L, -2, "path");
    lua_pop(L, 1);

    load_main_module();
}

void lua_engine_deinit(void)
{
    if (L) {
        lua_close(L);
        L = NULL;
    }
}

static void call_main_function(const char* fname, int nargs)
{
    load_main_module(); // 栈顶是模块 table
    lua_getfield(L, -1, fname); // 模块 table[fname]
    lua_remove(L, -2); // 移除模块 table

    if (!lua_isfunction(L, -1)) {
        printf("Function %s not found\n", fname);
        lua_pop(L, 1 + nargs);
        return;
    }

    if (lua_pcall(L, nargs, 0, 0) != LUA_OK) {
        printf("Error calling %s: %s\n", fname, lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

void lua_engine_call_init(void)
{
    call_main_function("init", 0);
}

void lua_engine_send_config(const char* lua_table_str)
{
    // 加载 main 模块
    lua_getglobal(L, "require");
    lua_pushstring(L, "main");
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        ESP_LOGE(TAG, "Failed to require 'main': %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }

    // 获取 main.on_config_received 函数
    lua_getfield(L, -1, "on_config_received");
    if (!lua_isfunction(L, -1)) {
        ESP_LOGE(TAG, "on_config_received is not a function");
        lua_pop(L, 2); // pop function + main table
        return;
    }

    lua_remove(L, -2); // 移除 main 模块 table，仅留下函数

    // 构造 Lua 表表达式
    char wrapped_code[2048];
    snprintf(wrapped_code, sizeof(wrapped_code), "return %s", lua_table_str);

    // 编译表
    if (luaL_loadstring(L, wrapped_code) != LUA_OK) {
        ESP_LOGE(TAG, "Failed to compile config string: %s", lua_tostring(L, -1));
        lua_pop(L, 2); // function + error
        return;
    }

    // 执行，获取 config table
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        ESP_LOGE(TAG, "Failed to eval config: %s", lua_tostring(L, -1));
        lua_pop(L, 2); // function + error
        return;
    }

    // 栈上现在：[on_config_received][config_table]
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        ESP_LOGE(TAG, "Error calling on_config_received: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }

    ESP_LOGI(TAG, "Device config successfully sent to Lua.");
}

void lua_engine_send_event(const char* device, const char* type, double value)
{
    load_main_module();
    lua_getfield(L, -1, "on_event");
    lua_remove(L, -2);

    lua_newtable(L);
    lua_pushstring(L, device);
    lua_setfield(L, -2, "device");
    lua_pushstring(L, type);
    lua_setfield(L, -2, "type");
    lua_pushnumber(L, value);
    lua_setfield(L, -2, "value");

    call_main_function("on_event", 1);
}

void lua_engine_send_rules(const char** rules_code_arr, int count)
{
    load_main_module(); // pushes module table on stack

    lua_getfield(L, -1, "on_mqtt_rule_message"); // push function
    if (!lua_isfunction(L, -1)) {
        ESP_LOGE(TAG, "on_mqtt_rule_message not a function");
        lua_pop(L, 2); // pop function or non-function and module table
        return;
    }
    lua_remove(L, -2); // remove module table, leave function at top

    lua_newtable(L);
    for (int i = 0; i < count; ++i) {
        lua_pushstring(L, rules_code_arr[i]);
        lua_rawseti(L, -2, i + 1);
    }
    // Now stack: function at -2, table at -1

    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        ESP_LOGE(TAG, "Error calling on_mqtt_rule_message: %s", err);
        lua_pop(L, 1);
    }
}

void lua_engine_corn_trigger(const char* rule_id)
{
    load_main_module();
    lua_getfield(L, -1, "on_corn_trigger");
    lua_remove(L, -2);

    lua_pushstring(L, rule_id);
    call_main_function("on_corn_trigger", 1);
}

void lua_engine_add_rule(const char* rule_json)
{
    load_main_module();
    lua_getfield(L, -1, "add_rule");
    lua_remove(L, -2);

    lua_getglobal(L, "cjson");
    lua_getfield(L, -1, "decode");
    lua_remove(L, -2);
    lua_pushstring(L, rule_json);
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        printf("cjson decode failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }

    call_main_function("add_rule", 1);
}

void lua_engine_remove_rule(const char* rule_id)
{
    load_main_module();
    lua_getfield(L, -1, "remove_rule");
    lua_remove(L, -2);

    lua_pushstring(L, rule_id);
    call_main_function("remove_rule", 1);
}

char* lua_engine_list_rules(void)
{
    load_main_module();
    lua_getfield(L, -1, "list_rules");
    lua_remove(L, -2);

    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        printf("Error calling list_rules: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return NULL;
    }

    lua_getglobal(L, "cjson");
    lua_getfield(L, -1, "encode");
    lua_remove(L, -2);
    lua_pushvalue(L, -2); // 把 list_rules 的返回值传入 encode

    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        printf("cjson encode error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return NULL;
    }

    const char* json = lua_tostring(L, -1);
    char* result = strdup(json);
    lua_pop(L, 1); // 清理栈
    return result;
}

// LittleFS 文件系统初始化
void init_littlefs()
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/assets",
        .partition_label = "assets",
        .format_if_mount_failed = true
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount or format filesystem: %s", esp_err_to_name(ret));
    } else {
        size_t total = 0, used = 0;
        esp_littlefs_info("assets", &total, &used);
        ESP_LOGI(TAG, "LittleFS mounted: total=%d, used=%d", total, used);
    }
}
