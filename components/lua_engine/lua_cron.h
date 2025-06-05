#ifndef LUA_CRON_H
#define LUA_CRON_H

#include "lua.h"
#include "cron.h"
typedef struct {
    lua_State* L;
    int lua_ref;  // Registry 中 RuleContext 实例的引用
} lua_cron_binding_t;
#define MAX_CRON_JOBS 30

void register_lua_cron(lua_State* L);

#endif // LUA_CRON_H
