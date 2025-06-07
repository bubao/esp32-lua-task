#include "lua_gpio.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "soc/gpio_num.h"
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
 *   pin = <gpio_num>,          -- 必选，GPIO引脚号
 *   mode = <gpio_mode>,        -- 必选，GPIO模式（0-4）
 *   pull_up = <true/false>,    -- 可选，上拉使能（默认false）
 *   pull_down = <true/false>,  -- 可选，下拉使能（默认false）
 *   intr = <gpio_intr_type>    -- 可选，中断类型（默认GPIO_INTR_DISABLE）
 * }
 *
 * @return 成功返回true，失败返回false和错误信息
 */
static int l_gpio_set_mode(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    gpio_config_t io_conf = { 0 };

    // 读取 pin
    gpio_num_t gpio_num = (gpio_num_t)get_table_int_field(L, 1, "pin", 0);
    if (gpio_num < 0 || gpio_num >= GPIO_NUM_MAX) {
        lua_pushboolean(L, false);
        lua_pushfstring(L, "Invalid GPIO number: %d (valid range: 0-%d)", gpio_num, GPIO_NUM_MAX - 1);
        return 2;
    }
    io_conf.pin_bit_mask = 1ULL << gpio_num;

    // 读取 mode
    gpio_mode_t mode = (gpio_mode_t)get_table_int_field(L, 1, "mode", GPIO_MODE_DISABLE);
    // 检查mode有效性（使用枚举范围）
    if (mode < GPIO_MODE_DISABLE || mode > GPIO_MODE_OUTPUT_OD) {
        lua_pushboolean(L, false);
        lua_pushfstring(L, "Invalid GPIO mode: %d (valid range: 0-%d)", mode, GPIO_MODE_OUTPUT_OD);
        return 2;
    }
    io_conf.mode = mode;

    // pull_up (bool)
    io_conf.pull_up_en = get_table_bool_field(L, 1, "pull_up", false) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;

    // pull_down (bool)
    io_conf.pull_down_en = get_table_bool_field(L, 1, "pull_down", false) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;

    // 中断类型 (optional)
    gpio_int_type_t intr_type = (gpio_int_type_t)get_table_int_field(L, 1, "intr", GPIO_INTR_DISABLE);
    // 检查intr_type有效性（使用枚举范围）
    if (intr_type < GPIO_INTR_DISABLE || intr_type > GPIO_INTR_HIGH_LEVEL) {
        lua_pushboolean(L, false);
        lua_pushfstring(L, "Invalid GPIO interrupt type: %d (valid range: 0-%d)", intr_type, GPIO_INTR_HIGH_LEVEL);
        return 2;
    }
    io_conf.intr_type = intr_type;

    // 配置 GPIO
    ESP_LOGI(TAG, "Configuring GPIO%d: mode=%d, pull_up=%d, pull_down=%d, intr=%d",
        gpio_num, mode, io_conf.pull_up_en, io_conf.pull_down_en, intr_type);

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed for GPIO%d: %s", gpio_num, esp_err_to_name(err));
        lua_pushboolean(L, false);
        lua_pushfstring(L, "Failed to configure GPIO%d: %s", gpio_num, esp_err_to_name(err));
        return 2;
    }

    lua_pushboolean(L, true);
    return 1;
}

/**
 * @brief 设置GPIO输出电平
 *
 * @param gpio_num GPIO编号
 * @param level 电平值 (0或1)
 * @return 成功返回true，失败返回false和错误信息
 */
static int l_gpio_set_level(lua_State* L)
{
    gpio_num_t gpio_num = (gpio_num_t)luaL_checkinteger(L, 1);
    int level = luaL_checkinteger(L, 2);

    if (gpio_num < 0 || gpio_num >= GPIO_NUM_MAX) {
        lua_pushboolean(L, false);
        lua_pushfstring(L, "Invalid GPIO number: %d (valid range: 0-%d)", gpio_num, GPIO_NUM_MAX - 1);
        return 2;
    }

    if (level != 0 && level != 1) {
        lua_pushboolean(L, false);
        lua_pushfstring(L, "Invalid GPIO level: %d (must be 0 or 1)", level);
        return 2;
    }

    esp_err_t err = gpio_set_level(gpio_num, level);
    if (err != ESP_OK) {
        lua_pushboolean(L, false);
        lua_pushfstring(L, "Failed to set GPIO%d level: %s", gpio_num, esp_err_to_name(err));
        return 2;
    }

    lua_pushboolean(L, true);
    return 1;
}

/**
 * @brief 获取GPIO输入电平
 *
 * @param gpio_num GPIO编号
 * @return 成功返回GPIO电平 (0或1)，失败返回nil和错误信息
 */
static int l_gpio_get_level(lua_State* L)
{
    gpio_num_t gpio_num = (gpio_num_t)luaL_checkinteger(L, 1);

    if (gpio_num < 0 || gpio_num >= GPIO_NUM_MAX) {
        lua_pushnil(L);
        lua_pushfstring(L, "Invalid GPIO number: %d (valid range: 0-%d)", gpio_num, GPIO_NUM_MAX - 1);
        return 2;
    }

    int level = gpio_get_level(gpio_num);
    lua_pushinteger(L, level);
    return 1;
}

// 存储GPIO中断回调函数的注册表
typedef struct {
    lua_State* L;
    int ref;
} gpio_callback_t;

static gpio_callback_t gpio_callbacks[GPIO_NUM_MAX] = { 0 };

// GPIO中断处理函数
static void gpio_isr_handler(void* arg)
{
    gpio_num_t gpio_num = (uint32_t)arg;

    if (gpio_num < GPIO_NUM_MAX && gpio_callbacks[gpio_num].ref != LUA_REFNIL) {
        lua_State* L = gpio_callbacks[gpio_num].L;

        // 在中断处理函数中不能直接调用Lua函数，需要通过任务通知主循环
        // 这里简化处理，实际项目中应使用队列或任务通知
        ESP_LOGW(TAG, "GPIO%d interrupt triggered, but callback handling not implemented in ISR", gpio_num);
    }
}

/**
 * @brief 注册GPIO中断处理函数
 *
 * Lua参数:
 * {
 *   pin = <gpio_num>,          -- 必选，GPIO引脚号
 *   callback = <function>,     -- 必选，中断回调函数
 *   intr = <gpio_intr_type>    -- 可选，中断触发类型（默认GPIO_INTR_ANYEDGE）
 * }
 *
 * @return 成功返回true，失败返回false和错误信息
 */
static int l_gpio_set_interrupt(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    // 获取GPIO编号
    gpio_num_t gpio_num = (gpio_num_t)get_table_int_field(L, 1, "pin", 0);
    if (gpio_num < 0 || gpio_num >= GPIO_NUM_MAX) {
        lua_pushboolean(L, false);
        lua_pushfstring(L, "Invalid GPIO number: %d (valid range: 0-%d)", gpio_num, GPIO_NUM_MAX - 1);
        return 2;
    }

    // 获取回调函数
    lua_getfield(L, 1, "callback");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, false);
        lua_pushstring(L, "Interrupt callback must be a function");
        return 2;
    }

    // 获取中断类型（默认双边沿触发）
    gpio_int_type_t intr_type = (gpio_int_type_t)get_table_int_field(L, 1, "intr", GPIO_INTR_ANYEDGE);
    if (intr_type < GPIO_INTR_DISABLE || intr_type > GPIO_INTR_HIGH_LEVEL) {
        lua_pop(L, 1); // 弹出回调函数
        lua_pushboolean(L, false);
        lua_pushfstring(L, "Invalid GPIO interrupt type: %d (valid range: 0-%d)", intr_type, GPIO_INTR_HIGH_LEVEL);
        return 2;
    }

    // 保存回调函数引用
    if (gpio_callbacks[gpio_num].ref != LUA_REFNIL) {
        // 释放之前的引用
        luaL_unref(L, LUA_REGISTRYINDEX, gpio_callbacks[gpio_num].ref);
    }

    gpio_callbacks[gpio_num].L = L;
    gpio_callbacks[gpio_num].ref = luaL_ref(L, LUA_REGISTRYINDEX);

    // 设置中断类型
    esp_err_t err = gpio_set_intr_type(gpio_num, intr_type);
    if (err != ESP_OK) {
        luaL_unref(L, LUA_REGISTRYINDEX, gpio_callbacks[gpio_num].ref);
        gpio_callbacks[gpio_num].ref = LUA_REFNIL;

        lua_pushboolean(L, false);
        lua_pushfstring(L, "Failed to set interrupt type for GPIO%d: %s", gpio_num, esp_err_to_name(err));
        return 2;
    }

    // 注册中断处理函数
    err = gpio_isr_handler_add(gpio_num, gpio_isr_handler, (void*)gpio_num);
    if (err != ESP_OK) {
        luaL_unref(L, LUA_REGISTRYINDEX, gpio_callbacks[gpio_num].ref);
        gpio_callbacks[gpio_num].ref = LUA_REFNIL;

        lua_pushboolean(L, false);
        lua_pushfstring(L, "Failed to add ISR handler for GPIO%d: %s", gpio_num, esp_err_to_name(err));
        return 2;
    }

    ESP_LOGI(TAG, "GPIO%d interrupt handler registered (type: %d)", gpio_num, intr_type);
    lua_pushboolean(L, true);
    return 1;
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

// 注册GPIO模块
void register_lua_gpio(lua_State* L)
{
    // 注册为模块
    luaL_requiref(L, "gpio", luaopen_gpio, 1);
    lua_pop(L, 1); // 弹出模块表

    // 初始化GPIO驱动
    gpio_install_isr_service(0);
    ESP_LOGI(TAG, "GPIO module registered successfully");

    // 初始化回调注册表
    for (int i = 0; i < GPIO_NUM_MAX; i++) {
        gpio_callbacks[i].ref = LUA_REFNIL;
    }
}

// 清理GPIO资源
void unregister_lua_gpio(lua_State* L)
{
    // 移除所有中断处理函数
    for (int i = 0; i < GPIO_NUM_MAX; i++) {
        if (gpio_callbacks[i].ref != LUA_REFNIL) {
            gpio_isr_handler_remove((gpio_num_t)i);
            luaL_unref(L, LUA_REGISTRYINDEX, gpio_callbacks[i].ref);
            gpio_callbacks[i].ref = LUA_REFNIL;
        }
    }

    // 卸载ISR服务
    gpio_uninstall_isr_service();
    ESP_LOGI(TAG, "GPIO module unregistered successfully");
}