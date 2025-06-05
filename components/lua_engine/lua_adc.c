#include "lua_adc.h"
#include "driver/adc.h"
#include <lauxlib.h>

static int l_read_adc(lua_State* L)
{
    int adc_channel = luaL_checkinteger(L, 1);
    adc1_channel_t channel = (adc1_channel_t)adc_channel;
    int value = adc1_get_raw(channel);
    lua_pushinteger(L, value);
    return 1;
}

void register_lua_adc(lua_State* L)
{
    adc1_config_width(SOC_ADC_RTC_MAX_BITWIDTH);
    adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_0); // 可适配参数
    lua_register(L, "read_adc", l_read_adc);
}
