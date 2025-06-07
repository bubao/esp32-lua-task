#include "lua_gpio.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <lauxlib.h>
#include <string.h>

static const char* TAG = "lua_gpio";

// 检查并获取表中的整数字段，支持默认值
static int get_table_int_field(lua_State* L, int table_idx, const char* field, int default_val)
{
    lua_getfield(L, table_idx, field);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return default_val;
    }
    int value = luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    return value;
}

// 检查并获取表中的布尔字段，支持默认值
static bool get_table_bool_field(lua_State* L, int table_idx, const char* field, bool default_val)
{
    lua_getfield(L, table_idx, field);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return default_val;
    }
    bool value = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return value;
}

/**
 * @brief 设置GPIO模式和配置
 *
 * Lua参数:
 * {
 *   pin = <gpio_num>,
 *   mode = <gpio_mode>,
 *   pull_up = <true/false>,
 *   pull_down = <true/false>,
 *   intr = <gpio_intr_type> (可选)
 * }
 *
 * @return 0 成功, 错误时返回错误信息
 */
static int l_gpio_set_mode(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    gpio_config_t io_conf = { 0 };

    // 读取 pin
    gpio_num_t gpio_num = (gpio_num_t)get_table_int_field(L, 1, "pin", 0);
    if (gpio_num < 0 || gpio_num >= GPIO_NUM_MAX) {
        return luaL_error(L, "Invalid GPIO number: %d", gpio_num);
    }
    io_conf.pin_bit_mask = 1ULL << gpio_num;

    // 读取 mode
    gpio_mode_t mode = (gpio_mode_t)get_table_int_field(L, 1, "mode", GPIO_MODE_DISABLE);
    // 检查mode有效性，使用实际存在的模式值
    if (mode != GPIO_MODE_DISABLE && mode != GPIO_MODE_INPUT && mode != GPIO_MODE_OUTPUT && mode != GPIO_MODE_INPUT_OUTPUT && mode != GPIO_MODE_OUTPUT_OD) {
        return luaL_error(L, "Invalid GPIO mode: %d", mode);
    }
    io_conf.mode = mode;

    // pull_up (bool)
    io_conf.pull_up_en = get_table_bool_field(L, 1, "pull_up", false) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;

    // pull_down (bool)
    io_conf.pull_down_en = get_table_bool_field(L, 1, "pull_down", false) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;

    // 中断类型 (optional)
    gpio_int_type_t intr_type = (gpio_int_type_t)get_table_int_field(L, 1, "intr", GPIO_INTR_DISABLE);
    // 检查intr_type有效性
    if (intr_type < 0 || intr_type > GPIO_INTR_HIGH_LEVEL) {
        return luaL_error(L, "Invalid GPIO interrupt type: %d", intr_type);
    }
    io_conf.intr_type = intr_type;

    // 配置 GPIO
    ESP_LOGD(TAG, "Configuring GPIO%d: mode=%d, pull_up=%d, pull_down=%d, intr=%d",
        gpio_num, mode, io_conf.pull_up_en, io_conf.pull_down_en, intr_type);

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed for GPIO%d: %s", gpio_num, esp_err_to_name(err));
        return luaL_error(L, "Failed to configure GPIO%d: %s", gpio_num, esp_err_to_name(err));
    }

    return 0;
}

/**
 * @brief 设置GPIO输出电平
 *
 * @param gpio_num GPIO编号
 * @param level 电平值 (0或1)
 * @return 0 成功, 错误时返回错误信息
 */
static int l_gpio_set_level(lua_State* L)
{
    gpio_num_t gpio_num = (gpio_num_t)luaL_checkinteger(L, 1);
    int level = luaL_checkinteger(L, 2);

    if (gpio_num < 0 || gpio_num >= GPIO_NUM_MAX) {
        return luaL_error(L, "Invalid GPIO number: %d", gpio_num);
    }

    if (level != 0 && level != 1) {
        return luaL_error(L, "Invalid GPIO level: %d (must be 0 or 1)", level);
    }

    esp_err_t err = gpio_set_level(gpio_num, level);
    if (err != ESP_OK) {
        return luaL_error(L, "Failed to set GPIO%d level: %s", gpio_num, esp_err_to_name(err));
    }

    return 0;
}

/**
 * @brief 获取GPIO输入电平
 *
 * @param gpio_num GPIO编号
 * @return 1个返回值: GPIO电平 (0或1)
 */
static int l_gpio_get_level(lua_State* L)
{
    gpio_num_t gpio_num = (gpio_num_t)luaL_checkinteger(L, 1);

    if (gpio_num < 0 || gpio_num >= GPIO_NUM_MAX) {
        return luaL_error(L, "Invalid GPIO number: %d", gpio_num);
    }

    int level = gpio_get_level(gpio_num);
    lua_pushinteger(L, level);
    return 1;
}

/**
 * @brief 注册GPIO中断处理函数
 *
 * Lua参数:
 * {
 *   pin = <gpio_num>,
 *   callback = <function>
 * }
 *
 * @return 0 成功, 错误时返回错误信息
 */
static int l_gpio_set_interrupt(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    // 获取GPIO编号
    gpio_num_t gpio_num = (gpio_num_t)get_table_int_field(L, 1, "pin", 0);
    if (gpio_num < 0 || gpio_num >= GPIO_NUM_MAX) {
        return luaL_error(L, "Invalid GPIO number: %d", gpio_num);
    }

    // 获取回调函数
    lua_getfield(L, 1, "callback");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return luaL_error(L, "Interrupt callback must be a function");
    }

    // TODO: 实现中断回调注册逻辑
    // 注意：ESP-IDF的中断处理需要静态存储回调信息
    // 这里仅作为示例，实际实现需要考虑内存管理和线程安全

    ESP_LOGW(TAG, "GPIO interrupt handling not fully implemented yet");
    lua_pop(L, 1); // 弹出回调函数

    return 0;
}

// GPIO函数列表
static const luaL_Reg gpio_funcs[] = {
    { "set_mode", l_gpio_set_mode },
    { "set_level", l_gpio_set_level },
    { "get_level", l_gpio_get_level },
    { "set_interrupt", l_gpio_set_interrupt },
    { NULL, NULL }
};

// 注册GPIO模块
int luaopen_gpio(lua_State* L)
{
    luaL_newlib(L, gpio_funcs);

    // 注册常量
    // 模式
    lua_pushinteger(L, GPIO_MODE_DISABLE);
    lua_setfield(L, -2, "MODE_DISABLE");
    lua_pushinteger(L, GPIO_MODE_INPUT);
    lua_setfield(L, -2, "MODE_INPUT");
    lua_pushinteger(L, GPIO_MODE_OUTPUT);
    lua_setfield(L, -2, "MODE_OUTPUT");
    lua_pushinteger(L, GPIO_MODE_INPUT_OUTPUT);
    lua_setfield(L, -2, "MODE_INPUT_OUTPUT");
    lua_pushinteger(L, GPIO_MODE_OUTPUT_OD);
    lua_setfield(L, -2, "MODE_OUTPUT_OD");

    // 中断类型
    lua_pushinteger(L, GPIO_INTR_DISABLE);
    lua_setfield(L, -2, "INTR_DISABLE");
    lua_pushinteger(L, GPIO_INTR_POSEDGE);
    lua_setfield(L, -2, "INTR_POSEDGE");
    lua_pushinteger(L, GPIO_INTR_NEGEDGE);
    lua_setfield(L, -2, "INTR_NEGEDGE");
    lua_pushinteger(L, GPIO_INTR_ANYEDGE);
    lua_setfield(L, -2, "INTR_ANYEDGE");
    lua_pushinteger(L, GPIO_INTR_LOW_LEVEL);
    lua_setfield(L, -2, "INTR_LOW_LEVEL");
    lua_pushinteger(L, GPIO_INTR_HIGH_LEVEL);
    lua_setfield(L, -2, "INTR_HIGH_LEVEL");

    // 上拉/下拉
    lua_pushinteger(L, GPIO_PULLUP_DISABLE);
    lua_setfield(L, -2, "PULLUP_DISABLE");
    lua_pushinteger(L, GPIO_PULLUP_ENABLE);
    lua_setfield(L, -2, "PULLUP_ENABLE");

    lua_pushinteger(L, GPIO_PULLDOWN_DISABLE);
    lua_setfield(L, -2, "PULLDOWN_DISABLE");
    lua_pushinteger(L, GPIO_PULLDOWN_ENABLE);
    lua_setfield(L, -2, "PULLDOWN_ENABLE");

    return 1;
}

// 注册GPIO模块（旧方式，保持兼容性）
void register_lua_gpio(lua_State* L)
{
    // 注册为模块（推荐方式）
    luaL_requiref(L, "gpio", luaopen_gpio, 1);
    lua_pop(L, 1); // 弹出模块表

    // 同时注册为全局变量（保持向后兼容性）
    lua_getglobal(L, "gpio");
    lua_setglobal(L, "gpio");

    // 初始化GPIO驱动
    gpio_install_isr_service(0);
    ESP_LOGI(TAG, "GPIO module registered successfully");
}