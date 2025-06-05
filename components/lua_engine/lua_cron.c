#include "lua_cron.h"
#include "lauxlib.h"
#include "lua.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 最大支持的 cron 数量
#define MAX_CRON_JOBS 30

typedef struct {
    cron_job* job;
    int lua_ref; // Lua 回调对象引用（RuleContext 实例）
} lua_cron_job_t;

static lua_cron_job_t cron_jobs[MAX_CRON_JOBS];

// 根据 cron id 查找任务
static cron_job* cron_get_job_by_id(int id)
{
    for (int i = 0; i < MAX_CRON_JOBS; ++i) {
        if (cron_jobs[i].job && cron_jobs[i].job->id == id) {
            return cron_jobs[i].job;
        }
    }
    return NULL;
}

// cron 的回调函数，由 esp_cron 调用
static void cron_callback(cron_job* job)
{
    if (!job || !job->data)
        return;

    lua_cron_binding_t* binding = (lua_cron_binding_t*)job->data;
    lua_State* L = binding->L;
    if (!L)
        return;

    lua_rawgeti(L, LUA_REGISTRYINDEX, binding->lua_ref);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }

    lua_getfield(L, -1, "handle_corn");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return;
    }

    lua_pushvalue(L, -2); // self

    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        printf("[cron_callback] handle_corn error: %s\n", err);
        lua_pop(L, 1);
    }

    lua_pop(L, 1); // 弹出 RuleContext
}

// Lua: cron.register_cron(expr, rule_context)
static int l_register_cron(lua_State* L)
{
    const char* schedule = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    lua_cron_binding_t* binding = malloc(sizeof(lua_cron_binding_t));
    if (!binding) {
        return luaL_error(L, "malloc failed");
    }

    binding->L = L;
    lua_pushvalue(L, 2);
    binding->lua_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    cron_job* job = cron_job_create(schedule, cron_callback, (void*)binding);
    if (!job) {
        luaL_unref(L, LUA_REGISTRYINDEX, binding->lua_ref);
        free(binding);
        return luaL_error(L, "cron_job_create failed");
    }

    if (cron_job_schedule(job) != 0) {
        cron_job_destroy(job);
        luaL_unref(L, LUA_REGISTRYINDEX, binding->lua_ref);
        free(binding);
        return luaL_error(L, "cron_job_schedule failed");
    }

    // 插入到管理数组中
    for (int i = 0; i < MAX_CRON_JOBS; ++i) {
        if (!cron_jobs[i].job) {
            cron_jobs[i].job = job;
            cron_jobs[i].lua_ref = binding->lua_ref;
            break;
        }
    }

    lua_pushinteger(L, job->id);
    return 1;
}

// Lua: cron.unregister_cron(job_id)
static int l_unregister_cron(lua_State* L)
{
    int job_id = luaL_checkinteger(L, 1);

    cron_job* job = cron_get_job_by_id(job_id);
    if (!job)
        return 0;

    cron_job_unschedule(job);

    lua_cron_binding_t* binding = (lua_cron_binding_t*)job->data;
    if (binding) {
        luaL_unref(binding->L, LUA_REGISTRYINDEX, binding->lua_ref);
        free(binding);
    }
    job->data = NULL;
    cron_job_destroy(job);

    for (int i = 0; i < MAX_CRON_JOBS; ++i) {
        if (cron_jobs[i].job && cron_jobs[i].job->id == job_id) {
            cron_jobs[i].job = NULL;
            cron_jobs[i].lua_ref = LUA_NOREF;
            break;
        }
    }

    return 0;
}

// Lua: cron.list()
static int l_list_cron_jobs(lua_State* L)
{
    lua_newtable(L);
    int index = 1;

    for (int i = 0; i < MAX_CRON_JOBS; ++i) {
        if (cron_jobs[i].job) {
            lua_newtable(L);
            lua_pushinteger(L, cron_jobs[i].job->id);
            lua_setfield(L, -2, "id");
            lua_pushboolean(L, 1);
            lua_setfield(L, -2, "active");
            lua_rawseti(L, -2, index++);
        }
    }

    return 1;
}

static const struct luaL_Reg cronlib[] = {
    { "register_cron", l_register_cron },
    { "unregister_cron", l_unregister_cron },
    { "list", l_list_cron_jobs },
    { NULL, NULL }
};

void register_lua_cron(lua_State* L)
{
    luaL_newlib(L, cronlib);
    lua_setglobal(L, "cron"); // 让 Lua 中可以直接使用全局变量 cron
}