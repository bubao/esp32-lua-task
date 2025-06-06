// lua_adc.c
#include "lua_adc.h"
#include "driver/adc.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include <inttypes.h> // 添加这个头文件以支持 PRIu32
#include <lauxlib.h>
#include <string.h>

static const char* TAG = "lua_adc";

// 默认配置
#ifndef DEFAULT_ADC_ATTEN
#define DEFAULT_ADC_ATTEN ADC_ATTEN_DB_0
#endif

#ifndef DEFAULT_ADC_WIDTH
#define DEFAULT_ADC_WIDTH ADC_WIDTH_BIT_DEFAULT
#endif

// GPIO 到 ADC1 通道映射结构体
typedef struct {
    int gpio;
    adc1_channel_t channel;
    bool initialized;
    adc_atten_t attenuation;
} gpio_adc_map_t;

// 不同芯片的 GPIO → ADC1通道映射
#if CONFIG_IDF_TARGET_ESP32
static const gpio_adc_map_t gpio_adc_map[] = {
    { 36, ADC1_CHANNEL_0, false, DEFAULT_ADC_ATTEN },
    { 37, ADC1_CHANNEL_1, false, DEFAULT_ADC_ATTEN },
    { 38, ADC1_CHANNEL_2, false, DEFAULT_ADC_ATTEN },
    { 39, ADC1_CHANNEL_3, false, DEFAULT_ADC_ATTEN },
    { 32, ADC1_CHANNEL_4, false, DEFAULT_ADC_ATTEN },
    { 33, ADC1_CHANNEL_5, false, DEFAULT_ADC_ATTEN },
    { 34, ADC1_CHANNEL_6, false, DEFAULT_ADC_ATTEN },
    { 35, ADC1_CHANNEL_7, false, DEFAULT_ADC_ATTEN },
};
#elif CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32S2
static const gpio_adc_map_t gpio_adc_map[] = {
    { 1, ADC1_CHANNEL_0, false, DEFAULT_ADC_ATTEN },
    { 2, ADC1_CHANNEL_1, false, DEFAULT_ADC_ATTEN },
    { 3, ADC1_CHANNEL_2, false, DEFAULT_ADC_ATTEN },
    { 4, ADC1_CHANNEL_3, false, DEFAULT_ADC_ATTEN },
    { 5, ADC1_CHANNEL_4, false, DEFAULT_ADC_ATTEN },
    { 6, ADC1_CHANNEL_5, false, DEFAULT_ADC_ATTEN },
    { 7, ADC1_CHANNEL_6, false, DEFAULT_ADC_ATTEN },
    { 8, ADC1_CHANNEL_7, false, DEFAULT_ADC_ATTEN },
    { 9, ADC1_CHANNEL_8, false, DEFAULT_ADC_ATTEN },
    { 10, ADC1_CHANNEL_9, false, DEFAULT_ADC_ATTEN },
};
#elif CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C2
static const gpio_adc_map_t gpio_adc_map[] = {
    { 0, ADC1_CHANNEL_0, false, DEFAULT_ADC_ATTEN },
    { 1, ADC1_CHANNEL_1, false, DEFAULT_ADC_ATTEN },
    { 2, ADC1_CHANNEL_2, false, DEFAULT_ADC_ATTEN },
    { 3, ADC1_CHANNEL_3, false, DEFAULT_ADC_ATTEN },
    { 4, ADC1_CHANNEL_4, false, DEFAULT_ADC_ATTEN },
};
#elif CONFIG_IDF_TARGET_ESP32C6 || CONFIG_IDF_TARGET_ESP32H2 || CONFIG_IDF_TARGET_ESP32C5 || CONFIG_IDF_TARGET_ESP32C61
static const gpio_adc_map_t gpio_adc_map[] = {
    { 0, ADC1_CHANNEL_0, false, DEFAULT_ADC_ATTEN },
    { 1, ADC1_CHANNEL_1, false, DEFAULT_ADC_ATTEN },
    { 2, ADC1_CHANNEL_2, false, DEFAULT_ADC_ATTEN },
    { 3, ADC1_CHANNEL_3, false, DEFAULT_ADC_ATTEN },
    { 4, ADC1_CHANNEL_4, false, DEFAULT_ADC_ATTEN },
    { 5, ADC1_CHANNEL_5, false, DEFAULT_ADC_ATTEN },
    { 6, ADC1_CHANNEL_6, false, DEFAULT_ADC_ATTEN },
};
#elif CONFIG_IDF_TARGET_ESP32P4
static const gpio_adc_map_t gpio_adc_map[] = {
    { 16, ADC1_CHANNEL_0, false, DEFAULT_ADC_ATTEN },
    { 17, ADC1_CHANNEL_1, false, DEFAULT_ADC_ATTEN },
    { 18, ADC1_CHANNEL_2, false, DEFAULT_ADC_ATTEN },
    { 19, ADC1_CHANNEL_3, false, DEFAULT_ADC_ATTEN },
    { 20, ADC1_CHANNEL_4, false, DEFAULT_ADC_ATTEN },
    { 21, ADC1_CHANNEL_5, false, DEFAULT_ADC_ATTEN },
    { 22, ADC1_CHANNEL_6, false, DEFAULT_ADC_ATTEN },
    { 23, ADC1_CHANNEL_7, false, DEFAULT_ADC_ATTEN },
};
#else
#error "Unsupported ESP32 chip for ADC1 mapping"
#endif

// 静态变量保存映射长度，避免多次计算
static const size_t gpio_adc_map_len = sizeof(gpio_adc_map) / sizeof(gpio_adc_map[0]);

// 查找通道的内部函数，返回映射表索引或 -1
static int find_adc1_channel_index(int gpio)
{
    for (size_t i = 0; i < gpio_adc_map_len; i++) {
        if (gpio_adc_map[i].gpio == gpio) {
            return (int)i;
        }
    }
    return -1;
}

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
    int index = find_adc1_channel_index(gpio);
    if (index >= 0) {
        *out_channel = gpio_adc_map[index].channel;
        return true;
    }
    return false;
}

/**
 * @brief 初始化指定 GPIO 对应的 ADC1 通道
 *
 * @param gpio GPIO 编号
 * @return esp_err_t 操作结果
 */
static esp_err_t init_adc_channel(int gpio)
{
    int index = find_adc1_channel_index(gpio);
    if (index < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // 检查是否已初始化
    if (gpio_adc_map[index].initialized) {
        return ESP_OK;
    }

    adc1_channel_t channel = gpio_adc_map[index].channel;
    adc_atten_t atten = gpio_adc_map[index].attenuation;

    // 配置通道衰减
    esp_err_t err = adc1_config_channel_atten(channel, atten);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure ADC1 channel %d (GPIO%d): %s",
            channel, gpio, esp_err_to_name(err));
        return err;
    }

    // 标记为已初始化 - 使用C风格类型转换
    ((gpio_adc_map_t*)&gpio_adc_map[index])->initialized = true;
    ESP_LOGI(TAG, "ADC1 Channel %d (GPIO%d) initialized with %d dB attenuation",
        channel, gpio, atten);

    return ESP_OK;
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
        return luaL_error(L, "Invalid or unsupported GPIO for ADC1: %d", gpio);
    }

    // 确保通道已初始化
    esp_err_t err = init_adc_channel(gpio);
    if (err != ESP_OK) {
        return luaL_error(L, "Failed to initialize ADC1 channel for GPIO%d: %s",
            gpio, esp_err_to_name(err));
    }

    int raw = adc1_get_raw(channel);
    if (raw == -1) {
        return luaL_error(L, "Failed to read ADC1 channel %d (GPIO%d)", channel, gpio);
    }

    lua_pushinteger(L, raw);
    return 1;
}

/**
 * @brief Lua 绑定函数：配置指定 GPIO 对应 ADC1 通道的衰减
 *
 * Lua调用示例: set_adc_atten(33, 2) -- 设置GPIO33的衰减为2dB
 *
 * @param L Lua状态机指针
 * @return int 返回参数个数
 */
static int l_set_adc_atten(lua_State* L)
{
    int gpio = luaL_checkinteger(L, 1);
    int atten_db = luaL_checkinteger(L, 2);

    adc_atten_t atten;
    switch (atten_db) {
    case 0:
        atten = ADC_ATTEN_DB_0;
        break;
    case 2:
        atten = ADC_ATTEN_DB_2_5;
        break;
    case 6:
        atten = ADC_ATTEN_DB_6;
        break;
    case 11:
        atten = ADC_ATTEN_DB_12;
        break; // 替换为非弃用版本
    default:
        return luaL_error(L, "Invalid ADC attenuation: %d dB. Supported values: 0, 2, 6, 11", atten_db);
    }

    int index = find_adc1_channel_index(gpio);
    if (index < 0) {
        return luaL_error(L, "Invalid or unsupported GPIO for ADC1: %d", gpio);
    }

    // 更新衰减配置 - 使用C风格类型转换
    ((gpio_adc_map_t*)&gpio_adc_map[index])->attenuation = atten;

    // 如果已初始化，则重新配置
    if (gpio_adc_map[index].initialized) {
        esp_err_t err = adc1_config_channel_atten(gpio_adc_map[index].channel, atten);
        if (err != ESP_OK) {
            return luaL_error(L, "Failed to set ADC1 attenuation for GPIO%d: %s",
                gpio, esp_err_to_name(err));
        }
        ESP_LOGI(TAG, "ADC1 Channel %d (GPIO%d) attenuation set to %d dB",
            gpio_adc_map[index].channel, gpio, atten_db);
    }

    lua_pushboolean(L, 1); // 返回成功
    return 1;
}

/**
 * @brief Lua 绑定函数：获取 ADC 原始值对应的电压 (mV)
 *
 * Lua调用示例: adc_to_voltage(2048)
 *
 * @param L Lua状态机指针
 * @return int 返回参数个数（1个整数值）
 */
static int l_adc_to_voltage(lua_State* L)
{
    int adc_value = luaL_checkinteger(L, 1);

    // 使用新的ADC校准API
    static adc_cali_handle_t adc1_cali_handle = NULL;
    static bool adc_cali_init = false;

    if (!adc_cali_init) {
        // 检查是否支持硬件校准
        bool cali_supported = false;
        adc_cali_handle_t handle = NULL;

        // 新的校准初始化方法
        adc_cali_line_fitting_config_t cali_config = {
            .unit_id = ADC_UNIT_1,
            .atten = ADC_ATTEN_DB_0,
            .bitwidth = DEFAULT_ADC_WIDTH,
        };

        // 检查是否支持线性拟合校准
        adc_cali_scheme_ver_t scheme_mask = ADC_CALI_SCHEME_VER_LINE_FITTING;
        esp_err_t err = adc_cali_check_scheme(&scheme_mask);
        cali_supported = (err == ESP_OK) && (scheme_mask & ADC_CALI_SCHEME_VER_LINE_FITTING);

        if (cali_supported) {
            err = adc_cali_create_scheme_line_fitting(&cali_config, &handle);
            if (err == ESP_OK) {
                adc1_cali_handle = handle;
                ESP_LOGI(TAG, "ADC calibration initialized (linear fitting)");
            } else {
                ESP_LOGW(TAG, "Failed to initialize ADC calibration: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGW(TAG, "ADC calibration not supported on this chip");
        }

        adc_cali_init = true;
    }

    int voltage = 0;
    if (adc1_cali_handle) {
        // 使用校准转换
        adc_cali_raw_to_voltage(adc1_cali_handle, adc_value, &voltage);
    } else {
        // 如果没有校准，使用简单的线性映射
        uint32_t max_raw = (1U << (uint32_t)DEFAULT_ADC_WIDTH) - 1;
        voltage = (adc_value * 3300) / max_raw;
        ESP_LOGW(TAG, "ADC calibration not available, using linear approximation");
    }

    lua_pushinteger(L, voltage);
    return 1;
}

/**
 * @brief Lua 绑定函数：检查 GPIO 是否支持 ADC1
 *
 * Lua调用示例: is_adc_gpio(33)
 *
 * @param L Lua状态机指针
 * @return int 返回参数个数（1个布尔值）
 */
static int l_is_adc_gpio(lua_State* L)
{
    int gpio = luaL_checkinteger(L, 1);
    lua_pushboolean(L, find_adc1_channel_index(gpio) >= 0);
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
    esp_err_t err = adc1_config_width(DEFAULT_ADC_WIDTH);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure ADC1 width: %s", esp_err_to_name(err));
        return;
    }

    // 使用新的API获取位宽信息
    uint32_t adc_bitwidth = (uint32_t)DEFAULT_ADC_WIDTH;
    ESP_LOGI(TAG, "ADC1 initialized with %" PRIu32 "-bit resolution", adc_bitwidth);

    // 注册 Lua 函数
    const luaL_Reg adc_funcs[] = {
        { "read_adc", l_read_adc },
        { "set_adc_atten", l_set_adc_atten },
        { "adc_to_voltage", l_adc_to_voltage },
        { "is_adc_gpio", l_is_adc_gpio },
        { NULL, NULL }
    };

    luaL_newlib(L, adc_funcs);
    lua_setglobal(L, "adc");

    ESP_LOGI(TAG, "Lua ADC module registered successfully");
}

/**
 * @brief 清理 ADC 资源
 */
void unregister_lua_adc(void)
{
    // 重置所有通道
    for (size_t i = 0; i < gpio_adc_map_len; i++) {
        if (gpio_adc_map[i].initialized) {
            // 注意：ESP-IDF 没有提供直接重置 ADC 通道的函数
            // 这里仅标记为未初始化
            ((gpio_adc_map_t*)&gpio_adc_map[i])->initialized = false;
            ESP_LOGI(TAG, "ADC1 Channel %d (GPIO%d) uninitialized",
                gpio_adc_map[i].channel, gpio_adc_map[i].gpio);
        }
    }

    // 释放校准句柄
    static bool adc_cali_init = false;
    static adc_cali_handle_t adc1_cali_handle = NULL;
    if (adc_cali_init && adc1_cali_handle) {
        adc_cali_delete_scheme_line_fitting(adc1_cali_handle);
        adc1_cali_handle = NULL;
        ESP_LOGI(TAG, "ADC calibration resources released");
    }

    ESP_LOGI(TAG, "ADC resources cleaned up");
}