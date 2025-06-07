#include "lua_adc.h"
#include "lua_cron.h"
#include "lua_event.h"
#include "lua_file.h"
#include "lua_gpio.h"
#include "lua_http.h"
#include "lua_log.h"
#include "lua_pwm.h"
#include "lua_reg.h"
#include "lua_timer.h"

void register_lua_bindings(lua_State* L)
{
    register_lua_gpio(L);
    register_lua_adc(L);
    register_lua_file(L);
    register_lua_log(L);
    register_lua_timer(L);
    register_lua_cron(L);
    register_reg_device(L);
    register_lua_event(L);
    register_lua_http(L);
    register_lua_pwm(L);
}