pin = 15
threshold = 50

local function setup()
    gpio.set_mode(pin, gpio.MODE_INPUT_OUTPUT)
end

local function loop()
    while true do
        local val = read_sensor()
        log("Sensor value: " .. val)
        level = gpio.get_level(pin)
        log("Current GPIO level: " .. level)
        gpio.set_level(pin, level==0 and 1 or 0)
        coroutine.yield(0.5*1000*1000)
    end
end

return {
    setup = setup,
    loop = loop,
}