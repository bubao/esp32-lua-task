#pragma once
#include <stdint.h>
#include <lua.h>

// 注册定时器函数（delay/settimeout）
void register_lua_timer(lua_State* L);

// 在主循环中调用此函数处理所有到期的协程
void timer_process(lua_State* L);

// 获取当前时间（微秒）
int64_t get_now_us(void);
