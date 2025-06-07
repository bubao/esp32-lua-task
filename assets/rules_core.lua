-- rules_core.lua
-- 规则引擎核心调度模块（文件遍历由 C 端完成，传入规则文件内容数组）

local RuleContext = require("rule_context")
local log = require("log")
local file = require("file") -- 引入文件操作模块

local TAG = "RULES_CORE"
local RULES_DIR = "assets/rules"  -- 规则文件目录

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

-- 加载默认规则（从文件系统读取）
function rules_core.load_default_rules()
    log.info(TAG, "加载默认规则...")
    
    -- 检查规则目录是否存在
    local exists, is_dir = file.exists(RULES_DIR)
    if not exists then
        log.info(TAG, "规则目录不存在: %s，使用内置默认规则", RULES_DIR)
        return rules_core.load_builtin_rules()
    end
    
    if not is_dir then
        log.error(TAG, "规则路径不是目录: %s", RULES_DIR)
        return 0, "规则路径不是目录"
    end
    
    -- 读取目录内容
    local files, err = file.listdir(RULES_DIR)
    if not files then
        log.error(TAG, "读取规则目录失败: %s", err or "未知错误")
        return rules_core.load_builtin_rules()
    end
    
    -- 过滤出 .lua 文件
    local rule_files = {}
    for _, file_name in ipairs(files) do
        if file_name:match("%.lua$") then
            table.insert(rule_files, file_name)
        end
    end
    
    -- 如果没有规则文件，使用内置默认规则
    if #rule_files == 0 then
        log.info(TAG, "规则目录中没有找到 .lua 文件，使用内置默认规则")
        return rules_core.load_builtin_rules()
    end
    
    log.info(TAG, "找到 %d 个规则文件", #rule_files)
    
    -- 读取所有规则文件内容
    local rule_contents = {}
    for _, file_name in ipairs(rule_files) do
        local full_path = RULES_DIR .. "/" .. file_name
        local content, read_err = file.read(full_path)
        
        if content then
            table.insert(rule_contents, content)
            log.info(TAG, "成功读取规则文件: %s", full_path)
        else
            log.error(TAG, "读取规则文件失败: %s，错误: %s", full_path, read_err)
        end
    end
    
    -- 如果没有成功读取任何规则文件，使用内置默认规则
    if #rule_contents == 0 then
        log.error(TAG, "未能从文件系统加载任何规则，使用内置默认规则")
        return rules_core.load_builtin_rules()
    end
    
    -- 加载规则内容
    return rules_core.load_all_from_strings(rule_contents)
end

-- 加载内置默认规则
function rules_core.load_builtin_rules()
    log.info(TAG, "加载内置默认规则...")
    
    -- 使用硬编码的示例规则
    local default_rules = {
        -- 默认规则1: 系统状态监控
        [[
            return {
                id = "system_monitor",
                name = "系统状态监控",
                description = "监控系统状态并记录日志",
                type = "system",
                
                -- 初始化函数
                init = function(ctx)
                    log.info("RULE", "系统监控规则已初始化")
                    return true
                end,
                
                -- 事件处理函数
                handle_event = function(ctx, event)
                    if event.type == "system" then
                        log.info("RULE", "系统事件: %s", event.message or "未知事件")
                    end
                end,
                
                -- 定时任务
                cron = {
                    "*/5 * * * *",  -- 每5分钟执行一次
                    function(ctx)
                        log.info("RULE", "系统监控规则定时执行")
                        -- 执行系统监控逻辑
                    end
                }
            }
        ]],
        
        -- 默认规则2: 设备状态检查
        [[
            return {
                id = "device_check",
                name = "设备状态检查",
                description = "定期检查设备状态",
                type = "device",
                
                init = function(ctx)
                    log.info("RULE", "设备检查规则已初始化")
                    return true
                end,
                
                cron = {
                    "*/10 * * * *",  -- 每10分钟执行一次
                    function(ctx)
                        -- 使用 pcall 捕获可能的错误
                        local ok, devices = pcall(function() 
                            return DeviceRegistry and DeviceRegistry.list_devices() or {} 
                        end)
                        
                        if ok then
                            log.info("RULE", "检查设备状态，共有 %d 个设备", #devices)
                            -- 检查设备状态逻辑
                        else
                            log.error("RULE", "获取设备列表失败: %s", devices)
                        end
                    end
                }
            }
        ]]
    }
    
    return rules_core.load_all_from_strings(default_rules)
end

-- 应用规则数据
function rules_core.apply_rules(rules_data) 
    log.info(TAG, "应用规则数据，类型: " .. type(rules_data))
    
    -- 检查规则数据有效性
    if not rules_data or type(rules_data) ~= "table" then 
        log.error(TAG, "无效的规则数据，必须是table类型")
        return 0, "无效的规则数据"
    end
    
    -- 清空现有规则
    rules_core.cleanup()
    
    -- 加载新规则
    return rules_core.load_all_from_strings(rules_data)
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