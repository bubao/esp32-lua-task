// lua_pwm.c
#include "lua_pwm.h"
#include "driver/ledc.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "hal/ledc_types.h"
#include "lauxlib.h"
#include "lua.h"
#include <stdbool.h>
#include <stdio.h>

static const char* TAG = "lua_pwm";

// 检查通道有效性
static bool is_valid_channel(ledc_channel_t channel)
{
    return channel >= 0 && channel < LEDC_CHANNEL_MAX;
}

// 检查定时器有效性
static bool is_valid_timer(int timer)
{
    return timer >= 0 && timer < LEDC_TIMER_MAX;
}

/**
 * @brief 初始化PWM控制器
 * @return ESP_OK表示成功
 */
esp_err_t pwm_init(int gpio_num, ledc_mode_t speed_mode, ledc_channel_t channel)
{
    ledc_set_pin(gpio_num, speed_mode, channel);
    return ESP_OK;
}

/**
 * @brief 设置PWM通道参数
 * @param config PWM配置结构体
 * @return ESP_OK表示成功
 */
esp_err_t pwm_setup(pwm_config_t* config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!is_valid_channel(config->channel) || !is_valid_timer(config->timer)) {
        ESP_LOGE(TAG, "Invalid channel or timer: channel=%d, timer=%d",
            config->channel, config->timer);
        return ESP_ERR_INVALID_ARG;
    }

    // 1. 配置定时器
    ledc_timer_config_t timer_conf = {
        .speed_mode = config->channel,
        .duty_resolution = config->resolution > 0 ? config->resolution : LEDC_TIMER_10_BIT,
        .timer_num = (ledc_timer_t)config->timer,
        .freq_hz = config->freq > 0 ? config->freq : 5000,
        .clk_cfg = LEDC_USE_RC_FAST_CLK,
        .deconfigure = false,
    };

    esp_err_t err = ledc_timer_config(&timer_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure timer: %d", err);
        return err;
    }

    // 2. 配置通道
    ledc_channel_config_t channel_conf = {
        .gpio_num = config->pin,
        .speed_mode = config->channel,
        .channel = (ledc_channel_t)config->channel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = (ledc_timer_t)config->timer,
        .duty = config->duty,
        .hpoint = 0,
    };

    channel_conf.sleep_mode = LEDC_SLEEP_MODE_KEEP_ALIVE;

    err = ledc_channel_config(&channel_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure channel: %d", err);
        return err;
    }

    // 3. 绑定GPIO
    err = ledc_set_pin(config->pin, config->channel, (ledc_channel_t)config->channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set pin: %d", err);
        return err;
    }

    ESP_LOGI(TAG, "PWM channel configured: pin=%d, channel=%d, timer=%d, freq=%dHz, duty=%d",
        config->pin, config->channel, config->timer, config->freq, config->duty);
    return ESP_OK;
}

/**
 * @brief 设置PWM占空比
 * @param channel PWM通道
 * @param duty 占空比(0-最大值，取决于分辨率)
 * @return ESP_OK表示成功
 */
esp_err_t pwm_set_duty(ledc_mode_t speed_mode, ledc_channel_t channel, uint32_t duty)
{
    if (!is_valid_channel(channel)) {
        return ESP_ERR_INVALID_ARG;
    }

    // 根据当前分辨率动态计算最大值
    int max_duty = (1 << 10) - 1; // 默认10位分辨率

    if (duty < 0 || duty > max_duty) {
        ESP_LOGE(TAG, "Invalid duty cycle: %" PRIu32 " (should be 0-%d for current resolution)", duty, max_duty);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ledc_set_duty(speed_mode, channel, duty);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set duty cycle: %d", err);
        return err;
    }

    err = ledc_update_duty(speed_mode, channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update duty cycle: %d", err);
        return err;
    }

    ESP_LOGI(TAG, "PWM duty cycle set: channel=%d, duty=%" PRIu32 "", channel, duty);
    return ESP_OK;
}

/**
 * @brief 启动PWM通道
 * @param channel PWM通道
 * @return ESP_OK表示成功
 */
esp_err_t pwm_start(ledc_mode_t speed_mode, ledc_channel_t channel, ledc_fade_mode_t fade_mode)
{
    if (!is_valid_channel(channel)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ledc_fade_start(speed_mode, channel, fade_mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start channel: %d", err);
        return err;
    }

    ESP_LOGI(TAG, "PWM channel started: %d", channel);
    return ESP_OK;
}

/**
 * @brief 停止PWM通道
 * @param channel PWM通道
 * @return ESP_OK表示成功
 */
esp_err_t pwm_stop(ledc_mode_t speed_mode, ledc_channel_t channel)
{
    if (!is_valid_channel(channel)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ledc_stop(speed_mode, channel, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop channel: %d", err);
        return err;
    }

    ESP_LOGI(TAG, "PWM channel stopped: %d", channel);
    return ESP_OK;
}

/**
 * @brief 设置PWM频率
 * @param timer PWM定时器
 * @param freq 频率(Hz)
 * @return ESP_OK表示成功
 */
esp_err_t pwm_set_freq(ledc_mode_t speed_mode, ledc_timer_t timer, uint32_t freq)
{
    if (!is_valid_timer(timer)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ledc_set_freq(speed_mode, timer, freq);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set frequency: %d", err);
        return err;
    }

    uint32_t current_freq = ledc_get_freq(speed_mode, timer);
    ESP_LOGI(TAG, "PWM frequency set: timer=%d, freq=%" PRIu32 "Hz", timer, current_freq);
    return ESP_OK;
}

/**
 * @brief Lua绑定函数：初始化PWM
 */
static int l_pwm_init(lua_State* L)
{
    int gpio_num = luaL_checkinteger(L, 1);
    ledc_mode_t speed_mode = luaL_checkinteger(L, 2);
    ledc_channel_t channel = luaL_checkinteger(L, 3);
    esp_err_t err = ledc_set_pin(gpio_num, speed_mode, channel);
    lua_pushboolean(L, err == ESP_OK);
    return 1;
}

/**
 * @brief Lua绑定函数：设置PWM通道
 */
static int l_pwm_setup(lua_State* L)
{
    // 检查参数是否为table
    luaL_checktype(L, 1, LUA_TTABLE);

    // 获取table
    lua_pushvalue(L, 1);

    pwm_config_t config = { 0 };

    // 从table中获取各个字段的值
    lua_getfield(L, -1, "pin");
    config.pin = luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);

    lua_getfield(L, -1, "channel");
    config.channel = luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);

    lua_getfield(L, -1, "timer");
    config.timer = luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);

    lua_getfield(L, -1, "freq");
    config.freq = luaL_optinteger(L, -1, 5000);
    lua_pop(L, 1);

    lua_getfield(L, -1, "duty");
    config.duty = luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);

    lua_getfield(L, -1, "resolution");
    config.resolution = luaL_optinteger(L, -1, 10);
    lua_pop(L, 1);

    // 弹出table
    lua_pop(L, 1);

    // 检查分辨率是否在有效范围内
    if (config.resolution < LEDC_TIMER_1_BIT || config.resolution > LEDC_TIMER_BIT_MAX) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "Invalid resolution setting");
        return 2;
    }

    // 检查通道是否在有效范围内
    if (config.channel < 0 || config.channel >= LEDC_CHANNEL_MAX) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "Invalid channel setting");
        return 2;
    }

    // 检查定时器是否在有效范围内
    if (config.timer < 0 || config.timer >= LEDC_TIMER_MAX) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "Invalid timer setting");
        return 2;
    }

    esp_err_t err = pwm_setup(&config);
    lua_pushboolean(L, err == ESP_OK);
    if (err != ESP_OK) {
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    return 1;
}

/**
 * @brief Lua绑定函数：设置PWM占空比
 */
static int l_pwm_set_duty(lua_State* L)
{
    ledc_mode_t speed_mode = luaL_checkinteger(L, 1);
    ledc_channel_t channel = luaL_checkinteger(L, 2);
    uint32_t duty = luaL_checkinteger(L, 3);

    // 计算当前分辨率下的最大占空比值
    int max_duty = (1 << 10) - 1; // 默认10位分辨率

    if (duty < 0 || duty > max_duty) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "Invalid duty cycle");
        return 2;
    }

    esp_err_t err = pwm_set_duty(speed_mode, channel, duty);
    lua_pushboolean(L, err == ESP_OK);
    return 1;
}

/**
 * @brief Lua绑定函数：启动PWM
 */
static int l_pwm_start(lua_State* L)
{
    ledc_mode_t speed_mode = luaL_checkinteger(L, 1);
    ledc_channel_t channel = luaL_checkinteger(L, 2);
    ledc_fade_mode_t fade_mode = luaL_checkinteger(L, 3);

    esp_err_t err = ledc_fade_start(speed_mode, channel, fade_mode);
    lua_pushboolean(L, err == ESP_OK);
    return 1;
}

/**
 * @brief Lua绑定函数：停止PWM
 */
static int l_pwm_stop(lua_State* L)
{
    ledc_mode_t speed_mode = luaL_checkinteger(L, 1);
    ledc_channel_t channel = luaL_checkinteger(L, 2);

    esp_err_t err = pwm_stop(speed_mode, channel);
    lua_pushboolean(L, err == ESP_OK);
    return 1;
}

/**
 * @brief Lua绑定函数：设置PWM频率
 */
static int l_pwm_set_freq(lua_State* L)
{
    ledc_mode_t speed_mode = luaL_checkinteger(L, 1);
    ledc_timer_t timer = luaL_checkinteger(L, 2);
    uint32_t freq = luaL_checkinteger(L, 2);

    esp_err_t err = pwm_set_freq(speed_mode, timer, freq);
    lua_pushboolean(L, err == ESP_OK);
    return 1;
}

/**
 * @brief 注册PWM模块到Lua
 */
int luaopen_pwm(lua_State* L)
{
    luaL_Reg pwm_funcs[] = {
        { "init", l_pwm_init },
        { "setup", l_pwm_setup },
        { "set_duty", l_pwm_set_duty },
        { "start", l_pwm_start },
        { "stop", l_pwm_stop },
        { "set_freq", l_pwm_set_freq },
        { NULL, NULL }
    };

    luaL_newlib(L, pwm_funcs);
    return 1;
}

/**
 * @brief 注册PWM模块到Lua环境（兼容旧方式）
 */
void register_lua_pwm(lua_State* L)
{
    // 注册为模块（推荐方式）
    luaL_requiref(L, "pwm", luaopen_pwm, 1);
    lua_pop(L, 1); // 弹出模块表

    // 同时注册为全局变量（保持向后兼容性）
    lua_getglobal(L, "pwm");
    lua_setglobal(L, "pwm");

    ESP_LOGI(TAG, "PWM module registered successfully");
}