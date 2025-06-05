-- config_loader.lua
-- 负责接收C端传入的Lua表配置，初始化设备，返回mqtt配置

local DeviceRegistry = require("device_registry")
local DeviceTemplate = require("device_templates")

local ConfigLoader = {}

--- 加载配置
-- @param config_table: 已解析的Lua表，包含 devices 和 mqtt
-- @return mqtt_config table 或 nil, error_message
function ConfigLoader.load(config_table)
    if type(config_table) ~= "table" then
        return nil, "配置参数必须是table类型"
    end

    if type(config_table.devices) ~= "table" then
        return nil, "配置缺少 devices 字段或格式错误"
    end
    if type(config_table.mqtt) ~= "table" then
        return nil, "配置缺少 mqtt 字段或格式错误"
    end

    -- 设备初始化
    for _, dev_conf in ipairs(config_table.devices) do
        if not dev_conf.type or not dev_conf.gpio then
            print("[ConfigLoader] 跳过无效设备配置，缺少type或gpio字段")
        else
            local ok, device_or_err = pcall(DeviceTemplate.new_device, dev_conf.type, dev_conf)
            if not ok then
                print("[ConfigLoader] 设备创建失败:", device_or_err)
            elseif device_or_err then
                DeviceRegistry.register(device_or_err)
            end
        end
    end

    return config_table  -- 直接返回整个配置表
end

return ConfigLoader
