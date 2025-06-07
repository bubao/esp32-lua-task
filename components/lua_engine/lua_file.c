#include "lua_file.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include <dirent.h>
#include <lauxlib.h>
#include <stdio.h>
#include <string.h>

static const char* TAG = "lua_file";

// 读取文件内容
static int l_file_read(lua_State* L)
{
    const char* filename = luaL_checkstring(L, 1);
    FILE* file = fopen(filename, "rb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open file: %s", filename);
        lua_pushnil(L);
        lua_pushfstring(L, "Failed to open file: %s", filename);
        return 2;
    }

    // 获取文件大小
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        lua_pushnil(L);
        lua_pushstring(L, "Failed to determine file size");
        return 2;
    }

    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        lua_pushnil(L);
        lua_pushstring(L, "Failed to determine file size");
        return 2;
    }

    fseek(file, 0, SEEK_SET);

    char* buffer = malloc(size);
    if (!buffer) {
        fclose(file);
        lua_pushnil(L);
        lua_pushstring(L, "Memory allocation failed");
        return 2;
    }

    size_t bytes_read = fread(buffer, 1, size, file);
    fclose(file);

    if (bytes_read != (size_t)size) {
        free(buffer);
        lua_pushnil(L);
        lua_pushstring(L, "Failed to read file completely");
        return 2;
    }

    lua_pushlstring(L, buffer, size);
    free(buffer);
    return 1;
}

// 写入文件
static int l_file_write(lua_State* L)
{
    const char* filename = luaL_checkstring(L, 1);
    size_t content_len;
    const char* content = luaL_checklstring(L, 2, &content_len);

    FILE* file = fopen(filename, "wb");
    if (!file) {
        lua_pushboolean(L, 0);
        lua_pushfstring(L, "Failed to open file: %s", filename);
        return 2;
    }

    size_t bytes_written = fwrite(content, 1, content_len, file);
    int close_status = fclose(file);

    if (bytes_written != content_len) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "Failed to write complete data");
        return 2;
    }

    if (close_status != 0) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "Failed to close file");
        return 2;
    }

    lua_pushboolean(L, 1);
    return 1;
}

// 列出目录内容
static int l_file_listdir(lua_State* L)
{
    const char* path = luaL_checkstring(L, 1);

    DIR* dir = opendir(path);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s", path);
        lua_pushnil(L);
        lua_pushfstring(L, "Failed to open directory: %s", path);
        return 2;
    }

    lua_newtable(L);
    int index = 1;

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        // 跳过.和..目录
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        lua_pushstring(L, entry->d_name);
        lua_rawseti(L, -2, index++);
    }

    closedir(dir);
    return 1;
}

// 检查路径是否存在
static int l_file_exists(lua_State* L)
{
    const char* path = luaL_checkstring(L, 1);

    // 使用fopen尝试打开文件来检查存在性
    FILE* file = fopen(path, "r");
    if (!file) {
        lua_pushboolean(L, 0); // 不存在
        return 1;
    }

    fclose(file);

    // 再次打开以检查是否为目录
    bool is_dir = false;
    DIR* dir = opendir(path);
    if (dir) {
        is_dir = true;
        closedir(dir);
    }

    lua_pushboolean(L, 1); // 存在
    lua_pushboolean(L, is_dir); // 是否为目录
    return 2;
}

// 文件操作函数列表
static const luaL_Reg file_funcs[] = {
    { "read", l_file_read },
    { "write", l_file_write },
    { "listdir", l_file_listdir },
    { "exists", l_file_exists },
    { NULL, NULL }
};

// 注册文件操作模块
int luaopen_file(lua_State* L)
{
    luaL_newlib(L, file_funcs);
    return 1;
}

// 注册文件操作函数（旧方式，保持兼容性）
void register_lua_file(lua_State* L)
{
    // 注册为模块（推荐方式）
    luaL_requiref(L, "file", luaopen_file, 1);
    lua_pop(L, 1); // 弹出模块表

    // 同时注册为全局函数（保持向后兼容性）
    lua_register(L, "file.read", l_file_read);
    lua_register(L, "file.write", l_file_write);
    lua_register(L, "file.listdir", l_file_listdir);
    lua_register(L, "file.exists", l_file_exists);
}