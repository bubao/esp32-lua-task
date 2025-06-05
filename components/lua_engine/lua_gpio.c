#include "lua_gpio.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <lauxlib.h>

static int l_gpio_set_mode(lua_State* L)
{
    gpio_num_t gpio_num = (gpio_num_t)luaL_checkinteger(L, 1);
    gpio_mode_t mode = (gpio_mode_t)luaL_checkinteger(L, 2);

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << gpio_num,
        .mode = mode,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

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

    lua_pushinteger(L, GPIO_MODE_INPUT);
    lua_setfield(L, -2, "MODE_INPUT");
    lua_pushinteger(L, GPIO_MODE_OUTPUT);
    lua_setfield(L, -2, "MODE_OUTPUT");

    lua_setglobal(L, "gpio");
}
