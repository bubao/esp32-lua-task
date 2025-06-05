#include "lua_gpio.h"
#include "driver/gpio.h"
#include <lauxlib.h>

static int l_gpio_set_mode(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    gpio_config_t io_conf = { 0 };

    // 读取 pin
    lua_getfield(L, 1, "pin");
    gpio_num_t gpio_num = (gpio_num_t)luaL_checkinteger(L, -1);
    io_conf.pin_bit_mask = 1ULL << gpio_num;
    lua_pop(L, 1);

    // 读取 mode
    lua_getfield(L, 1, "mode");
    io_conf.mode = (gpio_mode_t)luaL_checkinteger(L, -1);
    lua_pop(L, 1);

    // pull_up (bool)
    lua_getfield(L, 1, "pull_up");
    io_conf.pull_up_en = lua_toboolean(L, -1) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    lua_pop(L, 1);

    // pull_down (bool)
    lua_getfield(L, 1, "pull_down");
    io_conf.pull_down_en = lua_toboolean(L, -1) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
    lua_pop(L, 1);

    // 中断类型 (optional)
    lua_getfield(L, 1, "intr");
    if (!lua_isnil(L, -1)) {
        io_conf.intr_type = (gpio_int_type_t)luaL_checkinteger(L, -1);
    } else {
        io_conf.intr_type = GPIO_INTR_DISABLE;
    }
    lua_pop(L, 1);

    // 配置 GPIO
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        return luaL_error(L, "gpio_config failed: %s", esp_err_to_name(err));
    }

    return 0;
}

static int l_gpio_set_level(lua_State* L)
{
    int gpio_num = luaL_checkinteger(L, 1);
    int level = luaL_checkinteger(L, 2);
    gpio_set_level(gpio_num, level);
    return 0;
}

static int l_gpio_get_level(lua_State* L)
{
    int gpio_num = luaL_checkinteger(L, 1);
    lua_pushinteger(L, gpio_get_level(gpio_num));
    return 1;
}

void register_lua_gpio(lua_State* L)
{
    lua_newtable(L);

    lua_pushcfunction(L, l_gpio_set_mode);
    lua_setfield(L, -2, "set_mode");

    lua_pushcfunction(L, l_gpio_set_level);
    lua_setfield(L, -2, "set_level");

    lua_pushcfunction(L, l_gpio_get_level);
    lua_setfield(L, -2, "get_level");

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

    lua_pushinteger(L, GPIO_INTR_POSEDGE);
    lua_setfield(L, -2, "INTR_POSEDGE");
    lua_pushinteger(L, GPIO_INTR_NEGEDGE);
    lua_setfield(L, -2, "INTR_NEGEDGE");
    lua_pushinteger(L, GPIO_INTR_ANYEDGE);
    lua_setfield(L, -2, "INTR_ANYEDGE");

    lua_pushinteger(L, GPIO_PULLUP_DISABLE);
    lua_setfield(L, -2, "PULLUP_DISABLE");
    lua_pushinteger(L, GPIO_PULLUP_ENABLE);
    lua_setfield(L, -2, "PULLUP_ENABLE");

    lua_pushinteger(L, GPIO_PULLDOWN_DISABLE);
    lua_setfield(L, -2, "PULLDOWN_DISABLE");
    lua_pushinteger(L, GPIO_PULLDOWN_ENABLE);
    lua_setfield(L, -2, "PULLDOWN_ENABLE");

    lua_setglobal(L, "gpio");
}
