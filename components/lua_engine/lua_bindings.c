#include "lua_adc.h"
#include "lua_file.h"
#include "lua_gpio.h"
#include "lua_timer.h"

void register_lua_bindings(lua_State* L)
{
    register_lua_gpio(L);
    register_lua_adc(L);
    register_lua_file(L);
    register_lua_timer(L);
}
