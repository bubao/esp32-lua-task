#ifndef LUA_ENGINE_H
#define LUA_ENGINE_H


// 初始化 Lua 虚拟机并加载 main.lua 模块
void lua_engine_init(void);

// 销毁 Lua 虚拟机
void lua_engine_deinit(void);

// 调用 main.init()
void lua_engine_call_init(void);

// 调用 main.on_config_received(config_table)
void lua_engine_send_config(const char* lua_table_str);

// 调用 main.on_event({ device, type, value })
void lua_engine_send_event(const char *device, const char *type, double value);

// 调用 main.on_mqtt_rule_message(rule_code_array)
void lua_engine_send_rules(const char **rules_code_arr, int count);

// 调用 main.on_corn_trigger(rule_id)
void lua_engine_corn_trigger(const char *rule_id);

// 调用 main.add_rule(parsed_rule_table)
void lua_engine_add_rule(const char *rule_json);

// 调用 main.remove_rule(rule_id)
void lua_engine_remove_rule(const char *rule_id);

// 获取当前规则 ID 列表，返回 JSON 字符串（需要由调用方释放）
char *lua_engine_list_rules(void);

void init_littlefs();
#endif
