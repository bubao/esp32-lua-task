-- main.lua
-- 规则引擎主模块，负责初始化、运行主循环、与 C 层通信

local rules_core = require("rules_core")
local config_loader = require("config_loader")
local device_templates = require("device_templates")


local M = {}

-- 主循环（可选使用，建议 C 层定时调用 tick）
function M.main_loop()
    while true do
        local now_ms = os.time() * 1000
        rules_core.tick(now_ms)

        -- 可扩展：
        -- - 检查 C 层发来的事件队列
        -- - 判断是否有 cron 到期（可由 C 端调用 dispatch_cron）
        -- - mqtt/on_event 统一处理

        -- 为避免占用 CPU，这里可由 C 层通知唤醒，或加 sleep
        os.execute("sleep 1")  -- 或使用 node.timer 等机制
    end
end

-- 接收来自 C 层的 MQTT 消息触发规则加载
-- 参数为：Lua 文件字符串数组
function M.on_mqtt_rule_message(rule_code_arr)
    rules_core.load_all_from_strings(rule_code_arr)
end

-- 接收 C 层下发的 json 数据（已解析成 Lua table）
function M.on_config_received(tbl)
    local mqtt_config, err = config_loader.load(tbl)
    if not mqtt_config then
        print("配置加载失败:", err)
    else
        print("MQTT配置成功:", mqtt_config.host)
    end
end

-- C 层触发事件：sensor 变化、状态变化等
function M.on_event(event)
    rules_core.dispatch_event(event)
end

-- C 层 cron 调度回调（传入匹配的 rule_id）
function M.on_cron_trigger(rule_id)
    rules_core.dispatch_cron(rule_id)
end

-- 添加规则（支持 MQTT 增量添加）
function M.add_rule(rule_def)
    return rules_core.add_rule_from_def(rule_def)
end

-- 删除规则
function M.remove_rule(rule_id)
    return rules_core.remove_rule(rule_id)
end

-- 获取所有已加载规则 ID
function M.list_rules()
    return rules_core.list_rule_ids()
end

return M
