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
        { id = 'led1', type = 'led', gpio = 15, gpio_mode = 'input_output',gpio_type = 'gpio',name = 'led 灯' } \
    } \
}";

const char* blink_rule_code[] = {
    "return {\n"
    "    id = \"blink_led1\",\n"
    "    name = \"Blink LED1\",\n"
    "    description = \"Blink LED1 every second\",\n"
    "    devices = { \"led1\" },\n"
    "    type = \"cron\",\n"
    "    schedule = \"* * * * * *\", -- 每秒执行\n"
    "    on_init = function(self, config)\n"
    "        print(\"Blink rule initialized\")\n"
    "    end,\n"
    "    on_cron = function(self, rule_id)\n"
    "        -- 这里可以添加定时任务逻辑\n"
    "        -- 例如控制 GPIO 输出\n"
    "        gpio.set_level(15, 1) -- 打开 LED\n"
    "        print(\"Cron triggered for rule: \" .. rule_id)\n"
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

    xTaskCreate(lua_system_task, "lua_system_task", 8096, NULL, 5, NULL);

    while (1) {
        // 这里可以添加其他Lua任务逻辑
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "Main loop running...");
    }
}
