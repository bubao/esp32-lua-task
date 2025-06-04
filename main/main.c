#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lua_functions.h"

static const char* TAG = "main";

void app_main(void)
{
    init_littlefs();

    start_lua_system(); // 启动Lua任务

    while (1) {
        // 这里可以添加其他Lua任务逻辑
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
