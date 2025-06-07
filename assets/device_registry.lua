-- device_registry.lua - 设备注册表系统
local log = require("log")

local DeviceRegistry = {}
local devices = {}
local device_types = {
	["sensor"] = true,
	["actuator"] = true,
	["led"] = true, -- 添加新的设备类型
	["button"] = true -- 添加新的设备类型
}

local TAG = "DEVICE_REGISTRY"

-- 事件管理器
local EventManager = {
	listeners = {},

	on = function(self, event, callback)
		if type(event) ~= "string" or type(callback) ~= "function" then
			log.error(TAG, "无效的事件监听器参数")
			return false
		end

		self.listeners[event] = self.listeners[event] or {}
		table.insert(self.listeners[event], callback)
		log.debug(TAG, "添加事件监听器: %s", event)
		return true
	end,

	emit = function(self, event, ...)
		local listeners = self.listeners[event]
		if not listeners then return end

		log.debug(TAG, "触发事件: %s, 监听器数量: %d", event, #listeners)

		for _, callback in ipairs(listeners) do
			local ok, err = pcall(callback, ...)
			if not ok then
				log.error(TAG, "执行事件回调失败 [%s]: %s", event, err)
			end
		end
	end
}

-- 设备验证
local function validate_device(device)
	if not device then return false, "设备不能为空" end
	if not device.id then return false, "设备必须有ID" end
	if type(device.id) ~= "string" then return false, "设备ID必须是字符串" end
	if devices[device.id] then return false, "设备ID已存在: " .. device.id end

	if not device.type then return false, "设备必须有类型" end
	if not device_types[device.type] then return false, "未知设备类型: " .. device.type end

	-- 检查必要方法
	if device.type == "sensor" and type(device.update_value) ~= "function" then
		return false, "传感器设备必须实现update_value方法"
	end

	if device.type == "actuator" and type(device.set_status) ~= "function" then
		return false, "执行器设备必须实现set_status方法"
	end

	if device.type == "led" and type(device.set_status) ~= "function" then
		return false, "LED设备必须实现set_status方法"
	end

	if device.type == "button" and type(device.get_status) ~= "function" then
		return false, "按钮设备必须实现get_status方法"
	end

	return true
end

-- 注册设备
function DeviceRegistry.register(device)
	local valid, err = validate_device(device)
	if not valid then
		log.error(TAG, "无效设备: %s", err)
		return nil, err
	end

	-- 添加到注册表
	devices[device.id] = device
	log.info(TAG, "注册设备: " .. device.id .. " [" .. device.type .. "]")

	-- 触发注册事件
	EventManager:emit("device_registered", device)

	return device
end

-- 获取设备
function DeviceRegistry.get(id)
	if not id then
		log.warn(TAG, "获取设备时ID为空")
		return nil
	end

	local device = devices[id]
	if not device then
		log.debug(TAG, "未找到设备: %s", id)
	end

	return device
end

-- 获取所有设备
function DeviceRegistry.list_all()
	local result = {}
	for id, device in pairs(devices) do
		table.insert(result, device)
	end

	log.debug(TAG, "获取所有设备，总数: %d", #result)
	return result
end

-- 获取指定类型的设备
function DeviceRegistry.list_by_type(type_name)
	if not type_name or not device_types[type_name] then
		log.error(TAG, "无效的设备类型: %s", tostring(type_name))
		return {}
	end

	local result = {}
	for id, device in pairs(devices) do
		if device.type == type_name then
			table.insert(result, device)
		end
	end

	log.debug(TAG, "获取类型为 [%s] 的设备，总数: %d", type_name, #result)
	return result
end

-- 移除设备
function DeviceRegistry.remove(id)
	if not id then
		log.error(TAG, "移除设备时ID为空")
		return false, "ID不能为空"
	end

	local device = devices[id]
	if not device then
		log.warn(TAG, "移除不存在的设备: %s", id)
		return false, "设备不存在"
	end

	-- 触发移除事件
	EventManager:emit("device_removed", device)

	-- 清理设备资源
	if type(device.cleanup) == "function" then
		device:cleanup()
	end

	-- 从注册表移除
	devices[id] = nil
	log.info(TAG, "移除设备: %s [%s]", id, device.type)

	return true
end

-- 更新设备状态
function DeviceRegistry.update_status(id, status)
	local device = devices[id]
	if not device then
		log.warn(TAG, "更新不存在的设备状态: %s", id)
		return false, "设备不存在"
	end

	if type(device.set_status) ~= "function" then
		log.error(TAG, "设备 [%s] 不支持设置状态", id)
		return false, "设备不支持设置状态"
	end

	local ok, err = device:set_status(status)
	if not ok then
		log.error(TAG, "更新设备状态失败 [%s]: %s", id, err or "")
	else
		log.info(TAG, "设备状态更新成功 [%s]: %s", id, tostring(status))
	end

	return ok, err
end

-- 更新传感器值
function DeviceRegistry.update_sensor_value(id, value)
	local device = devices[id]
	if not device then
		log.warn(TAG, "更新不存在的设备值: %s", id)
		return false, "设备不存在"
	end

	if type(device.update_value) ~= "function" then
		log.error(TAG, "设备 [%s] 不支持更新值", id)
		return false, "设备不支持更新值"
	end

	local ok, err = device:update_value(value)
	if not ok then
		log.error(TAG, "更新传感器值失败 [%s]: %s", id, err or "")
	else
		log.info(TAG, "传感器值更新成功 [%s]: %s", id, tostring(value))
	end

	return ok, err
end

-- 添加事件监听器
function DeviceRegistry.on(event, callback)
	return EventManager:on(event, callback)
end

-- 轮询所有传感器
function DeviceRegistry.poll_all_sensors()
	local now_ms = os.time() * 1000
	local polled_count = 0

	for id, device in pairs(devices) do
		if device.type == "sensor" and type(device.poll) == "function" then
			-- 检查轮询间隔
			local interval = device.poll_interval or 1000
			if not device.last_poll or (now_ms - device.last_poll) >= interval then
				local ok, err = pcall(device.poll, device)
				if not ok then
					log.error(TAG, "轮询传感器失败 [%s]: %s", id, err)
				else
					polled_count = polled_count + 1
				end
				device.last_poll = now_ms
			end
		end
	end

	log.info(TAG, "传感器轮询完成，已轮询: %d/%d", polled_count, #devices)
end

-- 批量注册设备
function DeviceRegistry.register_batch(device_list)
	if not device_list or type(device_list) ~= "table" then
		log.error(TAG, "批量注册设备时参数无效")
		return 0, "参数必须是表"
	end

	local success_count = 0
	local failed_count = 0

	for _, device in ipairs(device_list) do
		local result, err = DeviceRegistry.register(device)
		if result then
			success_count = success_count + 1
		else
			failed_count = failed_count + 1
			log.error(TAG, "批量注册失败 [%s]: %s", device.id or "unknown", err)
		end
	end

	log.info(TAG, "批量注册完成: 成功 %d 个, 失败 %d 个", success_count, failed_count)
	return success_count, failed_count
end

-- 批量更新设备状态
function DeviceRegistry.update_batch_status(device_status_list)
	if not device_status_list or type(device_status_list) ~= "table" then
		log.error(TAG, "批量更新设备状态时参数无效")
		return 0, "参数必须是表"
	end

	local success_count = 0
	local failed_count = 0

	for _, item in ipairs(device_status_list) do
		if item.id and item.status then
			local ok, err = DeviceRegistry.update_status(item.id, item.status)
			if ok then
				success_count = success_count + 1
			else
				failed_count = failed_count + 1
				log.error(TAG, "批量更新失败 [%s]: %s", item.id, err)
			end
		else
			failed_count = failed_count + 1
			log.error(TAG, "批量更新参数错误: 缺少id或status")
		end
	end

	log.info(TAG, "批量更新完成: 成功 %d 个, 失败 %d 个", success_count, failed_count)
	return success_count, failed_count
end

-- 查找设备
function DeviceRegistry.find(filter_func)
	if type(filter_func) ~= "function" then
		log.error(TAG, "查找设备时过滤函数无效")
		return {}
	end

	local result = {}
	for id, device in pairs(devices) do
		if filter_func(device) then
			table.insert(result, device)
		end
	end

	log.debug(TAG, "查找设备完成，匹配数量: %d", #result)
	return result
end

-- 获取注册表统计信息
function DeviceRegistry.get_stats()
	local sensor_count = 0
	local actuator_count = 0
	local led_count = 0
	local button_count = 0

	for _, device in pairs(devices) do
		if device.type == "sensor" then
			sensor_count = sensor_count + 1
		elseif device.type == "actuator" then
			actuator_count = actuator_count + 1
		elseif device.type == "led" then
			led_count = led_count + 1
		elseif device.type == "button" then
			button_count = button_count + 1
		end
	end

	return {
		total = #devices,
		sensors = sensor_count,
		actuators = actuator_count,
		leds = led_count,
		buttons = button_count,
		online = 0, -- 可扩展为在线设备统计
		offline = 0 -- 可扩展为离线设备统计
	}
end

-- 清理所有设备
function DeviceRegistry.cleanup()
	log.info(TAG, "清理所有设备资源...")

	for id, device in pairs(devices) do
		if type(device.cleanup) == "function" then
			device:cleanup()
		end

		EventManager:emit("device_removed", device)
	end

	devices = {}
	log.info(TAG, "设备注册表已清空")
end

return DeviceRegistry
