local RuleContext = {}
RuleContext.__index = RuleContext

-- 构造函数，传入规则定义表 rule_def
-- 规则定义表结构示例：
-- {
--   id = "rule_01",
--   description = "温度控制规则",
--   on_init = function(self) end,
--   on_event = function(self, event) end,
--   on_tick = function(self, now_ms) end,
--   on_corn = function(self) end,
--   on_destroy = function(self) end,
--   -- 可扩展其他自定义字段，如阈值、设备id等
-- }
function RuleContext.new(rule_def, device_manager)
    assert(type(rule_def) == "table", "rule_def 必须是表")
    assert(type(rule_def.id) == "string", "rule_def.id 必须是字符串")

    local self = setmetatable({}, RuleContext)

    self.id = rule_def.id
    self.description = rule_def.description or ""
    self._on_init = rule_def.on_init
    self._on_event = rule_def.on_event
    self._on_tick = rule_def.on_tick
    self._on_corn = rule_def.on_corn
    self._on_destroy = rule_def.on_destroy

	-- 设备管理对象
    self.device_manager = device_manager

    -- 规则可自行保存状态数据
    self.state = {}

    -- 不在这里主动调用 on_init，改由 C 端调用 on_init(config)
    return self
end

function RuleContext:get_device_by_id(id)
    if self.device_manager and type(self.device_manager.get_device_by_id) == "function" then
        return self.device_manager:get_device_by_id(id)
    end
    return nil
end

-- on_init 支持外部调用时传入 config 表
function RuleContext:on_init(config)
    if type(self._on_init) == "function" then
        local ok, err = pcall(self._on_init, self, config)
        if not ok then
            print("[rule_context][" .. self.id .. "] on_init 错误: " .. tostring(err))
        end
    end
end

-- 处理事件回调
function RuleContext:handle_event(event)
    if type(self._on_event) == "function" then
        local ok, err = pcall(self._on_event, self, event)
        if not ok then
            print("[rule_context][" .. self.id .. "] on_event 错误: " .. tostring(err))
        end
    end
end

-- 定时驱动调用，传入当前时间毫秒数
function RuleContext:tick(now_ms)
    if type(self._on_tick) == "function" then
        local ok, err = pcall(self._on_tick, self, now_ms)
        if not ok then
            print("[rule_context][" .. self.id .. "] on_tick 错误: " .. tostring(err))
        end
    end
end

-- cron触发调用（C端触发）
function RuleContext:handle_corn()
    if type(self._on_corn) == "function" then
        local ok, err = pcall(self._on_corn, self)
        if not ok then
            print("[rule_context][" .. self.id .. "] on_corn 错误: " .. tostring(err))
        end
    end
end

-- 规则销毁调用
function RuleContext:destroy()
    if type(self._on_destroy) == "function" then
        local ok, err = pcall(self._on_destroy, self)
        if not ok then
            print("[rule_context][" .. self.id .. "] on_destroy 错误: " .. tostring(err))
        end
    end
end

return RuleContext
