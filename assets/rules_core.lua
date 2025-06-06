-- rules_core.lua
-- 规则引擎核心调度模块（文件遍历由 C 端完成，传入规则文件内容数组）

local RuleContext = require("rule_context")
local log = require("log")

local TAG = "RULES_CORE"

local rules_core = {}

local RULES = {}      -- 规则上下文列表（有序）
local RULE_MAP = {}   -- id -> RuleContext 映射，便于快速查找

-- 从字符串加载单个规则
-- rule_code: string，规则lua文件完整代码
local function load_rule_from_string(rule_code)
    if type(rule_code) ~= "string" then
        log.error(TAG, "规则代码类型错误，需要字符串")
        return nil, "规则代码类型错误"
    end
    
    local func, err = load(rule_code)
    if not func then
        log.error(TAG, "规则代码加载失败: %s", err)
        return nil, err
    end

    local ok, rule_def = pcall(func)
    if not ok then
        log.error(TAG, "执行规则代码失败: %s", rule_def)
        return nil, rule_def
    end

    if type(rule_def) ~= "table" or not rule_def.id then
        log.error(TAG, "规则定义无效或缺少id")
        return nil, "规则定义无效或缺少id"
    end

    if RULE_MAP[rule_def.id] then
        log.warn(TAG, "规则ID重复: %s，将覆盖现有规则", rule_def.id)
        rules_core.remove_rule(rule_def.id) -- 自动移除重复规则
    end

    local ctx, err = RuleContext.new(rule_def)
    if not ctx then
        log.error(TAG, "创建规则上下文失败 [%s]: %s", rule_def.id, err)
        return nil, err
    end
    
    table.insert(RULES, ctx)
    RULE_MAP[ctx.id] = ctx
    log.info(TAG, "规则已加载: %s", ctx.id)
    return ctx
end

-- 由 C 端调用，传入规则文件内容数组（字符串列表）
local is_loading_rules = false

function rules_core.load_all_from_strings(rules_code_arr)
    if not rules_code_arr or type(rules_code_arr) ~= "table" then
        log.error(TAG, "load_all_from_strings 参数错误，需要字符串数组")
        return 0, "参数错误"
    end
    
    is_loading_rules = true
    
    -- 清理现有规则
    rules_core.cleanup()
    
    local success_count = 0
    local failed_count = 0
    
    for _, code in ipairs(rules_code_arr) do
        local ctx, err = load_rule_from_string(code)
        if ctx then
            ctx:mounted() -- 挂载规则
            success_count = success_count + 1
        else
            failed_count = failed_count + 1
            log.error(TAG, "规则加载失败: %s", err)
        end
    end

    is_loading_rules = false
    log.info(TAG, "规则加载完成: 成功 %d 条, 失败 %d 条, 总计 %d 条", 
        success_count, failed_count, success_count + failed_count)
    
    return success_count, failed_count
end

-- 加载默认规则
function rules_core.load_default_rules()
    log.info(TAG, "加载默认规则...")
    -- 示例: 可以从内置资源加载默认规则
    -- 实际实现可能依赖于具体项目需求
    return 0
end

-- 定时tick调用，传入当前时间ms，触发规则定时逻辑
function rules_core.tick(now_ms)
    if is_loading_rules then return end
    
    for _, rule in ipairs(RULES) do
        local ok, err = pcall(rule.tick, rule, now_ms)
        if not ok then
            log.error(TAG, "执行规则tick失败 [%s]: %s", rule.id, err)
        end
    end
end

-- 事件派发调用，分发事件给所有规则
function rules_core.dispatch_event(event)
    if is_loading_rules or not event then return end
    
    for _, rule in ipairs(RULES) do
        local ok, err = pcall(rule.handle_event, rule, event)
        if not ok then
            log.error(TAG, "事件处理失败 [%s]: %s", rule.id, err)
        end
    end
end

-- 定时任务调用，触发指定规则的cron逻辑
function rules_core.dispatch_cron(rule_id)
    if is_loading_rules or not rule_id then return end
    
    local rule = RULE_MAP[rule_id]
    if rule then
        local ok, err = pcall(rule.handle_cron, rule)
        if not ok then
            log.error(TAG, "执行cron任务失败 [%s]: %s", rule_id, err)
        end
    else
        log.warn(TAG, "未找到规则ID: %s", rule_id)
    end
end

-- 新增规则（规则定义table格式）
function rules_core.add_rule_from_def(rule_def)
    if not rule_def or type(rule_def) ~= "table" then
        log.error(TAG, "add_rule_from_def 参数错误，需要table")
        return false, "参数必须是table"
    end
    
    if not rule_def.id or type(rule_def.id) ~= "string" then
        log.error(TAG, "规则定义缺少有效的id")
        return false, "规则必须有字符串类型的id"
    end

    if RULE_MAP[rule_def.id] then
        log.warn(TAG, "规则ID已存在，将覆盖: %s", rule_def.id)
        rules_core.remove_rule(rule_def.id)
    end

    local ctx, err = RuleContext.new(rule_def)
    if not ctx then
        log.error(TAG, "创建规则失败 [%s]: %s", rule_def.id, err)
        return false, err
    end
    
    table.insert(RULES, ctx)
    RULE_MAP[ctx.id] = ctx
    ctx:mounted() -- 挂载规则
    
    log.info(TAG, "添加规则成功: %s", ctx.id)
    return true
end

-- 删除规则
function rules_core.remove_rule(rule_id)
    if not rule_id or type(rule_id) ~= "string" then
        log.error(TAG, "remove_rule 参数错误，需要string")
        return false, "参数必须是string"
    end
    
    for i, ctx in ipairs(RULES) do
        if ctx.id == rule_id then
            ctx:destroy()
            table.remove(RULES, i)
            RULE_MAP[rule_id] = nil
            log.info(TAG, "删除规则: %s", rule_id)
            return true
        end
    end
    
    log.warn(TAG, "未找到规则ID: %s", rule_id)
    return false, "规则不存在"
end

-- 获取所有规则id列表
function rules_core.list_rule_ids()
    local ids = {}
    for id in pairs(RULE_MAP) do
        table.insert(ids, id)
    end
    log.debug(TAG, "获取规则列表，共 %d 条规则", #ids)
    return ids
end

-- 获取规则状态
function rules_core.get_rule_status(rule_id)
    local rule = RULE_MAP[rule_id]
    if rule then
        return {
            id = rule.id,
            type = rule.type,
            enabled = rule.enabled,
            state = rule.state
        }
    end
    return nil
end

-- 清理所有资源
function rules_core.cleanup()
    log.info(TAG, "清理所有规则资源...")
    
    -- 销毁所有规则
    for _, ctx in ipairs(RULES) do
        ctx:destroy()
    end
    
    -- 重置数据结构
    RULES = {}
    RULE_MAP = {}
    
    log.info(TAG, "规则资源清理完成")
end

return rules_core
