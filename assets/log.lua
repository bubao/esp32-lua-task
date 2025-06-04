pin = 15
threshold = 50

local function setup()
    gpio.set_mode(pin, gpio.MODE_INPUT_OUTPUT)
end

local function loop()
    local val = read_sensor()
    log("Sensor value: " .. val)
    -- 你可以根据阈值做一些判断
    -- if val > threshold then
    --     log("Value above threshold!")
    -- end
    return settimeout(1000) -- C端负责调度下一次
end

return {
    setup = setup,
    loop = loop
}