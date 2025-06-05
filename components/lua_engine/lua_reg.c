#include "lua_reg.h"
#include "driver/adc.h"
#include "driver/adc_types_legacy.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "lauxlib.h"
#include "lua.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// 将字符串解析为 GPIO 模式
static gpio_mode_t parse_gpio_mode(const char* mode_str)
{
    if (strcmp(mode_str, "input") == 0) {
        return GPIO_MODE_INPUT;
    } else if (strcmp(mode_str, "output") == 0) {
        return GPIO_MODE_OUTPUT;
    } else if (strcmp(mode_str, "input_output") == 0 || strcmp(mode_str, "inputoutput") == 0) {
        return GPIO_MODE_INPUT_OUTPUT;
    }
    // 默认返回非法值，调用处要判断
    return -1;
}

// GPIO 初始化封装
static void gpio_init_ex(int gpio_num, gpio_mode_t mode, bool pullup, bool pulldown)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << gpio_num,
        .mode = mode,
        .pull_up_en = pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = pulldown ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        printf("[C][ERROR] GPIO 配置失败 GPIO=%d err=%d\n", gpio_num, err);
    } else {
        printf("[C] GPIO 初始化完成 GPIO=%d mode=%d pullup=%d pulldown=%d\n", gpio_num, mode, pullup, pulldown);
    }
}

// PWM 初始化封装
static void pwm_init_ex(int gpio_num, int channel, int freq_hz, int duty)
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_SPEED_MODE_MAX,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .freq_hz = freq_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&ledc_timer);
    if (err != ESP_OK) {
        printf("[C][ERROR] PWM Timer 配置失败 err=%d\n", err);
        return;
    }

    ledc_channel_config_t ledc_channel = {
        .gpio_num = gpio_num,
        .speed_mode = LEDC_SPEED_MODE_MAX,
        .channel = channel,
        .timer_sel = LEDC_TIMER_0,
        .duty = duty,
        .hpoint = 0,
    };
    err = ledc_channel_config(&ledc_channel);
    if (err != ESP_OK) {
        printf("[C][ERROR] PWM Channel 配置失败 GPIO=%d 通道=%d err=%d\n", gpio_num, channel, err);
    } else {
        printf("[C] PWM 初始化完成 GPIO=%d 通道=%d 频率=%dHz 占空比=%d\n", gpio_num, channel, freq_hz, duty);
    }
}

// ADC 初始化封装
static void adc_init_ex(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten)
{
    if (unit == ADC_UNIT_1) {
        adc1_config_width(ADC_WIDTH_BIT_DEFAULT); // 示例: 12位宽度
        esp_err_t err = adc1_config_channel_atten(channel, atten);
        if (err != ESP_OK) {
            printf("[C][ERROR] ADC 通道配置失败 单元=%d 通道=%d err=%d\n", unit, channel, err);
        } else {
            printf("[C] ADC 初始化完成 单元=%d 通道=%d 衰减=%d\n", unit, channel, atten);
        }
    } else {
        // 这里只简单示意，ESP32 ADC2 初始化需要特殊处理
        printf("[C][WARN] 仅支持 ADC1 单元初始化，unit=%d 不支持\n", unit);
    }
}

// Lua 设备初始化入口
int l_init_device(lua_State* L)
{
    if (!lua_istable(L, 1)) {
        return luaL_error(L, "参数必须是 table");
    }

    // 读取 id 字段
    const char* id_str = "unknown";
    lua_getfield(L, 1, "id");
    if (lua_isstring(L, -1)) {
        id_str = lua_tostring(L, -1);
    }
    lua_pop(L, 1);

    // 读取 name 字段
    const char* name_str = "";
    lua_getfield(L, 1, "name");
    if (lua_isstring(L, -1)) {
        name_str = lua_tostring(L, -1);
    }
    lua_pop(L, 1);

    // 读取 gpio_type 字段
    lua_getfield(L, 1, "gpio_type");
    if (!lua_isstring(L, -1)) {
        return luaL_error(L, "设备必须包含有效的 gpio_type 字段");
    }
    const char* gpio_type_str = lua_tostring(L, -1);
    lua_pop(L, 1);

    printf("[C] 初始化设备 ID=%s 名称=%s 类型=%s\n", id_str, name_str, gpio_type_str);

    if (strcmp(gpio_type_str, "gpio") == 0) {
        lua_getfield(L, 1, "gpio");
        if (!lua_isinteger(L, -1)) {
            return luaL_error(L, "GPIO 设备必须包含 gpio 字段");
        }
        int gpio = lua_tointeger(L, -1);
        lua_pop(L, 1);

        gpio_mode_t mode = GPIO_MODE_OUTPUT;
        lua_getfield(L, 1, "gpio_mode");
        if (lua_isstring(L, -1)) {
            const char* mode_str = lua_tostring(L, -1);
            gpio_mode_t mode_parsed = parse_gpio_mode(mode_str);
            if (mode_parsed == -1) {
                return luaL_error(L, "不支持的 gpio mode 值: %s", mode_str);
            }
            mode = mode_parsed;
        }
        lua_pop(L, 1);

        bool pullup = false;
        lua_getfield(L, 1, "pullup");
        if (lua_isboolean(L, -1)) {
            pullup = lua_toboolean(L, -1);
        }
        lua_pop(L, 1);

        bool pulldown = false;
        lua_getfield(L, 1, "pulldown");
        if (lua_isboolean(L, -1)) {
            pulldown = lua_toboolean(L, -1);
        }
        lua_pop(L, 1);

        gpio_init_ex(gpio, mode, pullup, pulldown);

    } else if (strcmp(gpio_type_str, "pwm") == 0) {
        lua_getfield(L, 1, "gpio");
        if (!lua_isinteger(L, -1)) {
            return luaL_error(L, "PWM 设备必须包含 gpio 字段");
        }
        int gpio = lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 1, "channel");
        if (!lua_isinteger(L, -1)) {
            return luaL_error(L, "PWM 设备必须包含 channel 字段");
        }
        int channel = lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 1, "freq");
        int freq = 5000;
        if (lua_isinteger(L, -1)) {
            freq = lua_tointeger(L, -1);
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "duty");
        int duty = 0;
        if (lua_isinteger(L, -1)) {
            duty = lua_tointeger(L, -1);
        }
        lua_pop(L, 1);

        pwm_init_ex(gpio, channel, freq, duty);

    } else if (strcmp(gpio_type_str, "adc") == 0) {
        lua_getfield(L, 1, "unit");
        if (!lua_isinteger(L, -1)) {
            return luaL_error(L, "ADC 设备必须包含 unit 字段");
        }
        int unit = lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 1, "channel");
        if (!lua_isinteger(L, -1)) {
            return luaL_error(L, "ADC 设备必须包含 channel 字段");
        }
        int channel = lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 1, "atten");
        int atten = ADC_ATTEN_DB_0;
        if (lua_isinteger(L, -1)) {
            atten = lua_tointeger(L, -1);
        }
        lua_pop(L, 1);

        adc_init_ex((adc_unit_t)unit, (adc_channel_t)channel, (adc_atten_t)atten);

    } else {
        return luaL_error(L, "不支持的设备类型: %s", gpio_type_str);
    }

    return 0;
}

// 注册接口
void register_reg_device(lua_State* L)
{
    lua_pushcfunction(L, l_init_device);
    lua_setglobal(L, "c_device_initializer");
    printf("[C] ✅ 注册 c_device_initializer 成功\n");
}
