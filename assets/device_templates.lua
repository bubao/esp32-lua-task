-- device_template.lua - 适配新C端GPIO接口
local log = require("log")

local DeviceTemplate = {}

-- 引入ESP32 GPIO常量定义
-- local gpio = require("gpio")

local TAG = "DEVICE_TEMPLATE"

-- 事件发布函数（由外部注入）
local event_pub = nil
function DeviceTemplate.set_event_publisher(pub_func)
    assert(type(pub_func) == "function", "event publisher must be a function")
    event_pub = pub_func
end

-- 触发事件
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
        log.warn(TAG, "event_pub 未设置，事件未发送")
    end
end

-- 基类模板
local base_template = {
    defaults = {
        type = nil,
        id = nil,
        enabled = true,
        created_at = os.time(),
        updated_at = os.time(),
    },
    
    -- 初始化基础属性
    _init_base = function(self, params)
        for k, v in pairs(self.defaults) do
            self[k] = params[k] or v
        end
        
        -- 不允许覆盖type
        self.type = self.defaults.type
        self.updated_at = os.time()
        
        log.info(TAG, "设备初始化: %s [%s]", self.id, self.type)
    end,
    
    -- 检查设备是否有效
    is_valid = function(self)
        return self.id ~= nil and self.type ~= nil
    end,
    
    -- 更新设备属性
    update = function(self, params)
        if not self.enabled then
            log.warn(TAG, "设备 [%s] 已禁用，忽略更新", self.id)
            return false
        end
        
        for k, v in pairs(params) do
            if k ~= "type" then  -- 禁止修改type
                self[k] = v
            end
        end
        
        self.updated_at = os.time()
        trigger_event(self, "device_updated", self)
        log.info(TAG, "设备属性更新: %s", self.id)
        return true
    end,
    
    -- 启用/禁用设备
    set_enabled = function(self, enabled)
        self.enabled = enabled
        
        if enabled then
            self:init()  -- 重新初始化设备
            trigger_event(self, "device_enabled", self)
            log.info(TAG, "设备已启用: %s", self.id)
        else
            trigger_event(self, "device_disabled", self)
            log.info(TAG, "设备已禁用: %s", self.id)
        end
        
        return true
    end,
    
    -- 安全调用方法
    safe_call = function(self, method_name, ...)
        local method = self[method_name]
        if type(method) ~= "function" then
            log.error(TAG, "方法未找到: %s", method_name)
            return nil, "Method not found: " .. method_name
        end
        
        local ok, result = pcall(method, self, ...)
        if not ok then
            log.error(TAG, "调用方法错误 [%s]: %s", method_name, result)
            return nil, "Error: " .. tostring(result)
        end
        
        return result
    end,
    
    -- 释放资源
    cleanup = function(self)
        -- 子类可以重写此方法来清理资源
        log.info(TAG, "设备资源已清理: %s", self.id)
    end
}

-- 创建新模板的函数
function DeviceTemplate.create_template(base, defaults, methods)
    local template = {}
    
    -- 设置元表继承基类
    setmetatable(template, {__index = base})
    
    -- 合并默认值
    template.defaults = {}
    for k, v in pairs(base.defaults or {}) do
        template.defaults[k] = v
    end
    for k, v in pairs(defaults or {}) do
        template.defaults[k] = v
    end
    
    -- 合并方法
    for k, v in pairs(methods or {}) do
        template[k] = v
    end
    
    return template
end

-- 通用传感器模板
local generic_sensor = DeviceTemplate.create_template(
    base_template,
    {
        type = "sensor",
        gpio = nil,
        gpio_mode = gpio.MODE_INPUT,      -- 使用ESP32 GPIO模式常量
        gpio_pull = gpio.PULLUP_DISABLE,  -- 使用ESP32上拉/下拉常量
        raw_value = nil,
        value = nil,
        threshold_low = nil,
        threshold_high = nil,
        parse_func = nil,
        poll_interval = 1000,  -- 轮询间隔(ms)
        last_poll = 0,
    },
    {
        -- 初始化传感器
        init = function(self)
            self:_init_base()
            
            -- 配置GPIO
            if self.gpio then
                local ok, err = gpio.set_mode(self.gpio, self.gpio_mode, self.gpio_pull, gpio.INTR_DISABLE)
                if not ok then
                    log.error(TAG, "配置GPIO失败 [%d]: %s", self.gpio, err)
                else
                    log.info(TAG, "GPIO配置成功 [%d]: mode=%d, pull=%d", 
                        self.gpio, self.gpio_mode, self.gpio_pull)
                end
            end
        end,
        
        -- 更新传感器值
        update_value = function(self, raw_val)
            if not self.enabled then return end
            
            self.raw_value = raw_val
            self.last_poll = os.time()
            
            -- 解析值
            if type(self.parse_func) == "function" then
                local success, parsed = pcall(self.parse_func, raw_val)
                if success then
                    self.value = parsed
                else
                    log.error(TAG, "解析错误 [%s]: %s", self.id, parsed)
                    self.value = nil
                end
            else
                self.value = raw_val
            end
            
            -- 检查阈值
            if self.threshold_low and self.value and self.value < self.threshold_low then
                trigger_event(self, "threshold_low", self.value)
                log.info(TAG, "阈值事件 [%s]: 低于下限 (%s < %s)", 
                    self.id, tostring(self.value), tostring(self.threshold_low))
            elseif self.threshold_high and self.value and self.value > self.threshold_high then
                trigger_event(self, "threshold_high", self.value)
                log.info(TAG, "阈值事件 [%s]: 高于上限 (%s > %s)", 
                    self.id, tostring(self.value), tostring(self.threshold_high))
            else
                trigger_event(self, "value_update", self.value)
                log.debug(TAG, "值更新 [%s]: %s", self.id, tostring(self.value))
            end
            
            self.updated_at = os.time()
        end,
        
        -- 轮询传感器值
        poll = function(self)
            if not self.enabled then return end
            
            if self.gpio then
                local value, err = gpio.get_level(self.gpio)
                if value ~= nil then
                    self:update_value(value)
                else
                    log.error(TAG, "读取GPIO失败 [%d]: %s", self.gpio, err)
                end
            end
        end,
        
        -- 释放资源
        cleanup = function(self)
            if self.gpio then
                gpio.reset(self.gpio)
                log.info(TAG, "GPIO资源已释放 [%d]", self.gpio)
            end
            base_template.cleanup(self)
        end
    }
)

-- 通用执行器模板
local generic_actuator = DeviceTemplate.create_template(
    base_template,
    {
        type = "actuator",
        gpio = nil,
        gpio_mode = gpio.MODE_OUTPUT,     -- 使用ESP32 GPIO模式常量
        gpio_pull = gpio.PULLUP_DISABLE,  -- 使用ESP32上拉/下拉常量
        mode = "digital",  -- 数字或PWM
        status = nil,
    },
    {
        -- 初始化执行器
        init = function(self)
            self:_init_base()
            
            -- 配置GPIO
            if self.gpio then
                local ok, err = gpio.set_mode(self.gpio, self.gpio_mode, self.gpio_pull, gpio.INTR_DISABLE)
                if not ok then
                    log.error(TAG, "配置GPIO失败 [%d]: %s", self.gpio, err)
                else
                    log.info(TAG, "GPIO配置成功 [%d]: mode=%d, pull=%d", 
                        self.gpio, self.gpio_mode, self.gpio_pull)
                end
            end
            
            -- 设置初始状态
            if self.status ~= nil then
                self:set_status(self.status)
            end
        end,
        
        -- 设置执行器状态
        set_status = function(self, val)
            if not self.enabled then
                log.warn(TAG, "设备 [%s] 已禁用，忽略设置状态", self.id)
                return false
            end
            
            local success = true
            
            if self.mode == "digital" then
                self.status = (val == true or val == 1)
                
                if self.gpio then
                    success, err = gpio.set_level(self.gpio, self.status and 1 or 0)
                    if not success then
                        log.error(TAG, "设置GPIO电平失败 [%d]: %s", self.gpio, err)
                    end
                end
            elseif self.mode == "pwm" then
                self.status = tonumber(val) or 0
                
                -- 注意：这里需要使用你的PWM API，我假设它存在
                if self.gpio and type(pwm.set_duty) == "function" then
                    -- 将百分比转换为PWM值(0-1023)
                    local pwm_value = math.floor((self.status / 100) * 1023)
                    success, err = pwm.set_duty(self.gpio, pwm_value)
                    if not success then
                        log.error(TAG, "设置PWM占空比失败 [%d]: %s", self.gpio, err)
                    end
                else
                    log.error(TAG, "PWM API未找到或未配置")
                    success = false
                end
            else
                log.error(TAG, "未知模式 [%s]: %s", self.id, tostring(self.mode))
                success = false
            end
            
            if success then
                trigger_event(self, "status_changed", self.status)
                self.updated_at = os.time()
                log.info(TAG, "执行器状态已更新 [%s]: %s", self.id, tostring(self.status))
            end
            
            return success
        end,
        
        -- 释放资源
        cleanup = function(self)
            if self.gpio then
                gpio.reset(self.gpio)
                log.info(TAG, "GPIO资源已释放 [%d]", self.gpio)
            end
            base_template.cleanup(self)
        end
    }
)

-- 预设设备模板

-- DHT11温湿度传感器
local dht11_sensor = DeviceTemplate.create_template(
    generic_sensor,
    {
        sensor_type = "dht11",
        temperature = nil,
        humidity = nil,
    },
    {
        -- 解析DHT11数据
        update_value = function(self, raw_val)
            if not self.enabled then return end
            
            if type(raw_val) == "table" and raw_val.temp and raw_val.hum then
                self.temperature = raw_val.temp
                self.humidity = raw_val.hum
                self.value = { temp = self.temperature, hum = self.humidity }
                
                trigger_event(self, "temperature_update", self.temperature)
                trigger_event(self, "humidity_update", self.humidity)
                trigger_event(self, "value_update", self.value)
                
                self.updated_at = os.time()
                log.debug(TAG, "DHT11数据更新 [%s]: temp=%s, hum=%s", 
                    self.id, tostring(self.temperature), tostring(self.humidity))
            else
                log.error(TAG, "数据格式错误 [%s]: 期望table，得到 %s", 
                    self.id, type(raw_val))
            end
        end
    }
)

-- 数字输入传感器（如按钮）
local digital_input_sensor = DeviceTemplate.create_template(
    generic_sensor,
    {
        gpio_mode = gpio.MODE_INPUT,
        gpio_pull = gpio.PULLUP_ENABLE,  -- 按钮通常使用上拉
        debounce = 50,
        last_value = nil,
        last_debounce_time = 0,
    },
    {
        -- 带消抖的输入读取
        poll = function(self)
            if not self.enabled then return end
            
            if self.gpio then
                local current_value, err = gpio.get_level(self.gpio)
                if current_value ~= nil then
                    local current_time = os.time() * 1000  -- 转换为毫秒
                    
                    -- 消抖逻辑
                    if current_value ~= self.last_value then
                        self.last_debounce_time = current_time
                    end
                    
                    if (current_time - self.last_debounce_time) > self.debounce then
                        if current_value ~= self.value then
                            self:update_value(current_value)
                            log.debug(TAG, "按钮状态变化 [%s]: %s", self.id, tostring(current_value))
                        end
                    end
                    
                    self.last_value = current_value
                else
                    log.error(TAG, "读取GPIO失败 [%d]: %s", self.gpio, err)
                end
            end
        end
    }
)

-- LED执行器
local led_actuator = DeviceTemplate.create_template(
    generic_actuator,
    {
        actuator_type = "led",
        brightness = 0,  -- PWM模式下的亮度
    },
    {
        -- 设置LED状态/亮度
        set_status = function(self, val)
            if not self.enabled then
                log.warn(TAG, "设备 [%s] 已禁用，忽略设置亮度", self.id)
                return false
            end
            
            if self.mode == "digital" then
                -- 调用父类方法
                return generic_actuator.set_status(self, val)
            elseif self.mode == "pwm" then
                -- 确保亮度在0-100范围内
                self.brightness = math.max(0, math.min(100, tonumber(val) or 0))
                
                -- 将百分比转换为PWM值(0-1023)
                local pwm_value = math.floor((self.brightness / 100) * 1023)
                
                if self.gpio and type(pwm.set_duty) == "function" then
                    local success, err = pwm.set_duty(self.gpio, pwm_value)
                    
                    if success then
                        trigger_event(self, "brightness_changed", self.brightness)
                        self.updated_at = os.time()
                        log.info(TAG, "LED亮度已更新 [%s]: %s%%", self.id, tostring(self.brightness))
                    else
                        log.error(TAG, "设置PWM占空比失败 [%d]: %s", self.gpio, err)
                    end
                    
                    return success
                else
                    log.error(TAG, "PWM API未找到或未配置")
                    return false
                end
            end
            
            return false
        end
    }
)

-- 电机执行器
local motor_actuator = DeviceTemplate.create_template(
    generic_actuator,
    {
        actuator_type = "motor",
        speed = 0,  -- PWM模式下的速度
        direction = 1,  -- 方向(1或-1)
        direction_gpio = nil,  -- 方向控制GPIO
    },
    {
        -- 设置电机状态
        set_status = function(self, val)
            if not self.enabled then
                log.warn(TAG, "设备 [%s] 已禁用，忽略设置速度", self.id)
                return false
            end
            
            if self.mode == "digital" then
                -- 开关控制
                self.status = (val == true or val == 1)
                
                if self.gpio then
                    local success, err = gpio.set_level(self.gpio, self.status and 1 or 0)
                    
                    if success then
                        trigger_event(self, "status_changed", self.status)
                        self.updated_at = os.time()
                        log.info(TAG, "电机状态已更新 [%s]: %s", self.id, tostring(self.status))
                    else
                        log.error(TAG, "设置GPIO电平失败 [%d]: %s", self.gpio, err)
                    end
                    
                    return success
                end
            elseif self.mode == "pwm" then
                -- 速度控制
                self.speed = math.max(0, math.min(100, tonumber(val) or 0))
                
                if self.gpio and type(pwm.set_duty) == "function" then
                    -- 将百分比转换为PWM值
                    local pwm_value = math.floor((self.speed / 100) * 1023)
                    local success, err = pwm.set_duty(self.gpio, pwm_value)
                    
                    if success then
                        trigger_event(self, "speed_changed", self.speed)
                        self.updated_at = os.time()
                        log.info(TAG, "电机速度已更新 [%s]: %s%%", self.id, tostring(self.speed))
                    else
                        log.error(TAG, "设置PWM占空比失败 [%d]: %s", self.gpio, err)
                    end
                    
                    return success
                end
            end
            
            return false
        end,
        
        -- 设置电机方向
        set_direction = function(self, direction)
            if not self.enabled then
                log.warn(TAG, "设备 [%s] 已禁用，忽略设置方向", self.id)
                return false
            end
            
            self.direction = (direction < 0) and -1 or 1
            
            if self.direction_gpio then
                local success, err = gpio.set_level(self.direction_gpio, self.direction > 0)
                
                if success then
                    trigger_event(self, "direction_changed", self.direction)
                    self.updated_at = os.time()
                    log.info(TAG, "电机方向已更新 [%s]: %s", self.id, tostring(self.direction))
                else
                    log.error(TAG, "设置GPIO电平失败 [%d]: %s", self.direction_gpio, err)
                end
                
                return success
            end
            
            log.warn(TAG, "未配置方向控制GPIO [%s]", self.id)
            return false
        end
    }
)

-- 设备模板注册表
DeviceTemplate.templates = {
    -- 基础模板
    sensor = generic_sensor,
    actuator = generic_actuator,
    
    -- 预设模板
    dht11 = dht11_sensor,
    button = digital_input_sensor,
    led = led_actuator,
    motor = motor_actuator,
}

-- 生成设备实例
function DeviceTemplate.new_device(template_name, params)
    local template = DeviceTemplate.templates[template_name]
    
    if not template then
        log.error(TAG, "未知设备模板: %s", tostring(template_name))
        return nil, "未知设备模板"
    end
    
    local device = {}
    
    -- 设置元表继承模板
    setmetatable(device, {__index = template})
    
    -- 初始化设备
    device:init(params)
    
    -- 验证设备
    if not device:is_valid() then
        log.error(TAG, "设备创建失败: 缺少必要参数")
        return nil, "设备创建失败: 缺少必要参数"
    end
    
    log.info(TAG, "创建设备: %s [%s]", device.id, device.type)
    return device
end

return DeviceTemplate