// lua_timer.h
#ifndef LUA_TIMER_H
#define LUA_TIMER_H

#include "lua.h"
extern lua_State* global_L;
/**
 * 初始化定时器系统
 */
void timer_system_init(void);

/**
 * 处理所有到期的定时器任务
 * @param L Lua状态机
 */
void timer_process(lua_State* L);

/**
 * 清理所有定时器资源
 */
void timer_cleanup(void);

/**
 * 关闭定时器模块，释放所有资源
 */
void timer_shutdown(void);

/**
 * 注册timer模块到Lua环境
 * @param L Lua状态机
 * @return 模块函数数量
 */
int luaopen_timer(lua_State* L);

/**
 * 注册定时器API到Lua
 * @param L Lua状态机
 */
void register_lua_timer(lua_State* L);

#endif // LUA_TIMER_H