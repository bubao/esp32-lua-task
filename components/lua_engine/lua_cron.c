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

// 根据 cron id 查找任务，返回管理结构指针，方便操作lua_ref等
static lua_cron_job_t* cron_get_job_by_id(int id)
{
    for (int i = 0; i < MAX_CRON_JOBS; ++i) {
        if (cron_jobs[i].job && cron_jobs[i].job->id == id) {
            return &cron_jobs[i];
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

    // 保护栈，防止异常导致栈失衡
    int top = lua_gettop(L);

    lua_rawgeti(L, LUA_REGISTRYINDEX, binding->lua_ref);
    if (!lua_istable(L, -1)) {
        lua_settop(L, top);
        return;
    }

    lua_getfield(L, -1, "handle_cron");
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, top);
        return;
    }

    lua_pushvalue(L, -2); // self (rule_context)

    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        printf("[cron_callback] handle_cron error: %s\n", err);
        lua_pop(L, 1);
    }

    // 恢复栈顶
    lua_settop(L, top);
}

// Lua: cron.register_cron(expr, rule_context, [config])
static int l_register_cron(lua_State* L)
{
    const char* schedule = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE); // rule_context

    int has_config = !lua_isnoneornil(L, 3);

    lua_cron_binding_t* binding = malloc(sizeof(lua_cron_binding_t));
    if (!binding) {
        return luaL_error(L, "malloc failed");
    }

    binding->L = L;

    lua_pushvalue(L, 2); // rule_context
    binding->lua_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    // 调用 rule_context:on_init(config)
    lua_rawgeti(L, LUA_REGISTRYINDEX, binding->lua_ref); // push rule_context table
    lua_getfield(L, -1, "on_init"); // push on_init function

    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, -2); // push self (rule_context)
        if (has_config) {
            lua_pushvalue(L, 3); // push config table
        } else {
            lua_pushnil(L);
        }

        if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
            const char* err = lua_tostring(L, -1);
            lua_pop(L, 1);
            luaL_unref(L, LUA_REGISTRYINDEX, binding->lua_ref);
            free(binding);
            return luaL_error(L, "call on_init failed: %s", err);
        }
    } else {
        lua_pop(L, 1); // pop non-function
    }
    lua_pop(L, 1); // pop rule_context table

    // 创建 cron job
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

    // 记录管理
    int found_slot = 0;
    for (int i = 0; i < MAX_CRON_JOBS; ++i) {
        if (!cron_jobs[i].job) {
            cron_jobs[i].job = job;
            cron_jobs[i].lua_ref = binding->lua_ref;
            found_slot = 1;
            break;
        }
    }
    if (!found_slot) {
        // 没有空位，清理并返回错误
        cron_job_unschedule(job);
        cron_job_destroy(job);
        luaL_unref(L, LUA_REGISTRYINDEX, binding->lua_ref);
        free(binding);
        return luaL_error(L, "cron job slots full");
    }

    lua_pushinteger(L, job->id);
    return 1;
}

// Lua: cron.unregister_cron(job_id)
static int l_unregister_cron(lua_State* L)
{
    int job_id = luaL_checkinteger(L, 1);

    lua_cron_job_t* entry = cron_get_job_by_id(job_id);
    if (!entry)
        return 0;

    cron_job_unschedule(entry->job);

    lua_cron_binding_t* binding = (lua_cron_binding_t*)entry->job->data;
    if (binding) {
        luaL_unref(binding->L, LUA_REGISTRYINDEX, binding->lua_ref);
        free(binding);
    }
    entry->job->data = NULL;
    cron_job_destroy(entry->job);

    entry->job = NULL;
    entry->lua_ref = LUA_NOREF;

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
