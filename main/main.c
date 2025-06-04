#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lua_functions.h"

static const char* TAG = "main";

void start_lua_system_task(void* pvParameters)
{
    start_lua_system();
    ESP_LOGI(TAG, "Time sync task started");
    while (1) {
        // 这里可以添加时间同步逻辑
        ESP_LOGI(TAG, "Time sync task running...");
        vTaskDelay(pdMS_TO_TICKS(5000)); // 每5秒执行一次
    }
}

void app_main(void)
{
    init_littlefs();

    xTaskCreate(start_lua_system_task, "start_lua_system_task", 4096, NULL, 5, NULL);

    while (1) {
        // 这里可以添加其他Lua任务逻辑
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "Main loop running...");
    }
}
