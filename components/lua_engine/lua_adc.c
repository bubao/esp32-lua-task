// lua_adc.c
#include "lua_adc.h"
#include "driver/adc.h"
#include "esp_log.h"
#include <lauxlib.h>

static const char* TAG = "lua_adc";

// 默认 ADC 衰减，支持用户自行定义
#ifndef DEFAULT_ADC_ATTEN
#define DEFAULT_ADC_ATTEN ADC_ATTEN_DB_0
#endif

// GPIO 到 ADC1 通道映射结构体
typedef struct {
    int gpio;
    adc1_channel_t channel;
} gpio_adc_map_t;

// 不同芯片的 GPIO → ADC1通道映射
#if CONFIG_IDF_TARGET_ESP32
static const gpio_adc_map_t gpio_adc_map[] = {
    { 36, ADC1_CHANNEL_0 },
    { 37, ADC1_CHANNEL_1 },
    { 38, ADC1_CHANNEL_2 },
    { 39, ADC1_CHANNEL_3 },
    { 32, ADC1_CHANNEL_4 },
    { 33, ADC1_CHANNEL_5 },
    { 34, ADC1_CHANNEL_6 },
    { 35, ADC1_CHANNEL_7 },
};
#elif CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32S2
static const gpio_adc_map_t gpio_adc_map[] = {
    { 1, ADC1_CHANNEL_0 },
    { 2, ADC1_CHANNEL_1 },
    { 3, ADC1_CHANNEL_2 },
    { 4, ADC1_CHANNEL_3 },
    { 5, ADC1_CHANNEL_4 },
    { 6, ADC1_CHANNEL_5 },
    { 7, ADC1_CHANNEL_6 },
    { 8, ADC1_CHANNEL_7 },
    { 9, ADC1_CHANNEL_8 },
    { 10, ADC1_CHANNEL_9 },
};
#elif CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C2
static const gpio_adc_map_t gpio_adc_map[] = {
    { 0, ADC1_CHANNEL_0 },
    { 1, ADC1_CHANNEL_1 },
    { 2, ADC1_CHANNEL_2 },
    { 3, ADC1_CHANNEL_3 },
    { 4, ADC1_CHANNEL_4 },
};
#elif CONFIG_IDF_TARGET_ESP32C6 || CONFIG_IDF_TARGET_ESP32H2 || CONFIG_IDF_TARGET_ESP32C5 || CONFIG_IDF_TARGET_ESP32C61
static const gpio_adc_map_t gpio_adc_map[] = {
    { 0, ADC1_CHANNEL_0 },
    { 1, ADC1_CHANNEL_1 },
    { 2, ADC1_CHANNEL_2 },
    { 3, ADC1_CHANNEL_3 },
    { 4, ADC1_CHANNEL_4 },
    { 5, ADC1_CHANNEL_5 },
    { 6, ADC1_CHANNEL_6 },
};
#elif CONFIG_IDF_TARGET_ESP32P4
static const gpio_adc_map_t gpio_adc_map[] = {
    { 16, ADC1_CHANNEL_0 },
    { 17, ADC1_CHANNEL_1 },
    { 18, ADC1_CHANNEL_2 },
    { 19, ADC1_CHANNEL_3 },
    { 20, ADC1_CHANNEL_4 },
    { 21, ADC1_CHANNEL_5 },
    { 22, ADC1_CHANNEL_6 },
    { 23, ADC1_CHANNEL_7 },
};
#else
#error "Unsupported ESP32 chip for ADC1 mapping"
#endif

// 静态变量保存映射长度，避免多次计算
static const size_t gpio_adc_map_len = sizeof(gpio_adc_map) / sizeof(gpio_adc_map[0]);

/**
 * @brief 根据 GPIO 查找 ADC1 通道
 *
 * @param gpio GPIO 编号
 * @param out_channel 输出 ADC1 通道
 * @return true 找到对应通道
 * @return false 未找到
 */
static bool get_adc1_channel_from_gpio(int gpio, adc1_channel_t* out_channel)
{
    for (size_t i = 0; i < gpio_adc_map_len; i++) {
        if (gpio_adc_map[i].gpio == gpio) {
            *out_channel = gpio_adc_map[i].channel;
            return true;
        }
    }
    return false;
}

/**
 * @brief Lua 绑定函数：读取指定 GPIO 对应 ADC1 通道的原始 ADC 值
 *
 * Lua调用示例: read_adc(33)
 *
 * @param L Lua状态机指针
 * @return int 返回参数个数（1个整数值）
 */
static int l_read_adc(lua_State* L)
{
    int gpio = luaL_checkinteger(L, 1);
    adc1_channel_t channel;

    if (!get_adc1_channel_from_gpio(gpio, &channel)) {
        return luaL_error(L, "Invalid or unsupported GPIO: %d", gpio);
    }

    int raw = adc1_get_raw(channel);
    lua_pushinteger(L, raw);
    return 1;
}

/**
 * @brief 初始化 ADC1，配置所有支持的通道，注册 Lua 函数
 *
 * @param L Lua状态机指针
 */
void register_lua_adc(lua_State* L)
{
    // 配置 ADC1 分辨率
    adc1_config_width(ADC_WIDTH_BIT_DEFAULT);

    // 配置所有通道默认衰减
    for (size_t i = 0; i < gpio_adc_map_len; i++) {
        adc1_config_channel_atten(gpio_adc_map[i].channel, DEFAULT_ADC_ATTEN);
        ESP_LOGI(TAG, "ADC1 Channel %d mapped to GPIO%d, attenuation %d dB",
            gpio_adc_map[i].channel, gpio_adc_map[i].gpio, DEFAULT_ADC_ATTEN);
    }

    // 注册 Lua 函数
    lua_register(L, "read_adc", l_read_adc);
}
