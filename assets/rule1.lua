pin = 15
threshold = 50

local function setup()
    -- gpio.set_mode(pin, gpio.MODE_INPUT_OUTPUT)
end

local function loop()
    while true do
        log("loop tick")
        local val = read_sensor()
        log("loop value: " .. val)
        coroutine.yield(1*1000*1000)
    end
end

return {
    setup = setup,
    loop = loop
}