local DeviceRegistry = {}

local devices = {}

function DeviceRegistry.register(device)
    if not device or not device.id then
        error("设备必须有唯一的 id 字段")
    end
    if devices[device.id] then
        error("设备ID已存在: " .. tostring(device.id))
    end
	---@diagnostic disable-next-line: undefined-global
	if type(c_device_initializer) ~= "function" then
        print("[DeviceRegistry] 警告: c_device_initializer 未注册")
    else
		---@diagnostic disable-next-line: undefined-global
        local ok, err = pcall(c_device_initializer, device)
        if not ok then
            print("[DeviceRegistry] 设备初始化失败: " .. tostring(err))
        end
    end

    devices[device.id] = device
    print("[DeviceRegistry] 注册设备: " .. tostring(device.id))
end


function DeviceRegistry.get(id)
    return devices[id]
end

function DeviceRegistry.list_all()
    local list = {}
    for _, dev in pairs(devices) do
        table.insert(list, dev)
    end
    return list
end

function DeviceRegistry.remove(id)
    if devices[id] then
        devices[id] = nil
        print("[DeviceRegistry] 删除设备: " .. tostring(id))
        return true
    end
    return false
end

return DeviceRegistry
