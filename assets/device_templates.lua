-- device_template.lua - 适配新C端GPIO接口
local log = require("log")
local timer = require("timer")
local gpio = require("gpio") -- 引入GPIO模块
local adc = require("adc")   -- 引入ADC模块
local pwm = require("pwm")   -- 引入PWM模块
local DeviceTemplate = {}
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
		params = params or {} -- 确保params是table
		for k, v in pairs(self.defaults) do
			self[k] = params[k] or v
		end
		for k, v in pairs(params) do
			self[k] = v
		end
		-- 不允许覆盖type
		-- self.type = self.defaults.type
		self.updated_at = os.time()
		log.info(TAG, "设备初始化: " .. self.id .. "[" .. self.type .. "]")
	end,
	-- 验证设备是否有效
	is_valid = function(self)
		return self.id ~= nil and self.type ~= nil
	end,
	-- 更新设备属性
	update = function(self, new_params)
		for k, v in pairs(new_params) do
			if self.defaults[k] ~= nil then -- 只允许更新预定义的属性
				self[k] = v
			end
		end
		self.updated_at = os.time()
		log.info(TAG, "设备更新: " .. self.id .. "[" .. self.type .. "]")
	end,
	-- 获取设备状态
	get_status = function(self)
		return self.status or "unknown"
	end,
	-- 设置设备状态
	set_status = function(self, status)
		if self.status ~= status then
			self.status = status
			self.updated_at = os.time()
			trigger_event(self, "status_changed", { status = status })
			log.info(TAG, "设备状态变更: " .. self.id .. "[" .. self.type .. "] -> " .. status)
		end
	end,
	-- 获取设备信息
	get_info = function(self)
		return {
			id = self.id,
			type = self.type,
			status = self:get_status(),
			created_at = self.created_at,
			updated_at = self.updated_at
		}
	end,
}

-- 创建新模板的函数
function DeviceTemplate.create_template(base, defaults, methods)
	local template = {}
	-- 设置元表继承基础模板
	setmetatable(template, { __index = base })
	-- 合并默认值
	template.defaults = {}
	for k, v in pairs(base.defaults or {}) do
		template.defaults[k] = v
	end
	for k, v in pairs(defaults or {}) do
		template.defaults[k] = v
	end
	-- 添加方法
	for k, v in pairs(methods or {}) do
		template[k] = v
	end
	-- 确保init方法调用基类的init
	local original_init = template.init
	template.init = function(self, params)
		base._init_base(self, params)
		if original_init then
			original_init(self, params)
		end
	end
	return template
end

-- 通用传感器模板
local generic_sensor = DeviceTemplate.create_template(
	base_template,
	{
		type = "sensor",
		gpio = nil,
		gpio_mode = gpio.MODE_INPUT,
		gpio_pull = gpio.PULLUP_DISABLE,
		poll_interval = 1000, -- 轮询间隔(ms)
		last_value = nil,
		last_updated = nil,
	},
	{
		-- 初始化传感器
		init = function(self, params)
			-- 调用基类初始化
			log.info(TAG, "generic_sensor 调用基类初始化")
			base_template._init_base(self, params)
			-- 配置GPIO
			if self.gpio then
				local ok, err = gpio.set_mode({
					pin = self.gpio,
					mode = self.gpio_mode,
					pull_up = self.gpio_pull == gpio.PULLUP_ENABLE,
					pull_down = self.gpio_pull == gpio.PULLDOWN_ENABLE,
					intr = gpio.INTR_DISABLE
				})
				if not ok then
					log.error(TAG, "配置GPIO失败 [" .. self.gpio .. "]: " .. err)
				else
					log.info(TAG, "GPIO配置成功 [%d]: mode=%d, pull=%d",
						self.gpio, self.gpio_mode, self.gpio_pull)
				end
			end
			-- 启动轮询
			if self.poll_interval and self.poll_interval > 0 then
				self:start_polling()
			end
		end,
		-- 启动轮询
		start_polling = function(self)
			if self.poll_timer then
				timer.clearInterval(self.poll_timer)
			end
			local poll_func = function()
				if not self.enabled then return end
				local value = self:read_value()
				if value ~= nil and value ~= self.last_value then
					self.last_value = value
					self.last_updated = os.time()
					trigger_event(self, "value_changed", { value = value })
					log.debug(TAG, "传感器值变更: %s [%s] -> %s", self.id, self.type, tostring(value))
				end
			end
			self.poll_timer = timer.setInterval(poll_func,self.poll_interval)
			log.info(TAG, "启动传感器轮询: %s [%dms]", self.id, self.poll_interval)
		end,
		-- 停止轮询
		stop_polling = function(self)
			if self.poll_timer then
				timer.clearInterval(self.poll_timer)
				self.poll_timer = nil
				log.info(TAG, "停止传感器轮询: %s", self.id)
			end
		end,
		-- 读取传感器值（由子类实现）
		read_value = function(self)
			log.warn(TAG, "未实现的read_value方法: %s", self.id)
			return nil
		end,
		-- 获取传感器状态
		get_status = function(self)
			return {
				value = self.last_value,
				updated_at = self.last_updated,
				status = self.status or "active"
			}
		end,
	}
)

-- 通用执行器模板
local generic_actuator = DeviceTemplate.create_template(
	base_template,
	{
		type = "actuator",
		gpio = nil,
		gpio_mode = gpio.MODE_OUTPUT,
		gpio_pull = gpio.PULLUP_DISABLE,
		status = "off", -- 默认状态
	},
	{
		-- 初始化执行器
		init = function(self, params)
			-- 调用基类初始化
			log.info(TAG, "generic_actuator 调用基类初始化")
			base_template._init_base(self, params)
			-- 配置GPIO
			if self.gpio then
				local ok, err = gpio.set_mode({
					pin = self.gpio,
					mode = self.gpio_mode,
					pull_up = self.gpio_pull == gpio.PULLUP_ENABLE,
					pull_down = self.gpio_pull == gpio.PULLDOWN_ENABLE,
					intr = gpio.INTR_DISABLE
				})
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
		-- 打开执行器
		on = function(self)
			if self.gpio then
				local ok, err = gpio.set_level(self.gpio, 1)
				if not ok then
					log.error(TAG, "设置GPIO失败 [%d]: %s", self.gpio, err)
					return false
				end
			end
			self:set_status("on")
			return true
		end,
		-- 关闭执行器
		off = function(self)
			if self.gpio then
				local ok, err = gpio.set_level(self.gpio, 0)
				if not ok then
					log.error(TAG, "设置GPIO失败 [%d]: %s", self.gpio, err)
					return false
				end
			end
			self:set_status("off")
			return true
		end,
		-- 切换执行器状态
		toggle = function(self)
			if self:get_status() == "on" then
				return self:off()
			else
				return self:on()
			end
		end,
	}
)

-- 数字输入传感器（如按钮）
local digital_input_sensor = DeviceTemplate.create_template(
	generic_sensor,
	{
		type = "button",
		gpio_mode = gpio.MODE_INPUT,
		gpio_pull = gpio.PULLUP_ENABLE,
		debounce = 50, -- 防抖时间(ms)
		last_state = nil,
		last_change = nil,
	},
	{
		-- 初始化按钮传感器
		init = function(self, params)
			-- 调用父类初始化
			generic_sensor.init(self, params)
			-- 设置中断
			if self.gpio then
				local ok, err = gpio.set_interrupt({
					pin = self.gpio,
					callback = function(pin, level)
						self:handle_interrupt(pin, level)
					end,
					intr = gpio.INTR_ANYEDGE
				})
				if not ok then
					log.error(TAG, "设置GPIO中断失败 [%d]: %s", self.gpio, err)
				else
					log.info(TAG, "GPIO中断设置成功 [%d]", self.gpio)
				end
			end
		end,
		-- 处理中断
		handle_interrupt = function(self, pin, level)
			-- 防抖处理
			local now = os.clock() * 1000 -- 转换为毫秒
			if self.last_change and (now - self.last_change) < self.debounce then
				return           -- 忽略抖动
			end
			self.last_change = now
			-- 触发事件
			local state = level == 1 and "pressed" or "released"
			if state ~= self.last_state then
				self.last_state = state
				trigger_event(self, "state_changed", { state = state, level = level })
				log.info(TAG, "按钮状态变更: %s [%s] -> %s", self.id, self.type, state)
			end
		end,
		-- 读取按钮状态
		read_value = function(self)
			if not self.gpio then
				return nil
			end
			local level = gpio.get_level(self.gpio)
			return level == 1 and "pressed" or "released"
		end,
	}
)

-- LED执行器
local led_actuator = DeviceTemplate.create_template(
	generic_actuator,
	{
		type = "led",
		gpio_mode = gpio.MODE_OUTPUT,
		gpio_pull = gpio.PULLUP_DISABLE,
		status = "off",
	},
	{
		-- 闪烁LED
		blink = function(self, interval, count)
			interval = interval or 500 -- 默认500ms
			count = count or 0 -- 0表示无限循环
			if self.blink_timer then
				timer.clearInterval(self.blink_timer)
				self.blink_timer = nil
			end
			local blink_count = 0
			local toggle_func = function()
				self:toggle()
				if count > 0 then
					blink_count = blink_count + 1
					if blink_count >= count * 2 then -- 一次完整闪烁是两次切换
						timer.clearInterval(self.blink_timer)
						self.blink_timer = nil
						self:off() -- 闪烁结束后关闭LED
					end
				end
			end
			self.blink_timer = timer.setInterval(toggle_func, interval)
			log.info(TAG, "LED开始闪烁: %s [%dms, %s]", self.id, interval, count > 0 and count .. "次" or "无限")
		end,
		-- 停止闪烁
		stop_blink = function(self)
			if self.blink_timer then
				timer.clearInterval(self.blink_timer)
				self.blink_timer = nil
				self:off() -- 停止后关闭LED
				log.info(TAG, "LED停止闪烁: %s", self.id)
			end
		end,
	}
)

-- 电机执行器
local motor_actuator = DeviceTemplate.create_template(
	generic_actuator,
	{
		type = "motor",
		gpio_mode = gpio.MODE_OUTPUT,
		gpio_pull = gpio.PULLUP_DISABLE,
		status = "stopped",
		speed_pin = nil, -- PWM控制引脚
		speed_channel = nil, -- PWM通道
		max_speed = 100, -- 最大速度
		current_speed = 0, -- 当前速度
	},
	{
		-- 初始化电机
		init = function(self, params)
			-- 调用父类初始化
			generic_actuator.init(self, params)
			-- 初始化PWM控制（如果有speed_pin）
			if self.speed_pin and self.speed_channel ~= nil then
				pwm.setup({
					pin = self.speed_pin,
					channel = self.speed_channel,
					freq = 1000, -- 默认频率1kHz
					duty = 0 -- 初始占空比0%
				})
				log.info(TAG, "电机PWM初始化: %s [pin=%d, channel=%d]",
					self.id, self.speed_pin, self.speed_channel)
			end
		end,
		-- 启动电机
		start = function(self, speed)
			speed = speed or self.max_speed             -- 默认使用最大速度
			speed = math.max(0, math.min(speed, self.max_speed)) -- 限制在0-max_speed范围内
			if self.speed_pin and self.speed_channel ~= nil then
				-- 设置PWM占空比
				local duty = math.floor(speed * 1023 / self.max_speed) -- 转换为0-1023范围
				pwm.set_duty(self.speed_channel, duty)
				pwm.start(self.speed_channel)
			end
			self.current_speed = speed
			self:set_status("running")
			log.info(TAG, "电机启动: %s [速度=%d]", self.id, speed)
		end,
		-- 停止电机
		stop = function(self)
			if self.speed_pin and self.speed_channel ~= nil then
				pwm.set_duty(self.speed_channel, 0)
				pwm.start(self.speed_channel)
			end
			self.current_speed = 0
			self:set_status("stopped")
			log.info(TAG, "电机停止: %s", self.id)
		end,
		-- 设置电机速度
		set_speed = function(self, speed)
			if self:get_status() == "stopped" then
				self:start(speed)
			else
				self:start(speed) -- start方法已经包含速度设置逻辑
			end
		end,
	}
)

-- 设备模板注册表
DeviceTemplate.templates = {
	-- 基础模板
	sensor = generic_sensor,
	actuator = generic_actuator,
	-- 预设模板
	button = digital_input_sensor,
	led = led_actuator,
	motor = motor_actuator,
}

-- 生成设备实例
function DeviceTemplate.new_device(template_name, params)
	local template = DeviceTemplate.templates[template_name]
	if not template then
		log.error(TAG, "未知设备模板: " .. tostring(template_name))
		return nil, "未知设备模板"
	else
		log.info(TAG, "使用设备模板: " .. template_name)
	end
	local device = {}
	-- 设置元表继承模板
	setmetatable(device, { __index = template })
	-- 初始化设备
	device:init(params)
	-- 验证设备
	if not device:is_valid() then
		log.error(TAG, "设备创建失败: 缺少必要参数")
		return nil, "设备创建失败: 缺少必要参数"
	end
	log.info(TAG, "创建设备: " .. device.id .. " [" .. device.type .. "]")
	return device
end

-- 验证设备模板是否正确注册
for name, _ in pairs(DeviceTemplate.templates) do
	log.info(TAG, "注册的设备模板: " .. name)
end

return DeviceTemplate
