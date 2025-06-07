// lua_pwm.h
#ifndef LUA_PWM_H
#define LUA_PWM_H

#include "driver/ledc.h"
#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief PWM通道配置结构体
 */
typedef struct {
    int pin;            // GPIO引脚
    int channel;        // PWM通道
    int timer;          // PWM定时器
    int freq;           // 频率(Hz)
    int duty;           // 占空比(0-1023)
    int resolution;     // 占空比分辨率(位)
} pwm_config_t;

/**
 * @brief 初始化PWM控制器
 * @return ESP_OK表示成功
 */
esp_err_t pwm_init(int gpio_num, ledc_mode_t speed_mode, ledc_channel_t channel);

/**
 * @brief 设置PWM通道参数
 * @param config PWM配置结构体
 * @return ESP_OK表示成功
 */
esp_err_t pwm_setup(pwm_config_t* config);

/**
 * @brief 设置PWM占空比
 * @param channel PWM通道
 * @param duty 占空比(0-1023)
 * @return ESP_OK表示成功
 */
esp_err_t pwm_set_duty(ledc_mode_t speed_mode, ledc_channel_t channel, uint32_t duty);

/**
 * @brief 启动PWM通道
 * @param channel PWM通道
 * @return ESP_OK表示成功
 */
esp_err_t pwm_start(ledc_mode_t speed_mode, ledc_channel_t channel, ledc_fade_mode_t fade_mode);

/**
 * @brief 停止PWM通道
 * @param channel PWM通道
 * @return ESP_OK表示成功
 */
esp_err_t pwm_stop(ledc_mode_t speed_mode, ledc_channel_t channel);

/**
 * @brief 设置PWM频率
 * @param timer PWM定时器
 * @param freq 频率(Hz)
 * @return ESP_OK表示成功
 */
esp_err_t pwm_set_freq(ledc_mode_t speed_mode, ledc_timer_t timer, uint32_t freq);

void register_lua_pwm(lua_State* L);

#ifdef __cplusplus
}
#endif

#endif // LUA_PWM_H