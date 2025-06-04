#include "lua_functions.h"
#include "lua_bindings.h"

#include "dirent.h"
#include "driver/adc.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_spi_flash.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <dirent.h>
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

static const char* TAG = "lua_system";
LuaCoroutine coroutines[MAX_COROUTINES] = { 0 }; // 这里定义变量
int coroutine_count = 0;
#define MAX_PATH_LEN 256

lua_State* global_L = NULL; // 主 Lua 状态机

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

// 清空协程数组
static void clear_coroutines()
{
    for (int i = 0; i < MAX_COROUTINES; i++) {
        coroutines[i].co = NULL;
    }
    coroutine_count = 0;
}

LuaCoroutine* find_coroutine(lua_State* co)
{
    for (int i = 0; i < coroutine_count; i++) {
        if (coroutines[i].co == co) {
            return &coroutines[i];
        }
    }
    return NULL;
}

static bool load_lua_rule_with_setup(lua_State* L, const char* path)
{
    lua_State* co = lua_newthread(L);
    if (!co) {
        ESP_LOGE(TAG, "Failed to create Lua thread for %s", path);
        return false;
    }

    // 在 co 上注册 settimeout/delay，闭包 upvalue 绑定 co
    ESP_LOGI("LUA", "Register settimeout/delay for co=%p", co);
    lua_pushthread(co);
    lua_pushcclosure(co, l_settimeout, 1);
    lua_setglobal(co, "settimeout");
    lua_pushthread(co);
    lua_pushcclosure(co, l_delay, 1);
    lua_setglobal(co, "delay");

    // 加载 Lua 文件为函数（不会执行）
    if (luaL_loadfile(co, path) != LUA_OK) {
        ESP_LOGE(TAG, "Failed to load %s: %s", path, lua_tostring(co, -1));
        lua_pop(L, 1); // 弹出错误信息
        return false;
    }

    // 直接执行 chunk，不做 _ENV 隔离
    if (lua_pcall(co, 0, 1, 0) != LUA_OK) {
        ESP_LOGE(TAG, "Failed to run %s: %s", path, lua_tostring(co, -1));
        lua_pop(co, 1); // 弹出错误信息
        return false;
    }

    // 现在栈顶是返回的 table，获取其中的 setup() 函数
    if (!lua_istable(co, -1)) {
        ESP_LOGE(TAG, "%s did not return a table", path);
        lua_pop(co, 1);
        return false;
    }
    // 保存 table 到全局，区分每个文件
    char rule_table_name[64];
    snprintf(rule_table_name, sizeof(rule_table_name), "__lua_rule_table_%d", coroutine_count);
    lua_pushvalue(co, -1);
    lua_setglobal(co, rule_table_name);

    lua_getfield(co, -1, "setup");
    if (lua_isfunction(co, -1)) {
        if (lua_pcall(co, 0, 0, 0) != LUA_OK) {
            ESP_LOGE(TAG, "setup error in %s: %s", path, lua_tostring(co, -1));
            lua_pop(co, 1); // 弹出错误
            lua_pop(co, 1); // 弹出 table
            return false;
        }
    } else {
        lua_pop(co, 1); // 不是函数就弹掉
    }

    // 获取 loop() 作为主协程函数
    lua_getfield(co, -1, "loop");
    if (lua_isfunction(co, -1)) {
        // 启动 loop，主动 resume 一次，让协程进入 yield 状态
        int nresults = 0;
        int status = lua_resume(co, NULL, 0, &nresults);
        if (status != LUA_YIELD && status != LUA_OK) {
            ESP_LOGE(TAG, "Failed to start loop: %s", lua_tostring(co, -1));
            lua_pop(co, 1);
            lua_pop(co, 1);
            return false;
        }
        lua_pop(co, 1); // 弹出 loop
        // 保存 co 到 coroutines[]
        if (coroutine_count < MAX_COROUTINES) {
            coroutines[coroutine_count].co = co;
            strncpy(coroutines[coroutine_count].rule_table_name, rule_table_name, sizeof(coroutines[coroutine_count].rule_table_name));
            coroutines[coroutine_count].is_active = 1;
            coroutines[coroutine_count].wake_up_time_us = 0;
            coroutine_count++;
            ESP_LOGI(TAG, "Loaded Lua rule: %s", path);
        }
        lua_pop(co, 1); // 弹出 table
        return true;
    } else {
        ESP_LOGW(TAG, "loop() function not found in %s, skip coroutine", path);
        lua_pop(co, 2); // 弹出非函数和 table
        return true;
    }
}

static void load_lua_rules(lua_State* L, const char* dir)
{
    DIR* d = opendir(dir);
    if (!d) {
        ESP_LOGE(TAG, "Failed to open directory: %s", dir);
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
        if (strstr(entry->d_name, ".lua")) {
            char filepath[MAX_PATH_LEN] = { 0 };
            // 先复制目录
            strlcpy(filepath, dir, sizeof(filepath));

            // 添加结尾斜杠（若没有）
            size_t len = strlen(filepath);
            if (filepath[len - 1] != '/') {
                strlcat(filepath, "/", sizeof(filepath));
            }

            // 添加文件名
            strlcat(filepath, entry->d_name, sizeof(filepath));

            if (strlen(filepath) >= MAX_PATH_LEN - 1) {
                ESP_LOGW(TAG, "路径过长，跳过: %s/%s", dir, entry->d_name);
                continue;
            }
            ESP_LOGI(TAG, "加载 Lua 文件: %s", filepath);
            load_lua_rule_with_setup(L, filepath);
        }
    }
    closedir(d);
}

static lua_State* init_lua()
{
    lua_State* L = luaL_newstate();
    if (!L) {
        ESP_LOGE(TAG, "Failed to create Lua state");
        return NULL;
    }
    luaL_openlibs(L);

    register_lua_bindings(L);

    return L;
}

void start_lua_system()
{
    if (global_L) {
        ESP_LOGW(TAG, "Lua system already started");
        return;
    }

    ESP_LOGI(TAG, "Starting Lua system...");

    global_L = init_lua();
    if (!global_L) {
        ESP_LOGE(TAG, "Failed to initialize Lua");
        return;
    }

    clear_coroutines();
    load_lua_rules(global_L, "/assets");

    ESP_LOGI(TAG, "Lua system started");
}
