-- main.lua
-- 规则引擎主模块，负责初始化、运行主循环、与 C 层通信

local rules_core = require("rules_core")
local config_loader = require("config_loader")
local log = require("log")

local TAG = "RULE_ENGINE"

local M = {}


-- 初始化引擎
function M.init()
    log.info(TAG, "规则引擎初始化中...")
    -- 注册事件处理函数
    M.register_event_handlers()
    -- 加载默认配置
    local ok, err = config_loader.load_default()
    if not ok then
        log.error(TAG, "加载默认配置失败: " .. err)
    else
        log.info(TAG, "默认配置加载成功")
    end
    
    -- 加载默认规则
    rules_core.load_default_rules()
    
    log.info(TAG, "规则引擎初始化完成")
    return true
end

-- C 层定时调用，处理周期性任务
function M.tick(now_ms)
    rules_core.tick(now_ms)
end

-- 注册事件处理函数
function M.register_event_handlers()
    -- 注册配置事件处理函数
    on("config", function(config_data)
        log.info(TAG, "收到配置事件，类型: " .. type(config_data))
        
        -- 处理配置数据
        if type(config_data) == "table" then
            for key, value in pairs(config_data) do
                log.info(TAG, "配置项 %s: %s", tostring(key), tostring(value))
            end
        end
        
        -- 更新规则引擎配置
        rules_core.update_config(config_data)
    end)
    
    -- 注册规则事件处理函数
    on("rules", function(rules_data)
        log.info(TAG, "收到规则事件，类型: " .. type(rules_data))
        
        -- 处理规则数据
        if type(rules_data) == "table" then
            for i, rule in ipairs(rules_data) do
                log.info(TAG, "规则 %d: %s", i, rule.name or "未命名规则")
            end
        end
        
        -- 应用新规则
        rules_core.apply_rules(rules_data)
    end)
    
    log.info(TAG, "事件处理函数注册完成")
end

-- 接收来自 C 层的 MQTT 消息触发规则加载
-- 参数为：Lua 文件字符串数组
function M.on_mqtt_rule_message(rule_code_arr)
    log.info(TAG, "收到规则更新消息，共 %d 条规则", #rule_code_arr)
    
    local success, failed = rules_core.load_all_from_strings(rule_code_arr)
    log.info(TAG, "规则更新完成: 成功 %d 条, 失败 %d 条", success, failed)
end

-- 接收 C 层下发的 json 数据（已解析成 Lua table）
function M.on_config_received(tbl)
    local config, err = config_loader.load(tbl)
    if not config then
        log.error(TAG, "配置加载失败: " .. err)
        return false
    end
    
    log.info(TAG, "配置加载成功: %s", config.name or "未命名配置")
    
    -- 可扩展：应用配置变更
    return true
end

-- C 层触发事件：sensor 变化、状态变化等
function M.on_event(event)
    if not event then
        log.warn(TAG, "收到空事件，忽略")
        return
    end
    
    -- 记录重要事件
    if event.type == "system" or event.level == "warning" then
        log.info(TAG, "事件触发: %s [%s]", event.type or "unknown", event.source or "unknown")
    end
    
    rules_core.dispatch_event(event)
end

-- C 层 cron 调度回调（传入匹配的 rule_id）
function M.on_cron_trigger(rule_id)
    if not rule_id then
        log.error(TAG, "on_cron_trigger 缺少 rule_id 参数")
        return
    end
    
    rules_core.dispatch_cron(rule_id)
end

-- 添加规则（支持 MQTT 增量添加）
function M.add_rule(rule_def)
    if not rule_def or type(rule_def) ~= "table" then
        log.error(TAG, "add_rule 参数无效")
        return false, "参数必须是表"
    end
    
    local rule_id = rule_def.id
    if not rule_id or type(rule_id) ~= "string" then
        log.error(TAG, "规则定义缺少有效的 id")
        return false, "规则必须有字符串类型的 id"
    end
    
    local ok, err = rules_core.add_rule_from_def(rule_def)
    if ok then
        log.info(TAG, "规则添加成功: %s", rule_id)
        return true
    else
        log.error(TAG, "规则添加失败 [%s]: %s", rule_id, err)
        return false, err
    end
end

-- 删除规则
function M.remove_rule(rule_id)
    if not rule_id or type(rule_id) ~= "string" then
        log.error(TAG, "remove_rule 参数无效")
        return false, "rule_id 必须是字符串"
    end
    
    local ok, err = rules_core.remove_rule(rule_id)
    if ok then
        log.info(TAG, "规则删除成功: %s", rule_id)
        return true
    else
        log.error(TAG, "规则删除失败 [%s]: %s", rule_id, err)
        return false, err
    end
end

-- 获取所有已加载规则 ID
function M.list_rules()
    local rule_ids = rules_core.list_rule_ids()
    log.debug(TAG, "获取规则列表，共 %d 条规则", #rule_ids)
    return rule_ids
end

-- 清理资源
function M.cleanup()
    log.info(TAG, "规则引擎正在清理资源...")
    rules_core.cleanup()
    log.info(TAG, "规则引擎资源清理完成")
end

M.init()

return M
