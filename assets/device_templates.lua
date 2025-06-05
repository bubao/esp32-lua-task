-- device_template.lua
-- 设备模板管理，支持预设设备和通用模版自定义

local DeviceTemplate = {}

-- 事件发布函数（由外部注入）
local event_pub = nil
function DeviceTemplate.set_event_publisher(pub_func)
    assert(type(pub_func) == "function", "event publisher must be a function")
    event_pub = pub_func
end

local function trigger_event(device, event_type, data)
    if event_pub then
        event_pub({
            device_id = device.id,
            device_type = device.type,
            event_type = event_type,
            data = data,
            timestamp = os.time(),
        })
    else
        print("[device_template] event_pub 未设置，事件未发送")
    end
end

-- 通用传感器模版
local generic_sensor = {
    defaults = {
        type = "sensor",
        id = nil,
        gpio = nil,
        raw_value = nil,
        value = nil,
        threshold_low = nil,
        threshold_high = nil,
        parse_func = nil,
    },

    update_value = function(self, raw_val)
        self.raw_value = raw_val
        if type(self.parse_func) == "function" then
            local success, parsed = pcall(self.parse_func, raw_val)
            if success then
                self.value = parsed
            else
                print("[sensor][" .. tostring(self.id) .. "] 解析错误: " .. tostring(parsed))
                self.value = nil
            end
        else
            self.value = raw_val
        end

        if self.threshold_low and self.value and self.value < self.threshold_low then
            trigger_event(self, "threshold_low", self.value)
        elseif self.threshold_high and self.value and self.value > self.threshold_high then
            trigger_event(self, "threshold_high", self.value)
        else
            trigger_event(self, "value_update", self.value)
        end
    end,
}

-- 通用执行器模版
local generic_actuator = {
    defaults = {
        type = "actuator",
        id = nil,
        gpio = nil,
        mode = "digital",
        status = nil,
    },

    init = function(self, params)
        self.gpio = params.gpio
        self.mode = params.mode or "digital"
        self.status = false
    end,

    set_status = function(self, val)
        if self.mode == "digital" then
            self.status = (val == true or val == 1)
            print(string.format("[actuator][digital] gpio=%d 设置为 %s", self.gpio, tostring(self.status)))
        elseif self.mode == "pwm" then
            self.status = tonumber(val) or 0
            print(string.format("[actuator][pwm] gpio=%d PWM值设置为 %d", self.gpio, self.status))
        else
            print("[actuator] 未知模式: " .. tostring(self.mode))
        end
        trigger_event(self, "status_changed", self.status)
    end,
}

-- 预设设备模版（继承自通用模版，可重写或扩展）

-- DHT11 温湿度传感器
local dht11_sensor = {
    defaults = {
        type = "sensor",
        id = nil,
        gpio = nil,
        raw_value = nil,
        temperature = nil,
        humidity = nil,
    },

    -- 解析DHT11特有数据格式，假设raw_value是表 {temp=xx, hum=xx}
    update_value = function(self, raw_val)
        if type(raw_val) == "table" then
            self.temperature = raw_val.temp or nil
            self.humidity = raw_val.hum or nil
            trigger_event(self, "temperature_update", self.temperature)
            trigger_event(self, "humidity_update", self.humidity)
        else
            print("[dht11_sensor] 数据格式错误，期望table")
        end
    end,
}

-- 温度传感器（假设简单数字）
local temperature_sensor = {
    defaults = {
        type = "sensor",
        id = nil,
        gpio = nil,
        value = nil,
    },

    update_value = generic_sensor.update_value, -- 复用通用传感器的更新逻辑
}

-- 电机执行器，假设支持数字控制和PWM控制
local motor_actuator = {
    defaults = {
        type = "actuator",
        id = nil,
        gpio = nil,
        mode = "digital",
        status = false,
        speed = 0, -- PWM值或速度百分比
    },

    init = generic_actuator.init,

    set_status = function(self, val)
        if self.mode == "digital" then
            self.status = (val == true or val == 1)
            print(string.format("[motor_actuator][digital] gpio=%d 状态: %s", self.gpio, tostring(self.status)))
        elseif self.mode == "pwm" then
            self.speed = tonumber(val) or 0
            print(string.format("[motor_actuator][pwm] gpio=%d 速度: %d", self.gpio, self.speed))
        else
            print("[motor_actuator] 未知模式: " .. tostring(self.mode))
        end
        trigger_event(self, "status_changed", self.status or self.speed)
    end,
}

-- LED执行器，支持数字开关和PWM调光
local led_actuator = {
    defaults = {
        type = "actuator",
        id = nil,
        gpio = nil,
        mode = "digital",
        status = false,
        brightness = 0,
    },

    init = generic_actuator.init,

    set_status = function(self, val)
        if self.mode == "digital" then
            self.status = (val == true or val == 1)
            print(string.format("[led_actuator][digital] gpio=%d 状态: %s", self.gpio, tostring(self.status)))
        elseif self.mode == "pwm" then
            self.brightness = tonumber(val) or 0
            print(string.format("[led_actuator][pwm] gpio=%d 亮度: %d", self.gpio, self.brightness))
        else
            print("[led_actuator] 未知模式: " .. tostring(self.mode))
        end
        trigger_event(self, "status_changed", self.status or self.brightness)
    end,
}

-- 设备模版注册表
DeviceTemplate.templates = {
    sensor = generic_sensor,
    actuator = generic_actuator,

    dht11 = dht11_sensor,
    temperature_sensor = temperature_sensor,
    motor = motor_actuator,
    led = led_actuator,
}

-- 生成设备实例，支持指定预设模板或通用模板
function DeviceTemplate.new_device(type_name, params)
    local tmpl = DeviceTemplate.templates[type_name]

    if not tmpl then
        -- 不是预设，判断是否是通用类型sensor或actuator
        if params and params.type and DeviceTemplate.templates[params.type] then
            tmpl = DeviceTemplate.templates[params.type]
        else
            error("未知设备类型: " .. tostring(type_name))
        end
    end

    local device = {}

    -- 复制默认字段
    for k, v in pairs(tmpl.defaults) do
        device[k] = v
    end

    -- 绑定方法
    for k, v in pairs(tmpl) do
        if type(v) == "function" and k ~= "init" then
            device[k] = v
        end
    end

    -- 赋ID
    device.id = params.id or (type_name .. "_" .. tostring(math.random(10000, 99999)))

    -- 赋参数覆盖默认值
    for k, v in pairs(params or {}) do
        if k ~= "type" then
            device[k] = v
        end
    end

    -- 调用初始化
    if tmpl.init then
        tmpl.init(device, params)
    end

    return device
end

return DeviceTemplate
