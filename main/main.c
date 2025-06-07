#include "cron.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "init_littlefs.h"
#include "lua.h"
#include "lua_engine.h"

static const char* TAG = "main";
// main.c - 修改设备配置格式为更规范的Lua表
const char* device_config = "{ \
    base = { \
        mqtt = { \
            server = \"localhost\", \
            port = 1883, \
            client_id = \"esp32-01\", \
            keepalive = 60 \
        }, \
        system = { \
            log_level = \"info\", \
                update_interval = 60 \
            } \
        }, \
    }, \
    devices = { \
        { \
            id = \"led1\", \
            type = \"led\", \
            gpio = 15, \
            gpio_mode = 1, \
            gpio_pull = 0, \
            name = \"LED灯\" \
        }, \
        { \
            id = \"button1\", \
            type = \"button\", \
            gpio = 2, \
            gpio_mode = 0, \
            gpio_pull = 1, \
            name = \"按钮\" \
        } \
    } \
}";

const char* blink_rule_code[] = {
    "return {\n"
    "    id = \"blink_led1\",\n"
    "    name = \"Blink LED1\",\n"
    "    description = \"Blink LED1 every second\",\n"
    "    type = \"cron\",\n"
    "    schedule = \"*/1 * * * * *\", -- 每秒执行\n"
    "    on_init = function(self, config)\n"
    "        print(\"Blink rule initialized\")\n"
    "    end,\n"
    "    on_cron = function(self, rule_id)\n"
    "        local led = self:get_device_by_id(\"led1\")\n"
    "        if led and led.gpio then\n"
    "            print(\"Blinking LED on GPIO: \" .. led.gpio)\n"
    "            local gpio_status = gpio.get_level(led.gpio) -- 读取当前状态\n"
    "            gpio.set_level(led.gpio, gpio_status == 1 and 0 or 1)\n"
    "        else\n"
    "            print(\"LED device not found or set_level missing\")\n"
    "        end\n"
    "        print(\"Cron triggered for rule: \" .. rule_id)\n"
    "    end\n"
    "}\n",
    "return {\n"
    "    id = \"loop_log\",\n"
    "    name = \"loop log\",\n"
    "    description = \"log every second\",\n"
    "    type = \"cron\",\n"
    "    schedule = \"*/2 * * * * *\", -- 每2秒执行\n"
    "    on_cron = function(self, rule_id)\n"
    "        print(\"loop log:Cron triggered for rule: \" .. rule_id)\n"
    "    end\n"
    "}\n"
};

void lua_system_task(void* pvParameters)
{
    ESP_LOGI(TAG, "lua_system_task started");

    // 初始化Lua引擎
    if (lua_engine_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Lua engine");
        vTaskDelete(NULL);
        return;
    }

    lua_State* L = lua_engine_get_state();

    // 发送设备配置
    ESP_LOGI(TAG, "Sending device config to Lua...");
    // 验证注册

    lua_engine_send_config(device_config);
    // 添加配置验证代码
    ESP_LOGI(TAG, "Validating device config in Lua...");
    lua_getglobal(L, "require");
    lua_pushstring(L, "config_loader");
    lua_call(L, 1, 1); // 加载config_loader模块

    lua_getfield(L, -1, "validate_config");
    lua_pushstring(L, device_config);
    lua_call(L, 1, 1); // 调用validate_config函数

    bool config_valid = lua_toboolean(L, -1);
    lua_pop(L, 2); // 弹出结果和config_loader模块

    if (!config_valid) {
        ESP_LOGE(TAG, "设备配置验证失败");
    } else {
        ESP_LOGI(TAG, "设备配置验证成功");
    }

    // 添加规则
    ESP_LOGI(TAG, "Adding rules to Lua...");
    lua_engine_send_rules(blink_rule_code, 2);

    // 注册Cron回调并启动调度器
    cron_start();

    // 主循环
    while (1) {
        // 这里可以添加时间同步逻辑
        ESP_LOGI(TAG, "lua_system_task running...");

        // 定期检查Lua内存使用情况
        // size_t mem_used = lua_gc(L, LUA_GCCOUNT, 0);
        // ESP_LOGI(TAG, "Lua memory usage: %d KB", mem_used);

        vTaskDelay(pdMS_TO_TICKS(5000)); // 每5秒执行一次
    }
}

void app_main(void)
{
    // 初始化文件系统
    init_littlefs();

    // 创建Lua系统任务
    xTaskCreate(lua_system_task, "lua_system_task", 8192, NULL, 5, NULL);

    // 主循环 - 可以添加其他系统任务
    while (1) {
        // 这里可以添加其他低优先级任务
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "Main loop running...");
    }
}
