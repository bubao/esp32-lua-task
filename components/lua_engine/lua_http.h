#ifndef __LUA_HTTP_H__
#define __LUA_HTTP_H__

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 注册 HTTP 模块到 Lua
 * @param L Lua 状态机指针
 * @return 模块表的数量（通常为 1）
 */
int luaopen_http(lua_State* L);


/**
 * @brief 注册Lua事件函数（on/trigger）
 * @param L Lua状态机指针
 */
void register_lua_http(lua_State* L);

#ifdef __cplusplus
}
#endif

#endif // __LUA_HTTP_H__