local DeviceRegistry = require("device_registry")

local RuleContext = {}
RuleContext.__index = RuleContext

local function validate_rule_def(rule_def)
    local rule_type = rule_def.type

    if rule_type == "cron" then
        if type(rule_def.schedule) ~= "string" then
            error("type 为 'cron' 时必须提供字符串类型的 schedule")
        end
        if type(rule_def.on_cron) ~= "function" then
            error("type 为 'cron' 时必须定义 on_cron 函数")
        end
    elseif rule_type == "event" then
        if not rule_def.event then
            error("type 为 'event' 时必须提供 event 字段")
        end
        if type(rule_def.on_event) ~= "function" then
            error("type 为 'event' 时必须定义 on_event 函数")
        end
    elseif rule_type == "threshold" then
        if rule_def.threshold_min == nil or rule_def.threshold_max == nil then
            error("type 为 'threshold' 时必须同时定义 threshold_min 和 threshold_max")
        end
        if type(rule_def.threshold_min_func) ~= "function" or type(rule_def.threshold_max_func) ~= "function" then
            error("type 为 'threshold' 时必须定义 threshold_min_func 和 threshold_max_func 函数")
        end
    end
end

function RuleContext.new(rule_def)
    assert(type(rule_def) == "table", "rule_def 必须是表")
    assert(type(rule_def.id) == "string", "rule_def.id 必须是字符串")

    -- 类型验证（抛出错误将阻止规则加载）
    validate_rule_def(rule_def)

    local self = setmetatable({}, RuleContext)

    self.id = rule_def.id
    self.description = rule_def.description or ""
    self.type = rule_def.type or "unknown"

    -- 生命周期钩子
    self._on_event = rule_def.on_event
    self._on_tick = rule_def.on_tick
    self._on_cron = rule_def.on_cron
    self._on_mounted = rule_def.on_mounted
    self._on_destroy = rule_def.on_destroy

    -- threshold 类型相关
    self.threshold_min = rule_def.threshold_min
    self.threshold_max = rule_def.threshold_max
    self.threshold_min_func = rule_def.threshold_min_func
    self.threshold_max_func = rule_def.threshold_max_func

    -- 事件类型相关
    self.event = rule_def.event

    -- init 方法（非钩子）
    if type(rule_def.init) == "function" then
        self.init = function(_, config)
            local ok, err = pcall(rule_def.init, self, config)
            if not ok then
                print("[rule_context][" .. self.id .. "] init 错误: " .. tostring(err))
            end
        end
    end

	if rule_def.type == "cron" and rule_def.schedule and rule_def.id then
        local self = setmetatable({}, RuleContext) -- 先定义 self

        -- 包装函数，确保调用时 self 是 RuleContext 实例
        local function cron_wrapper(rule_id)
            if rule_def.on_cron then
                rule_def.on_cron(self, rule_id)
            end
        end

        local ok, err = cron.register_cron(rule_def.id, rule_def.schedule, cron_wrapper, { foo = 123 })
        if not ok then
            print("[cron] 注册失败: " .. tostring(err))
        end
    end

    self.state = {}

    return self
end

function RuleContext:get_device_by_id(id)
    return DeviceRegistry.get(id)
end

function RuleContext:mounted()
    if type(self._on_mounted) == "function" then
        local ok, err = pcall(self._on_mounted, self)
        if not ok then
            print("[rule_context][" .. self.id .. "] on_mounted 错误: " .. tostring(err))
        end
    end
end

function RuleContext:handle_event(event)
    if type(self._on_event) == "function" then
        local ok, err = pcall(self._on_event, self, event)
        if not ok then
            print("[rule_context][" .. self.id .. "] on_event 错误: " .. tostring(err))
        end
    end
end

function RuleContext:tick(now_ms)
    if type(self._on_tick) == "function" then
        local ok, err = pcall(self._on_tick, self, now_ms)
        if not ok then
            print("[rule_context][" .. self.id .. "] on_tick 错误: " .. tostring(err))
        end
    end
end

function RuleContext:handle_cron()
    if type(self._on_cron) == "function" then
        local ok, err = pcall(self._on_cron, self)
        if not ok then
            print("[rule_context][" .. self.id .. "] on_cron 错误: " .. tostring(err))
        end
    end
end

function RuleContext:destroy()
    if type(self._on_destroy) == "function" then
        local ok, err = pcall(self._on_destroy, self)
        if not ok then
            print("[rule_context][" .. self.id .. "] on_destroy 错误: " .. tostring(err))
        end
    end
end

return RuleContext
