-- config_loader.lua
-- 负责加载和解析配置，分离基础配置和设备配置

local DeviceRegistry = require("device_registry")
local DeviceTemplate = require("device_templates")
-- local log = require("log")

local ConfigLoader = {}

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
    if device_config.type and not DeviceTemplate.templates[device_config.type] then
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
    -- 验证基础配置
    local valid, err = ConfigLoader.validate_base_config(config_table.base)
    if not valid then
        return nil, "基础配置错误: " .. err
    end
    
    local base_config = config_table.base
    local device_configs = config_table.devices or {}
    
    -- 设备初始化
    local initialized_devices = {}
    local failed_devices = {}
    
    for _, dev_conf in ipairs(device_configs) do
        local valid, err = ConfigLoader.validate_device_config(dev_conf)
        if not valid then
            print("[ConfigLoader] 无效设备配置:", err)
            table.insert(failed_devices, {config = dev_conf, error = err})
        else
            local ok, device_or_err = pcall(DeviceTemplate.new_device, dev_conf.type, dev_conf)
            if not ok then
                print("[ConfigLoader] 设备创建失败:", device_or_err)
                table.insert(failed_devices, {config = dev_conf, error = device_or_err})
            elseif device_or_err then
                -- 注册设备
                local success, reg_err = pcall(DeviceRegistry.register, device_or_err)
                if not success then
                    print("[ConfigLoader] 设备注册失败:", reg_err)
                    table.insert(failed_devices, {config = dev_conf, error = reg_err})
                else
                    print("[ConfigLoader] 成功初始化设备:", device_or_err.id)
                    table.insert(initialized_devices, device_or_err)
                end
            end
        end
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
    local valid, err = ConfigLoader.validate_base_config(base_config)
    if not valid then
        return nil, err
    end
    
    return base_config
end

--- 单独加载设备配置
-- @param device_configs: 设备配置表数组
-- @return 设备初始化结果或nil, error_message
function ConfigLoader.load_devices(device_configs)
    if type(device_configs) ~= "table" then
        return nil, "设备配置必须是table数组"
    end
    
    -- 设备初始化
    local initialized_devices = {}
    local failed_devices = {}
    
    for _, dev_conf in ipairs(device_configs) do
        local valid, err = ConfigLoader.validate_device_config(dev_conf)
        if not valid then
            print("[ConfigLoader] 无效设备配置:", err)
            table.insert(failed_devices, {config = dev_conf, error = err})
        else
            local ok, device_or_err = pcall(DeviceTemplate.new_device, dev_conf.type, dev_conf)
            if not ok then
                print("[ConfigLoader] 设备创建失败:", device_or_err)
                table.insert(failed_devices, {config = dev_conf, error = device_or_err})
            elseif device_or_err then
                -- 注册设备
                local success, reg_err = pcall(DeviceRegistry.register, device_or_err)
                if not success then
                    print("[ConfigLoader] 设备注册失败:", reg_err)
                    table.insert(failed_devices, {config = dev_conf, error = reg_err})
                else
                    print("[ConfigLoader] 成功初始化设备:", device_or_err.id)
                    table.insert(initialized_devices, device_or_err)
                end
            end
        end
    end
    
    -- 构建返回结果
    local result = {
        total = #device_configs,
        initialized = #initialized_devices,
        failed = #failed_devices,
        list = initialized_devices,
        failed_list = failed_devices
    }
    
    -- 如果所有设备都失败，返回错误
    if #initialized_devices == 0 and #device_configs > 0 then
        return nil, "所有设备初始化失败", result
    end
    
    return result
end

return ConfigLoader
