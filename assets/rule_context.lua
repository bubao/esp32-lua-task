-- rule_context.lua - 规则上下文管理器

local DeviceRegistry = require("device_registry")
local log = require("log")

local RuleContext = {}
RuleContext.__index = RuleContext

-- 规则类型定义（添加SYSTEM类型）
local RULE_TYPES = {
    CRON = "cron",
    EVENT = "event",
    THRESHOLD = "threshold",
    TICK = "tick",
    SYSTEM = "system"  -- 添加系统规则类型
}

-- 规则类型验证器（添加system规则类型的验证器）
local rule_validators = {
    [RULE_TYPES.CRON] = function(rule_def)
        if type(rule_def.schedule) ~= "string" then
            return false, "type为'cron'时必须提供字符串类型的schedule"
        end
        if type(rule_def.on_cron) ~= "function" then
            return false, "type为'cron'时必须定义on_cron函数"
        end
        return true
    end,
    
    [RULE_TYPES.EVENT] = function(rule_def)
        if not rule_def.event then
            return false, "type为'event'时必须提供event字段"
        end
        if type(rule_def.on_event) ~= "function" then
            return false, "type为'event'时必须定义on_event函数"
        end
        return true
    end,
    
    [RULE_TYPES.THRESHOLD] = function(rule_def)
        if rule_def.threshold_min == nil or rule_def.threshold_max == nil then
            return false, "type为'threshold'时必须同时定义threshold_min和threshold_max"
        end
        if type(rule_def.threshold_min_func) ~= "function" or type(rule_def.threshold_max_func) ~= "function" then
            return false, "type为'threshold'时必须定义threshold_min_func和threshold_max_func函数"
        end
        return true
    end,
    
    [RULE_TYPES.TICK] = function(rule_def)
        if type(rule_def.on_tick) ~= "function" then
            return false, "type为'tick'时必须定义on_tick函数"
        end
        return true
    end,
    
    -- 添加system规则类型的验证器
    [RULE_TYPES.SYSTEM] = function(rule_def)
        if not rule_def.system_action then
            return false, "system规则必须定义system_action字段"
        end
        if type(rule_def.on_system_event) ~= "function" then
            return false, "system规则必须定义on_system_event函数"
        end
        return true
    end
}

-- 验证规则定义（修改此函数以处理system:前缀）
local function validate_rule_def(rule_def)
    local rule_type = rule_def.type
    
    if not rule_type then
        return false, "规则定义缺少type字段"
    end
    
    -- 处理system:前缀的规则类型
    if type(rule_type) == "string" and rule_type:sub(1, 7) == "system:" then
        rule_def.type = RULE_TYPES.SYSTEM  -- 规范化类型
        rule_def.system_action = rule_type:sub(8)  -- 提取action部分
        local validator = rule_validators[RULE_TYPES.SYSTEM]
        if validator then
            return validator(rule_def)
        end
    end
    
    -- 现有验证逻辑
    local validator = rule_validators[rule_type]
    if not validator then
        return false, "未知规则类型: " .. rule_type
    end
    
    return validator(rule_def)
end

-- 创建新的规则上下文
function RuleContext.new(rule_def)
    assert(type(rule_def) == "table", "rule_def必须是表")
    assert(type(rule_def.id) == "string", "rule_def.id必须是字符串")
    
    -- 类型验证（抛出错误将阻止规则加载）
    local valid, err = validate_rule_def(rule_def)
    if not valid then
        log.error("[RuleContext] 规则验证失败: " .. err)
        error("[RuleContext] 规则验证失败: " .. err)
    end
    
    local self = setmetatable({}, RuleContext)
    
    -- 基本属性
    self.id = rule_def.id
    self.description = rule_def.description or ""
    self.type = rule_def.type
    self.enabled = rule_def.enabled ~= false -- 默认启用
    self.state = {} -- 规则状态存储
    
    -- 生命周期钩子
    self._on_event = rule_def.on_event
    self._on_tick = rule_def.on_tick
    self._on_cron = rule_def.on_cron
    self._on_mounted = rule_def.on_mounted
    self._on_destroy = rule_def.on_destroy
    
    -- 阈值类型相关
    self.threshold_min = rule_def.threshold_min
    self.threshold_max = rule_def.threshold_max
    self.threshold_min_func = rule_def.threshold_min_func
    self.threshold_max_func = rule_def.threshold_max_func
    
    -- 事件类型相关
    self.event = rule_def.event
    
    -- 初始化方法
    if type(rule_def.init) == "function" then
        self._init = rule_def.init
    end
    
    -- 特殊处理system规则
    if rule_def.type == RULE_TYPES.SYSTEM then
        self.system_action = rule_def.system_action
        self._on_system_event = rule_def.on_system_event
    end
    
    -- 注册cron任务
    if rule_def.type == RULE_TYPES.CRON then
        local ok, err = self:register_cron_job()
        if not ok then
            log.error("[RuleContext][" .. self.id .. "] Cron注册失败: " .. err)
            return nil, err
        end
    end
    
    return self
end

-- 注册cron任务
function RuleContext:register_cron_job()
    if not self._on_cron then
        return false, "缺少on_cron函数"
    end
    
    -- 包装函数，确保调用时self是RuleContext实例
    local function cron_wrapper()
        if not self.enabled then return end
        
        local ok, err = pcall(self._on_cron, self)
        if not ok then
            log.error("[RuleContext][" .. self.id .. "] Cron执行错误: " .. err)
        end
    end
    
    -- 调用C端注册函数
    local result = cron.register_cron(self.id, self.schedule, cron_wrapper)
    
    -- 根据C端返回值处理
    if result == 0 then
        self._cron_registered = true
        return true
    else
        return false, "Cron注册失败，错误码: " .. result
    end
end

-- 初始化规则
function RuleContext:init(config)
    if self._init then
        local ok, err = pcall(self._init, self, config)
        if not ok then
            log.error("[RuleContext][" .. self.id .. "] 初始化错误: " .. err)
            return false
        end
    end
    return true
end

-- 获取设备
function RuleContext:get_device_by_id(id)
    return DeviceRegistry.get(id)
end

-- 规则挂载时调用
function RuleContext:mounted()
    if not self.enabled then return end
    
    if self._on_mounted then
        local ok, err = pcall(self._on_mounted, self)
        if not ok then
            log.error("[RuleContext][" .. self.id .. "] on_mounted错误: " .. err)
        end
    end
end

-- 处理事件（修改此函数以处理system事件）
function RuleContext:handle_event(event)
    if not self.enabled then return end
    
    -- 处理system事件
    if self.type == RULE_TYPES.SYSTEM and event.event_type:sub(1, 7) == "system:" then
        if self._on_system_event then
            local ok, err = pcall(self._on_system_event, self, event)
            if not ok then
                log.error("[RuleContext][" .. self.id .. "] 处理system事件错误: " .. err)
            end
            return
        end
    end
    
    if self._on_event then
        local ok, err = pcall(self._on_event, self, event)
        if not ok then
            log.error("[RuleContext][" .. self.id .. "] on_event错误: " .. err)
        end
    end
end

-- 定时调用
function RuleContext:tick(now_ms)
    if not self.enabled then return end
    
    if self._on_tick then
        local ok, err = pcall(self._on_tick, self, now_ms)
        if not ok then
            log.error("[RuleContext][" .. self.id .. "] on_tick错误: " .. err)
        end
    end
    
    -- 阈值检查
    if self.type == RULE_TYPES.THRESHOLD and self.threshold_min_func and self.threshold_max_func then
        local value = self.threshold_min_func(self)
        if value < self.threshold_min then
            self.threshold_min_func(self)
        elseif value > self.threshold_max then
            self.threshold_max_func(self)
        end
    end
end

-- 销毁规则
function RuleContext:destroy()
    if self._on_destroy then
        local ok, err = pcall(self._on_destroy, self)
        if not ok then
            log.error("[RuleContext][" .. self.id .. "] on_destroy错误: " .. err)
        end
    end
    
    -- 取消cron注册
    if self._cron_registered then
        cron.unregister_cron(self.id)
        self._cron_registered = false
    end
end

-- 启用/禁用规则
function RuleContext:set_enabled(enabled)
    self.enabled = enabled
    
    -- 如果启用且是cron类型但未注册，则注册
    if enabled and self.type == RULE_TYPES.CRON and not self._cron_registered then
        self:register_cron_job()
    -- 如果禁用且已注册，则取消注册
    elseif not enabled and self._cron_registered then
        cron.unregister_cron(self.id)
        self._cron_registered = false
    end
end

return RuleContext