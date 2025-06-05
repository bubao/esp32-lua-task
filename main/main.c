#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "init_littlefs.h"
#include "lua_engine.h"

static const char* TAG = "main";
const char* device_config = "{ \
    mqtt = { \
        host = 'broker.hivemq.com', \
        port = 1883, \
        client_id = 'esp32-01' \
    }, \
    devices = { \
        { id = 'led1', type = 'led', gpio = 15, dir = 'output' } \
    } \
}";

const char* blink_rule_code[] = {
    "return {\n"
    "    id = \"blink_gpio15\",\n"
    "    on_init = function(self)\n"
    "        local pin = 15\n"
    "        local count = 0\n"
    "        local interval = 1000\n"
    "        local gpio = require(\"gpio\")\n"
    "        gpio.setup(pin, gpio.OUTPUT)\n"
    "        self.timer = self:start_timer(interval, function()\n"
    "            count = (count + 1) % 2\n"
    "            gpio.write(pin, count)\n"
    "        end)\n"
    "    end\n"
    "}\n"
};

void lua_system_task(void* pvParameters)
{
    ESP_LOGI(TAG, "lua_system_task started");
    lua_engine_init();
    ESP_LOGI(TAG, "Sending device config to Lua...");
    lua_engine_send_config(device_config); // 内部会调用 on_json_received.lua
    ESP_LOGI(TAG, "Adding blink rule to Lua...");
    lua_engine_send_rules(blink_rule_code, 1);

    while (1) {
        // 这里可以添加时间同步逻辑
        ESP_LOGI(TAG, "lua_system_task running...");
        vTaskDelay(pdMS_TO_TICKS(5000)); // 每5秒执行一次
    }
}

void app_main(void)
{
    init_littlefs();

    xTaskCreate(lua_system_task, "lua_system_task", 4096, NULL, 5, NULL);

    while (1) {
        // 这里可以添加其他Lua任务逻辑
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "Main loop running...");
    }
}
