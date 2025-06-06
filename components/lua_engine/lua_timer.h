#ifndef LUA_TIMER_H
#define LUA_TIMER_H

#include <stdint.h>
#include <lua.h>

// 注册定时器函数（delay/settimeout）
void register_lua_timer(lua_State* L);

// 在主循环中调用此函数处理所有到期的协程
void timer_process(lua_State* L);

#endif // LUA_REG_H

