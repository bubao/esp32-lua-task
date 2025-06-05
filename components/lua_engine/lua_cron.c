#include "lua_cron.h"
#include "lauxlib.h"
#include "lua.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CRON_JOBS 30

typedef struct {
    cron_job* job;
    char* rule_id; // 任务ID字符串
    int callback_ref; // Lua中回调函数引用
    int config_ref; // Lua中config表引用，可能为LUA_NOREF
    lua_State* L; // lua状态机指针
} lua_cron_job_t;

static lua_cron_job_t cron_jobs[MAX_CRON_JOBS] = { 0 };

static lua_cron_job_t* cron_get_job_by_id(const char* rule_id)
{
    for (int i = 0; i < MAX_CRON_JOBS; ++i) {
        if (cron_jobs[i].job && cron_jobs[i].rule_id && strcmp(cron_jobs[i].rule_id, rule_id) == 0) {
            return &cron_jobs[i];
        }
    }
    return NULL;
}

// cron回调，由esp_cron调用
static void cron_callback(cron_job* job)
{
    if (!job || !job->data)
        return;

    lua_cron_job_t* entry = (lua_cron_job_t*)job->data;
    lua_State* L = entry->L;
    if (!L)
        return;

    printf("[cron_callback] Triggered job '%s' (id=%d)\n", entry->rule_id, job->id);

    int top = lua_gettop(L);

    // 推入回调函数
    lua_rawgeti(L, LUA_REGISTRYINDEX, entry->callback_ref);
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, top);
        printf("[cron_callback] Callback is not a function\n");
        return;
    }

    // 参数1: rule_id
    lua_pushstring(L, entry->rule_id);

    // 参数2: config 表 或 nil
    if (entry->config_ref != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, entry->config_ref);
    } else {
        lua_pushnil(L);
    }

    // 调用 callback(rule_id, config)
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        printf("[cron_callback] callback error: %s\n", err);
        lua_pop(L, 1);
    }

    lua_settop(L, top);
}

// Lua接口：cron.register_cron(rule_id, schedule, callback, config)
static int l_register_cron(lua_State* L)
{
    const char* rule_id = luaL_checkstring(L, 1);
    const char* schedule = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION); // callback
    int has_config = !lua_isnoneornil(L, 4);

    // 检查是否已有同id任务，防止重复注册
    if (cron_get_job_by_id(rule_id)) {
        return luaL_error(L, "rule_id '%s' already registered", rule_id);
    }

    // 申请管理槽位
    int slot = -1;
    for (int i = 0; i < MAX_CRON_JOBS; ++i) {
        if (cron_jobs[i].job == NULL) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        return luaL_error(L, "cron job slots full");
    }

    // 复制rule_id字符串
    char* rule_id_copy = strdup(rule_id);
    if (!rule_id_copy) {
        return luaL_error(L, "malloc failed");
    }

    // 引用 callback
    lua_pushvalue(L, 3);
    int callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    // 引用 config（可选）
    int config_ref = LUA_NOREF;
    if (has_config) {
        lua_pushvalue(L, 4);
        config_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    // 创建 cron job
    cron_job* job = cron_job_create(schedule, cron_callback, NULL);
    if (!job) {
        free(rule_id_copy);
        luaL_unref(L, LUA_REGISTRYINDEX, callback_ref);
        if (config_ref != LUA_NOREF)
            luaL_unref(L, LUA_REGISTRYINDEX, config_ref);
        return luaL_error(L, "cron_job_create failed");
    }
    job->data = &cron_jobs[slot]; // 绑定数据

    int schedule_ret = cron_job_schedule(job);
    if (schedule_ret != 0) {
        cron_job_destroy(job);
        free(rule_id_copy);
        luaL_unref(L, LUA_REGISTRYINDEX, callback_ref);
        if (config_ref != LUA_NOREF)
            luaL_unref(L, LUA_REGISTRYINDEX, config_ref);
        printf("[cron] cron_job_schedule failed, ret=%d\n", schedule_ret);
        return luaL_error(L, "cron_job_schedule failed");
    }

    // 保存管理信息
    cron_jobs[slot].job = job;
    cron_jobs[slot].rule_id = rule_id_copy;
    cron_jobs[slot].callback_ref = callback_ref;
    cron_jobs[slot].config_ref = config_ref;
    cron_jobs[slot].L = L;

    printf("[cron] Registered cron job '%s' with schedule '%s', job_id=%d\n",
        rule_id_copy, schedule, job->id);

    lua_pushinteger(L, job->id);
    return 1;
}

// Lua接口：cron.unregister_cron(rule_id)
static int l_unregister_cron(lua_State* L)
{
    const char* rule_id = luaL_checkstring(L, 1);

    lua_cron_job_t* entry = cron_get_job_by_id(rule_id);
    if (!entry) {
        lua_pushboolean(L, 0);
        return 1;
    }

    cron_job_unschedule(entry->job);

    free(entry->rule_id);
    entry->rule_id = NULL;

    luaL_unref(L, LUA_REGISTRYINDEX, entry->callback_ref);
    entry->callback_ref = LUA_NOREF;

    if (entry->config_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, entry->config_ref);
        entry->config_ref = LUA_NOREF;
    }

    cron_job_destroy(entry->job);
    entry->job = NULL;
    entry->L = NULL;

    lua_pushboolean(L, 1);
    return 1;
}

// Lua接口：cron.list()
static int l_list_cron_jobs(lua_State* L)
{
    lua_newtable(L);
    int index = 1;
    for (int i = 0; i < MAX_CRON_JOBS; ++i) {
        if (cron_jobs[i].job) {
            lua_newtable(L);

            lua_pushstring(L, cron_jobs[i].rule_id);
            lua_setfield(L, -2, "rule_id");

            lua_pushinteger(L, cron_jobs[i].job->id);
            lua_setfield(L, -2, "job_id");

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
    lua_setglobal(L, "cron");
}
