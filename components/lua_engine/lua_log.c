#include "esp_log.h"
#include "lauxlib.h"
#include "lua.h"
#include <stdarg.h>
#include <string.h>

// 日志级别常量
#define LUA_LOG_ERROR 1
#define LUA_LOG_WARN 2
#define LUA_LOG_INFO 3
#define LUA_LOG_DEBUG 4
#define LUA_LOG_VERBOSE 5

// 将Lua参数转换为格式化字符串
static const char* format_lua_args(lua_State* L, int start_idx, char* buffer, size_t buffer_size)
{
    int nargs = lua_gettop(L) - start_idx + 1;
    if (nargs <= 0) {
        return "";
    }

    // 如果只有一个参数且是字符串，直接返回
    if (nargs == 1 && lua_isstring(L, start_idx)) {
        return lua_tostring(L, start_idx);
    }

    // 构建格式化字符串
    size_t offset = 0;
    for (int i = 0; i < nargs; i++) {
        if (i > 0 && offset < buffer_size - 2) {
            buffer[offset++] = ' ';
        }

        if (offset >= buffer_size - 1) {
            break;
        }

        if (lua_isstring(L, start_idx + i)) {
            const char* str = lua_tostring(L, start_idx + i);
            size_t len = strlen(str);
            if (offset + len < buffer_size) {
                memcpy(buffer + offset, str, len);
                offset += len;
            } else {
                size_t remaining = buffer_size - offset - 1;
                memcpy(buffer + offset, str, remaining);
                offset += remaining;
                break;
            }
        } else if (lua_isnumber(L, start_idx + i)) {
            double num = lua_tonumber(L, start_idx + i);
            if ((double)(int)num == num) {
                offset += snprintf(buffer + offset, buffer_size - offset, "%d", (int)num);
            } else {
                offset += snprintf(buffer + offset, buffer_size - offset, "%f", num);
            }
        } else if (lua_isboolean(L, start_idx + i)) {
            const char* bool_str = lua_toboolean(L, start_idx + i) ? "true" : "false";
            size_t len = strlen(bool_str);
            if (offset + len < buffer_size) {
                memcpy(buffer + offset, bool_str, len);
                offset += len;
            } else {
                break;
            }
        } else if (lua_isnil(L, start_idx + i)) {
            const char* nil_str = "nil";
            if (offset + 3 < buffer_size) {
                memcpy(buffer + offset, nil_str, 3);
                offset += 3;
            } else {
                break;
            }
        } else {
            const char* type_str = lua_typename(L, lua_type(L, start_idx + i));
            size_t len = strlen(type_str);
            if (offset + len + 6 < buffer_size) { // 6 = len("[object ") + len("]")
                memcpy(buffer + offset, "[object ", 8);
                offset += 8;
                memcpy(buffer + offset, type_str, len);
                offset += len;
                buffer[offset++] = ']';
            } else {
                break;
            }
        }
    }

    buffer[offset] = '\0';
    return buffer;
}

// 通用日志函数
static int log_generic(lua_State* L, const char* level_tag, esp_log_level_t level)
{
    const char* tag = luaL_checkstring(L, 1);
    char buffer[512];

    const char* message = format_lua_args(L, 2, buffer, sizeof(buffer));

    // 根据日志级别调用对应的ESP_LOG函数
    switch (level) {
    case ESP_LOG_ERROR:
        ESP_LOGE(tag, "%s", message);
        break;
    case ESP_LOG_WARN:
        ESP_LOGW(tag, "%s", message);
        break;
    case ESP_LOG_INFO:
        ESP_LOGI(tag, "%s", message);
        break;
    case ESP_LOG_DEBUG:
        ESP_LOGD(tag, "%s", message);
        break;
    case ESP_LOG_VERBOSE:
        ESP_LOGV(tag, "%s", message);
        break;
    default:
        ESP_LOGI(tag, "%s", message);
        break;
    }

    return 0;
}

// Lua接口: log.error(tag, message, ...)
static int l_log_error(lua_State* L)
{
    return log_generic(L, "ERROR", ESP_LOG_ERROR);
}

// Lua接口: log.warn(tag, message, ...)
static int l_log_warn(lua_State* L)
{
    return log_generic(L, "WARN", ESP_LOG_WARN);
}

// Lua接口: log.info(tag, message, ...)
static int l_log_info(lua_State* L)
{
    return log_generic(L, "INFO", ESP_LOG_INFO);
}

// Lua接口: log.debug(tag, message, ...)
static int l_log_debug(lua_State* L)
{
    return log_generic(L, "DEBUG", ESP_LOG_DEBUG);
}

// Lua接口: log.verbose(tag, message, ...)
static int l_log_verbose(lua_State* L)
{
    return log_generic(L, "VERBOSE", ESP_LOG_VERBOSE);
}

// Lua接口: log.set_level(tag, level)
static int l_log_set_level(lua_State* L)
{
    const char* tag = luaL_checkstring(L, 1);
    int level = luaL_checkinteger(L, 2);

    // 确保级别在有效范围内
    if (level < ESP_LOG_NONE || level > ESP_LOG_VERBOSE) {
        return luaL_error(L, "无效的日志级别: %d", level);
    }

    esp_log_level_set(tag, (esp_log_level_t)level);
    return 0;
}

// 注册Lua日志模块
LUAMOD_API int luaopen_log(lua_State* L)
{
    static const luaL_Reg log_funcs[] = {
        { "error", l_log_error },
        { "warn", l_log_warn },
        { "info", l_log_info },
        { "debug", l_log_debug },
        { "verbose", l_log_verbose },
        { "set_level", l_log_set_level },
        { NULL, NULL }
    };

    // 创建日志模块表
    luaL_newlib(L, log_funcs);

    // 添加日志级别常量
    lua_pushinteger(L, ESP_LOG_ERROR);
    lua_setfield(L, -2, "ERROR");

    lua_pushinteger(L, ESP_LOG_WARN);
    lua_setfield(L, -2, "WARN");

    lua_pushinteger(L, ESP_LOG_INFO);
    lua_setfield(L, -2, "INFO");

    lua_pushinteger(L, ESP_LOG_DEBUG);
    lua_setfield(L, -2, "DEBUG");

    lua_pushinteger(L, ESP_LOG_VERBOSE);
    lua_setfield(L, -2, "VERBOSE");

    return 1;
}

void register_lua_log(lua_State* L)
{
    // 获取 package 表
    lua_getglobal(L, "package");

    // 获取 package.preload 表
    lua_getfield(L, -1, "preload");

    // 将 log 模块注册到 preload 表中
    lua_pushcfunction(L, luaopen_log);
    lua_setfield(L, -2, "log");

    // 弹出 package 和 preload 表
    lua_pop(L, 2);
}