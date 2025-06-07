-- config_loader.lua
-- 负责加载和解析配置，分离基础配置和设备配置
local DeviceRegistry = require("device_registry")
local DeviceTemplate = require("device_templates")
local log = require("log") -- 启用日志记录
local ConfigLoader = {}

-- 当前加载的配置
local current_config = nil

-- 基础配置校验规则
local base_config_rules = {
	mqtt = {
		required = true,
		type = "table",
		fields = {
			server = { required = true, type = "string" },
			port = { required = true, type = "number" },
			client_id = { required = false, type = "string" },
			username = { required = false, type = "string" },
			password = { required = false, type = "string" },
			keepalive = { required = false, type = "number" }
		}
	},
	system = {
		required = false,
		type = "table",
		fields = {
			log_level = { required = false, type = "string" },
			update_interval = { required = false, type = "number" }
		}
	}
}

-- 设备配置校验规则
local device_config_rules = {
	id = { required = true, type = "string" },
	type = { required = true, type = "string" },
	gpio = { required = true, type = "number" },
	gpio_mode = { required = false, type = "string" },
	gpio_pull = { required = false, type = "string" },
	enabled = { required = false, type = "boolean" },
	mode = { required = false, type = "string" }
}

-- 校验单个字段
local function validate_field(value, rule)
	if rule.required and value == nil then
		return false, "缺少必需字段"
	end
	if value ~= nil and type(value) ~= rule.type then
		return false, "字段类型错误，期望" .. rule.type
	end
	return true
end

-- 校验配置表
local function validate_config(config, rules)
	local errors = {}
	for field_name, rule in pairs(rules) do
		local value = config[field_name]
		-- 检查字段是否存在
		if rule.required and value == nil then
			table.insert(errors, string.format("缺少必需字段: %s", field_name))
			goto continue
		end
		-- 跳过不存在的可选字段
		if value == nil then
			goto continue
		end
		-- 检查类型
		if type(value) ~= rule.type then
			table.insert(errors, string.format("字段 %s 类型错误，期望 %s，实际 %s",
				field_name, rule.type, type(value)))
			goto continue
		end
		-- 如果是表类型，递归校验子字段
		if rule.type == "table" and rule.fields then
			local sub_errors = validate_config(value, rule.fields)
			for _, err in ipairs(sub_errors) do
				table.insert(errors, string.format("%s.%s", field_name, err))
			end
		end
		::continue::
	end
	return errors
end

-- 校验基础配置
function ConfigLoader.validate_base_config(base_config)
	if type(base_config) ~= "table" then
		return false, "基础配置必须是table类型"
	end
	local errors = validate_config(base_config, base_config_rules)
	if #errors > 0 then
		return false, table.concat(errors, "\n")
	end
	return true
end

-- 校验设备配置
function ConfigLoader.validate_device_config(device_config)
	if type(device_config) ~= "table" then
		return false, "设备配置必须是table类型"
	end
	-- 检查设备类型是否存在
	if not device_config.type then
		return false, "设备配置缺少type字段"
	end
	if not DeviceTemplate.templates[device_config.type] then
		return false, "未知设备类型: " .. device_config.type
	end
	local errors = validate_config(device_config, device_config_rules)
	if #errors > 0 then
		return false, table.concat(errors, "\n")
	end
	return true
end

--- 加载完整配置
-- @param config_table: 已解析的Lua表，包含base和devices
-- @return 解析结果或nil, error_message
function ConfigLoader.load(config_table)
	log.info("CONFIG_LOADER", "开始加载完整配置...")
	-- 验证基础配置
	local valid, err = ConfigLoader.validate_base_config(config_table.base)
	if not valid then
		log.error("CONFIG_LOADER", "基础配置错误: " .. err)
		return nil, "基础配置错误: " .. err
	end
	local base_config = config_table.base
	local device_configs = config_table.devices or {}
	-- 设备初始化
	local initialized_devices = {}
	local failed_devices = {}
	for _, dev_conf in ipairs(device_configs) do
		-- 添加防御性检查
		if not dev_conf or type(dev_conf) ~= "table" then
			log.error("CONFIG_LOADER", "无效的设备配置: 不是table类型")
			table.insert(failed_devices, { config = dev_conf, error = "无效的设备配置: 不是table类型" })
			goto next_device
		end
		local valid, err = ConfigLoader.validate_device_config(dev_conf)
		if not valid then
			log.error("CONFIG_LOADER", "无效设备配置: " .. err)
			table.insert(failed_devices, { config = dev_conf, error = err })
			goto next_device
		end
		local ok, device_or_err = pcall(DeviceTemplate.new_device, dev_conf.type, dev_conf)
		if not ok then
			log.error("CONFIG_LOADER", "设备创建失败: " .. device_or_err)
			table.insert(failed_devices, { config = dev_conf, error = device_or_err })
			goto next_device
		elseif not device_or_err then
			log.error("CONFIG_LOADER", "设备创建返回nil")
			table.insert(failed_devices, { config = dev_conf, error = "设备创建返回nil" })
			goto next_device
		end
		-- 注册设备
		local success, reg_err = pcall(DeviceRegistry.register, device_or_err)
		if not success then
			log.error("CONFIG_LOADER", "设备注册失败: " .. reg_err)
			table.insert(failed_devices, { config = dev_conf, error = reg_err })
		else
			log.info("CONFIG_LOADER", "成功初始化设备: " .. device_or_err.id)
			table.insert(initialized_devices, device_or_err)
		end
		::next_device::
	end
	-- 构建返回结果
	local result = {
		base = base_config,
		devices = {
			total = #device_configs,
			initialized = #initialized_devices,
			failed = #failed_devices,
			list = initialized_devices,
			failed_list = failed_devices
		}
	}
	-- 保存当前配置
	current_config = result
	log.info("CONFIG_LOADER", "配置加载完成: 成功 %d 个, 失败 %d 个",
		#initialized_devices, #failed_devices)
	-- 如果所有设备都失败，返回错误
	if #initialized_devices == 0 and #device_configs > 0 then
		return nil, "所有设备初始化失败", result
	end
	return result
end

--- 单独加载基础配置
-- @param base_config: 基础配置表
-- @return 解析后的基础配置或nil, error_message
function ConfigLoader.load_base(base_config)
	log.info("CONFIG_LOADER", "加载基础配置...")
	local valid, err = ConfigLoader.validate_base_config(base_config)
	if not valid then
		log.error("CONFIG_LOADER", "load_base 基础配置错误: " .. err)
		return nil, err
	end
	-- 保存基础配置
	if current_config then
		current_config.base = base_config
	else
		current_config = { base = base_config, devices = { list = {} } }
	end
	log.info("CONFIG_LOADER", "基础配置加载成功")
	return base_config
end

--- 单独加载设备配置
-- @param device_configs: 设备配置表数组
-- @return 设备初始化结果或nil, error_message
function ConfigLoader.load_devices(device_configs)
	log.info("CONFIG_LOADER", "开始加载设备配置...")
	if type(device_configs) ~= "table" then
		log.error("CONFIG_LOADER", "设备配置必须是table数组")
		return nil, "设备配置必须是table数组"
	end
	-- 设备初始化
	local initialized_devices = {}
	local failed_devices = {}
	-- 先清除现有设备
	if current_config and current_config.devices then
		for _, device in ipairs(current_config.devices.list) do
			if device.unregister then
				pcall(device.unregister, device)
			end
		end
	end
	for _, dev_conf in ipairs(device_configs) do
		-- 添加防御性检查
		if not dev_conf or type(dev_conf) ~= "table" then
			log.error("CONFIG_LOADER", "无效的设备配置: 不是table类型")
			table.insert(failed_devices, { config = dev_conf, error = "无效的设备配置: 不是table类型" })
			goto next_device
		end
		local valid, err = ConfigLoader.validate_device_config(dev_conf)
		if not valid then
			log.error("CONFIG_LOADER", "无效设备配置: " .. err)
			table.insert(failed_devices, { config = dev_conf, error = err })
			goto next_device
		end
		local ok, device_or_err = pcall(DeviceTemplate.new_device, dev_conf.type, dev_conf)
		if not ok then
			log.error("CONFIG_LOADER", "设备创建失败: " .. device_or_err)
			table.insert(failed_devices, { config = dev_conf, error = device_or_err })
			goto next_device
		elseif not device_or_err then
			log.error("CONFIG_LOADER", "设备创建返回nil")
			table.insert(failed_devices, { config = dev_conf, error = "设备创建返回nil" })
			goto next_device
		end
		-- 注册设备
		local success, reg_err = pcall(DeviceRegistry.register, device_or_err)
		if not success then
			log.error("CONFIG_LOADER", "设备注册失败: " .. reg_err)
			table.insert(failed_devices, { config = dev_conf, error = reg_err })
		else
			log.info("CONFIG_LOADER", "成功初始化设备: " .. device_or_err.id)
			table.insert(initialized_devices, device_or_err)
		end
		::next_device::
	end
	-- 构建返回结果
	local result = {
		total = #device_configs,
		initialized = #initialized_devices,
		failed = #failed_devices,
		list = initialized_devices,
		failed_list = failed_devices
	}
	-- 保存设备配置
	if current_config then
		current_config.devices = result
	else
		current_config = { base = {}, devices = result }
	end
	log.info("CONFIG_LOADER", "设备配置加载完成: 成功 %d 个, 失败 %d 个",
		#initialized_devices, #failed_devices)
	-- 如果所有设备都失败，返回错误
	if #initialized_devices == 0 and #device_configs > 0 then
		return nil, "所有设备初始化失败", result
	end
	return result
end

--- 更新配置（合并新配置到现有配置）
-- @param new_config: 新配置表
-- @return 更新后的配置或nil, error_message
function ConfigLoader.update_config(new_config)
	log.info("CONFIG_LOADER", "开始更新配置...")
	if not current_config then
		log.info("CONFIG_LOADER", "没有现有配置，执行完整加载")
		return ConfigLoader.load(new_config)
	end
	-- 更新基础配置
	if new_config.base then
		local valid, err = ConfigLoader.validate_base_config(new_config.base)
		if not valid then
			log.error("CONFIG_LOADER", "新基础配置无效: " .. err)
			return nil, "新基础配置无效: " .. err
		end
		-- 合并基础配置（保留现有配置中不存在的字段）
		for key, value in pairs(new_config.base) do
			current_config.base[key] = value
		end
		log.info("CONFIG_LOADER", "基础配置更新成功")
	end
	-- 更新设备配置
	if new_config.devices then
		-- 先卸载所有现有设备
		for _, device in ipairs(current_config.devices.list or {}) do
			if device.unregister then
				pcall(device.unregister, device)
			end
		end
		-- 加载新设备
		local device_result, err = ConfigLoader.load_devices(new_config.devices)
		if not device_result then
			log.error("CONFIG_LOADER", "设备配置更新失败: " .. err)
			return nil, "设备配置更新失败: " .. err
		end
		log.info("CONFIG_LOADER", "设备配置更新成功")
	end
	log.info("CONFIG_LOADER", "配置更新完成")
	return current_config
end

--- 加载默认配置
-- @return 配置结果或nil, error_message
function ConfigLoader.load_default()
	log.info("CONFIG_LOADER", "加载默认配置...")
	-- 这里应该从默认源加载配置，例如内置配置或文件
	-- 以下是示例默认配置，实际实现可能需要根据项目需求调整
	local default_config = {
		base = {
			mqtt = {
				server = "localhost",
				port = 1883,
				client_id = "esp32-device",
				keepalive = 60
			},
			system = {
				log_level = "info",
				update_interval = 60
			}
		},
		devices = {
			{
				id = "led1",
				type = "led",
				gpio = 2,
				enabled = true
			},
			{
				id = "button1",
				type = "button",
				gpio = 4,
				enabled = true
			}
		}
	}
	return ConfigLoader.load(default_config)
end

--- 获取当前配置
-- @return 当前配置
function ConfigLoader.get_current_config()
	return current_config
end

return ConfigLoader
