#include "lua_reg.h"
#include "driver/adc.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "lauxlib.h"
#include "lua.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// 日志标签
static const char* TAG = "lua_reg";

// 将字符串解析为 GPIO 模式
static gpio_mode_t parse_gpio_mode(const char* mode_str)
{
    static const struct {
        const char* name;
        gpio_mode_t mode;
    } mode_map[] = {
        { "input", GPIO_MODE_INPUT },
        { "output", GPIO_MODE_OUTPUT },
        { "input_output", GPIO_MODE_INPUT_OUTPUT },
        { "inputoutput", GPIO_MODE_INPUT_OUTPUT },
        { "output_od", GPIO_MODE_OUTPUT_OD },
    };

    for (size_t i = 0; i < sizeof(mode_map) / sizeof(mode_map[0]); i++) {
        if (strcmp(mode_str, mode_map[i].name) == 0) {
            return mode_map[i].mode;
        }
    }

    return -1; // 无效模式
}

// 获取表中的整数字段，支持默认值
static int get_table_int_field(lua_State* L, int table_idx, const char* field, int default_val)
{
    lua_getfield(L, table_idx, field);
    int value = default_val;
    if (lua_isnumber(L, -1)) {
        value = lua_tointeger(L, -1);
    }
    lua_pop(L, 1);
    return value;
}

// 获取表中的字符串字段，支持默认值
static const char* get_table_string_field(lua_State* L, int table_idx, const char* field, const char* default_val)
{
    lua_getfield(L, table_idx, field);
    const char* value = default_val;
    if (lua_isstring(L, -1)) {
        value = lua_tostring(L, -1);
    }
    lua_pop(L, 1);
    return value;
}

// 获取表中的布尔字段，支持默认值
static bool get_table_bool_field(lua_State* L, int table_idx, const char* field, bool default_val)
{
    lua_getfield(L, table_idx, field);
    bool value = default_val;
    if (lua_isboolean(L, -1)) {
        value = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);
    return value;
}

// GPIO 初始化封装
static esp_err_t gpio_init_ex(int gpio_num, gpio_mode_t mode, bool pullup, bool pulldown)
{
    if (gpio_num < 0 || gpio_num >= GPIO_NUM_MAX) {
        ESP_LOGE(TAG, "Invalid GPIO number: %d", gpio_num);
        return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << gpio_num,
        .mode = mode,
        .pull_up_en = pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = pulldown ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GPIO%d: %s", gpio_num, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "GPIO initialized: GPIO=%d mode=%d pullup=%d pulldown=%d",
            gpio_num, mode, pullup, pulldown);
    }

    return err;
}

// PWM 初始化封装
static esp_err_t pwm_init_ex(int gpio_num, int channel, int freq_hz, int duty)
{
    if (gpio_num < 0 || gpio_num >= GPIO_NUM_MAX) {
        ESP_LOGE(TAG, "Invalid GPIO number for PWM: %d", gpio_num);
        return ESP_ERR_INVALID_ARG;
    }

    if (channel < 0 || channel >= LEDC_CHANNEL_MAX) {
        ESP_LOGE(TAG, "Invalid LEDC channel: %d", channel);
        return ESP_ERR_INVALID_ARG;
    }

    // 配置定时器
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE, // 使用低速模式
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .freq_hz = freq_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };

    esp_err_t err = ledc_timer_config(&ledc_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure PWM timer: %s", esp_err_to_name(err));
        return err;
    }

    // 配置通道
    ledc_channel_config_t ledc_channel = {
        .gpio_num = gpio_num,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = (ledc_channel_t)channel,
        .timer_sel = LEDC_TIMER_0,
        .duty = duty,
        .hpoint = 0,
    };

    err = ledc_channel_config(&ledc_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure PWM channel %d on GPIO%d: %s",
            channel, gpio_num, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "PWM initialized: GPIO=%d channel=%d freq=%dHz duty=%d",
            gpio_num, channel, freq_hz, duty);
    }

    return err;
}

// ADC 初始化封装
static esp_err_t adc_init_ex(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten)
{
    if (unit != ADC_UNIT_1 && unit != ADC_UNIT_2) {
        ESP_LOGE(TAG, "Invalid ADC unit: %d", unit);
        return ESP_ERR_INVALID_ARG;
    }

    // 使用类型转换避免枚举比较警告
    if (unit == ADC_UNIT_1 && (channel < 0 || (int)channel > (int)ADC1_CHANNEL_MAX)) {
        ESP_LOGE(TAG, "Invalid ADC1 channel: %d", channel);
        return ESP_ERR_INVALID_ARG;
    }

    if (unit == ADC_UNIT_2 && (channel < 0 || (int)channel > (int)ADC2_CHANNEL_MAX)) {
        ESP_LOGE(TAG, "Invalid ADC2 channel: %d", channel);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;

    if (unit == ADC_UNIT_1) {
        // 使用SOC_ADC_RTC_MAX_BITWIDTH配置ADC1位宽
        err = adc1_config_width(SOC_ADC_RTC_MAX_BITWIDTH);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure ADC1 width: %s", esp_err_to_name(err));
            return err;
        }

        // 配置ADC1通道
        err = adc1_config_channel_atten((adc1_channel_t)channel, atten);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure ADC1 channel %d: %s", channel, esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "ADC1 initialized: unit=%d channel=%d atten=%d", unit, channel, atten);
        }
    } else {
        // ADC2初始化需要特殊处理，因为WiFi也使用ADC2
        ESP_LOGW(TAG, "ADC2 initialization requires careful handling due to WiFi conflict");

        // 配置ADC2通道
        err = adc2_config_channel_atten((adc2_channel_t)channel, atten);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure ADC2 channel %d: %s", channel, esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "ADC2 initialized: unit=%d channel=%d atten=%d", unit, channel, atten);
        }
    }

    return err;
}

// Lua 设备初始化入口
static int l_init_device(lua_State* L)
{
    if (!lua_istable(L, 1)) {
        return luaL_error(L, "Parameter must be a table");
    }

    // 读取设备基本信息
    const char* id = get_table_string_field(L, 1, "id", "unknown");
    const char* name = get_table_string_field(L, 1, "name", "");
    const char* gpio_type = get_table_string_field(L, 1, "gpio_type", NULL);

    if (!gpio_type) {
        return luaL_error(L, "Missing 'gpio_type' field in device configuration");
    }

    ESP_LOGI(TAG, "Initializing device: ID=%s, Name=%s, Type=%s", id, name, gpio_type);

    esp_err_t err = ESP_OK;

    if (strcmp(gpio_type, "gpio") == 0) {
        // 初始化GPIO设备
        int gpio = get_table_int_field(L, 1, "gpio", -1);
        if (gpio < 0) {
            return luaL_error(L, "Missing or invalid 'gpio' field for GPIO device");
        }

        const char* mode_str = get_table_string_field(L, 1, "gpio_mode", "output");
        gpio_mode_t mode = parse_gpio_mode(mode_str);
        if (mode == -1) {
            return luaL_error(L, "Invalid 'gpio_mode' value: %s", mode_str);
        }

        bool pullup = get_table_bool_field(L, 1, "pullup", false);
        bool pulldown = get_table_bool_field(L, 1, "pulldown", false);

        err = gpio_init_ex(gpio, mode, pullup, pulldown);

    } else if (strcmp(gpio_type, "pwm") == 0) {
        // 初始化PWM设备
        int gpio = get_table_int_field(L, 1, "gpio", -1);
        if (gpio < 0) {
            return luaL_error(L, "Missing or invalid 'gpio' field for PWM device");
        }

        int channel = get_table_int_field(L, 1, "channel", -1);
        if (channel < 0) {
            return luaL_error(L, "Missing or invalid 'channel' field for PWM device");
        }

        int freq = get_table_int_field(L, 1, "freq", 5000); // 默认5kHz
        int duty = get_table_int_field(L, 1, "duty", 0); // 默认占空比0

        err = pwm_init_ex(gpio, channel, freq, duty);

    } else if (strcmp(gpio_type, "adc") == 0) {
        // 初始化ADC设备
        int unit = get_table_int_field(L, 1, "unit", -1);
        if (unit < 0) {
            return luaL_error(L, "Missing or invalid 'unit' field for ADC device");
        }

        int channel = get_table_int_field(L, 1, "channel", -1);
        if (channel < 0) {
            return luaL_error(L, "Missing or invalid 'channel' field for ADC device");
        }

        int atten = get_table_int_field(L, 1, "atten", ADC_ATTEN_DB_0);

        err = adc_init_ex((adc_unit_t)unit, (adc_channel_t)channel, (adc_atten_t)atten);

    } else {
        return luaL_error(L, "Unsupported device type: %s", gpio_type);
    }

    if (err != ESP_OK) {
        return luaL_error(L, "Failed to initialize device: %s", esp_err_to_name(err));
    }

    // 返回成功标志
    lua_pushboolean(L, 1);
    return 1;
}

// 注册接口
void register_reg_device(lua_State* L)
{
    lua_pushcfunction(L, l_init_device);
    lua_setglobal(L, "c_device_initializer");
    ESP_LOGI(TAG, "Device initializer registered successfully");
}