#ifndef __LUA_EVENT_H__
#define __LUA_EVENT_H__

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 处理所有待处理的事件
 * @param L Lua状态机指针
 */
void event_process(lua_State* L);

/**
 * @brief 从C代码触发事件
 * @param L Lua状态机指针
 * @param event_name 事件名称
 * @param argc 参数数量
 * @param argv 参数数组（字符串类型）
 */
void event_trigger(lua_State* L, const char* event_name, int argc, const char** argv);

/**
 * @brief 注册Lua事件函数（on/trigger）
 * @param L Lua状态机指针
 */
void register_lua_event(lua_State* L);

/**
 * @brief 清理事件系统资源
 */
void event_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif // __LUA_EVENT_H__