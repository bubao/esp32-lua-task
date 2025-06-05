-- device_registry.lua
-- 设备注册与管理模块

local DeviceRegistry = {}

-- 用于存储所有注册的设备
local devices = {}

--- 注册设备
-- @param device table，设备对象，要求至少有唯一id字段
function DeviceRegistry.register(device)
    if not device or not device.id then
        error("设备必须有唯一的 id 字段")
    end
    if devices[device.id] then
        error("设备ID已存在: " .. tostring(device.id))
    end
    devices[device.id] = device
    print("[DeviceRegistry] 注册设备: " .. tostring(device.id))
end

--- 根据设备ID获取设备
-- @param id string
-- @return device table 或 nil
function DeviceRegistry.get(id)
    return devices[id]
end

--- 获取所有设备列表
-- @return table 数组，包含所有设备对象
function DeviceRegistry.list_all()
    local list = {}
    for _, dev in pairs(devices) do
        table.insert(list, dev)
    end
    return list
end

--- 删除设备
-- @param id string
-- @return boolean 是否成功删除
function DeviceRegistry.remove(id)
    if devices[id] then
        devices[id] = nil
        print("[DeviceRegistry] 删除设备: " .. tostring(id))
        return true
    end
    return false
end

return DeviceRegistry
