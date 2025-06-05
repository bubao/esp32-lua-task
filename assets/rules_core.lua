-- rules_core.lua
-- 规则引擎核心调度模块（文件遍历由 C 端完成，传入规则文件内容数组）

local RuleContext = require("rule_context")

local rules_core = {}

local RULES = {}      -- 规则上下文列表（有顺序）
local RULE_MAP = {}   -- id -> RuleContext 映射，便于快速查找

local function log(tag, msg)
    print(string.format("[rules_core][%s] %s", tag, msg))
end

-- 从字符串加载单个规则
-- rule_code: string，规则lua文件完整代码
local function load_rule_from_string(rule_code)
    local func, err = load(rule_code)
    if not func then
        log("load_error", "规则代码加载失败: " .. err)
        return nil, err
    end

    local ok, rule_def = pcall(func)
    if not ok then
        log("load_error", "执行规则代码失败: " .. tostring(rule_def))
        return nil, tostring(rule_def)
    end

    if type(rule_def) ~= "table" or not rule_def.id then
        log("invalid_rule", "规则定义无效或缺少id")
        return nil, "规则定义无效或缺少id"
    end

    if RULE_MAP[rule_def.id] then
        log("duplicate_rule", "规则ID重复: " .. rule_def.id)
        return nil, "规则ID重复"
    end

    local ctx = RuleContext.new(rule_def)
    table.insert(RULES, ctx)
    RULE_MAP[ctx.id] = ctx
    log("loaded", "规则已加载: " .. ctx.id)
    return ctx
end

-- 由 C 端调用，传入规则文件内容数组（字符串列表）
local is_loading_rules = false

function rules_core.load_all_from_strings(rules_code_arr)
    is_loading_rules = true
    RULES = {}
    RULE_MAP = {}

    for _, code in ipairs(rules_code_arr) do
        local _, err = load_rule_from_string(code)
        if err then
            log("load_warn", "部分规则加载失败: " .. err)
        end
    end

    is_loading_rules = false
    log("load_done", "总共加载规则数量: " .. tostring(#RULES))
end

-- 定时tick调用，传入当前时间ms，触发规则定时逻辑
function rules_core.tick(now_ms)
    for _, rule in ipairs(RULES) do
        rule:tick(now_ms)
    end
end

-- 事件派发调用，分发事件给所有规则
function rules_core.dispatch_event(event)
    for _, rule in ipairs(RULES) do
        rule:handle_event(event)
    end
end

-- 定时任务调用，触发指定规则的corn逻辑
function rules_core.dispatch_corn(rule_id)
    local rule = RULE_MAP[rule_id]
    if rule then
        rule:handle_corn()
    else
        log("corn_error", "未找到规则ID: " .. tostring(rule_id))
    end
end

-- 新增规则（规则定义table格式）
function rules_core.add_rule_from_def(rule_def)
    if not rule_def or not rule_def.id then
        log("add_fail", "规则定义缺少id")
        return false, "规则定义缺少id"
    end

    if RULE_MAP[rule_def.id] then
        log("add_fail", "规则ID已存在: " .. rule_def.id)
        return false, "规则ID已存在"
    end

    local ctx = RuleContext.new(rule_def)
    table.insert(RULES, ctx)
    RULE_MAP[ctx.id] = ctx
    log("add", "添加规则成功: " .. ctx.id)
    return true
end

-- 删除规则
function rules_core.remove_rule(rule_id)
    for i, ctx in ipairs(RULES) do
        if ctx.id == rule_id then
            ctx:destroy()
            table.remove(RULES, i)
            RULE_MAP[rule_id] = nil
            log("remove", "删除规则: " .. rule_id)
            return true
        end
    end
    log("remove_fail", "未找到规则ID: " .. tostring(rule_id))
    return false, "规则不存在"
end

-- 获取所有规则id列表
function rules_core.list_rule_ids()
    local ids = {}
    for id in pairs(RULE_MAP) do
        table.insert(ids, id)
    end
    return ids
end

return rules_core
