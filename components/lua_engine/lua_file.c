#include "lua_file.h"
#include <lauxlib.h>
#include <stdio.h>
#include <string.h>

static int l_file_read(lua_State* L)
{
    const char* filename = luaL_checkstring(L, 1);
    FILE* file = fopen(filename, "rb");
    if (!file) {
        lua_pushnil(L);
        lua_pushfstring(L, "Failed to open file: %s", filename);
        return 2;
    }

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

void register_lua_file(lua_State* L)
{
    lua_register(L, "file.read", l_file_read);
    lua_register(L, "file.write", l_file_write);
}