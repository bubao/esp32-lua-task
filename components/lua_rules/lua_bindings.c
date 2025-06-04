#include "lua_bindings.h"
#include "driver/adc.h"
#include "driver/gpio.h"
#include "driver/mcpwm.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lua_functions.h"
#include "rom/gpio.h"
#include "soc/gpio_num.h"
#include <dirent.h>
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

timer_task_t* timer_list = NULL; // 不要 static，供多文件共享

// 定时器句柄
static esp_timer_handle_t lua_timer_handle = NULL;

// 函数声明提前，防止隐式声明错误
void insert_timer_task(timer_task_t* task);
void remove_timer_task(lua_State* co);
static int l_log(lua_State* L);

// timer_process 的 esp_timer 回调
static void lua_timer_callback(void* arg)
{
    extern lua_State* global_L;
    timer_process(global_L);
}

LuaCoroutine* get_current_coroutine(lua_State* L)
{
    for (int i = 0; i < MAX_COROUTINES; i++) {
        if (coroutines[i].co == L) {
            return &coroutines[i];
        }
    }
    return NULL;
}

int64_t get_now_us()
{
    return esp_timer_get_time(); // 微秒
}
int l_delay(lua_State* L)
{
    return l_settimeout(L); // 调用 l_settimeout 只 yield，不再插入 timer_task
}

// 移除已有同协程的 timer_task，防止重复
void remove_timer_task(lua_State* co)
{
    timer_task_t** p = &timer_list;
    while (*p) {
        if ((*p)->co == co) {
            timer_task_t* to_free = *p;
            *p = (*p)->next;
            free(to_free);
        } else {
            p = &(*p)->next;
        }
    }
}

int l_settimeout(lua_State* L)
{
    int delay_ms = luaL_checkinteger(L, 1);
    int delay_us = delay_ms * 1000;
    ESP_LOGI("LUA", "l_settimeout called, ms=%d", delay_ms);
    lua_pushinteger(L, delay_us);
    return lua_yield(L, 1); // yield 微秒
}

void insert_timer_task(timer_task_t* task)
{
    timer_task_t** current = &timer_list;

    // 找到第一个唤醒时间晚于当前 task 的位置
    while (*current && (*current)->wakeup_time_us <= task->wakeup_time_us) {
        current = &(*current)->next;
    }

    // 插入 task
    task->next = *current;
    *current = task;
}

void timer_process(lua_State* L)
{
    int64_t now = get_now_us();
    timer_task_t** current = &timer_list;
    int max_iter = 1000; // 死循环保护

    // 打印 timer_list 状态
    int task_count = 0;
    timer_task_t* debug_task = timer_list;
    // ESP_LOGI("LUA", "timer_process: timer_list status:");
    while (debug_task && task_count < 20) {
        // ESP_LOGI("LUA", "  task[%d]: co=%p, wakeup_us=%lld", task_count, debug_task->co, debug_task->wakeup_time_us);
        debug_task = debug_task->next;
        task_count++;
    }
    if (task_count >= 20) {
        ESP_LOGW("LUA", "  ...more tasks in timer_list, truncated log");
    }

    while (*current && max_iter-- > 0) {
        timer_task_t* task = *current;
        ESP_LOGI("LUA", "timer_process: now=%lld, task co=%p, wakeup_time_us=%lld", now, task->co, task->wakeup_time_us);
        if (task->wakeup_time_us > now) {
            break;
        }
        lua_State* co = task->co;
        int nresults = 0;
        int status = lua_resume(co, NULL, 0, &nresults);
        ESP_LOGI("LUA", "timer_process: resume status=%d, nresults=%d", status, nresults);
        if ((status == LUA_YIELD || status == LUA_OK) && nresults > 0) {
            int found = 0;
            int64_t delay_us = 0;
            for (int i = 0; i < nresults; i++) {
                int idx = -nresults + i;
                int type = lua_type(co, idx);
                ESP_LOGI("LUA", "timer_process: result[%d] type: %s", i, lua_typename(co, type));
                if (lua_isnumber(co, idx)) {
                    delay_us = (int64_t)lua_tointeger(co, idx);
                    found = 1;
                }
            }
            lua_pop(co, nresults);
            if (found) {
                ESP_LOGI("LUA", "timer_process: got delay_us=%lld, will reschedule", delay_us);
                timer_task_t* next = task->next;
                task->wakeup_time_us = get_now_us() + delay_us;
                insert_timer_task(task);
                *current = next;
                continue;
            } else {
                ESP_LOGW("LUA", "timer_process: yield/return but no number in results, co=%p, status=%d, nresults=%d", co, status, nresults);
                for (int i = 0; i < nresults; i++) {
                    int idx = -nresults + i;
                    int type = lua_type(co, idx);
                    if (type == LUA_TNUMBER) {
                        ESP_LOGW("LUA", "  result[%d]: type=number, value=%lld", i, (long long)lua_tointeger(co, idx));
                    } else if (type == LUA_TSTRING) {
                        ESP_LOGW("LUA", "  result[%d]: type=string, value=%s", i, lua_tostring(co, idx));
                    } else if (type == LUA_TBOOLEAN) {
                        ESP_LOGW("LUA", "  result[%d]: type=boolean, value=%d", i, lua_toboolean(co, idx));
                    } else {
                        ESP_LOGW("LUA", "  result[%d]: type=%s", i, lua_typename(co, type));
                    }
                }
                lua_pop(co, nresults);
                *current = task->next;
                free(task);
                continue;
            }
        }
        ESP_LOGI("LUA", "timer_process: no delay, coroutine finished, co=%p, status=%d, nresults=%d", co, status, nresults);
        *current = task->next;
        free(task);
    }
    if (max_iter <= 0) {
        ESP_LOGE("LUA", "timer_process: possible dead loop in timer_list! (exceeded 1000 iterations)");
    }
}

// Lua函数：设置GPIO模式
static int l_gpio_set_mode(lua_State* L)
{
    gpio_num_t gpio_num = (gpio_num_t)luaL_checkinteger(L, 1);
    gpio_mode_t mode = (gpio_mode_t)luaL_checkinteger(L, 2);
    gpio_pad_select_gpio(gpio_num);
    gpio_set_direction(gpio_num, (gpio_mode_t)mode);
    return 0;
}

// Lua函数：设置GPIO电平
static int l_gpio_set_level(lua_State* L)
{
    int gpio_num = luaL_checkinteger(L, 1);
    int level = luaL_checkinteger(L, 2);
    gpio_set_level(gpio_num, level);
    return 0;
}

// Lua函数：读取GPIO电平
static int l_gpio_get_level(lua_State* L)
{
    int gpio_num = luaL_checkinteger(L, 1);
    int level = gpio_get_level(gpio_num);
    lua_pushinteger(L, level);
    return 1;
}

// Lua函数：读取ADC信号
static int l_read_adc(lua_State* L)
{
    int adc_channel = luaL_checkinteger(L, 1);
    adc1_channel_t channel = (adc1_channel_t)adc_channel;
    int adc_value = adc1_get_raw(channel);
    lua_pushinteger(L, adc_value);
    return 1;
}

// Lua函数：读取文件内容
static int l_file_read(lua_State* L)
{
    const char* filename = luaL_checkstring(L, 1);
    FILE* file = fopen(filename, "r");
    if (!file) {
        lua_pushnil(L);
        return 1;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char* buffer = (char*)malloc(size + 1);
    fread(buffer, 1, size, file);
    fclose(file);
    buffer[size] = '\0';
    lua_pushstring(L, buffer);
    free(buffer);
    return 1;
}

// Lua函数：写入文件内容
static int l_file_write(lua_State* L)
{
    const char* filename = luaL_checkstring(L, 1);
    const char* content = luaL_checkstring(L, 2);
    FILE* file = fopen(filename, "w");
    if (!file) {
        lua_pushboolean(L, 0);
        return 1;
    }
    fwrite(content, 1, strlen(content), file);
    fclose(file);
    lua_pushboolean(L, 1);
    return 1;
}

// 模拟传感器读取
static int l_read_sensor(lua_State* L)
{
    static int value = 0;
    value = (value + 10) % 100;
    lua_pushinteger(L, value);
    return 1; // ✅ 表示有1个返回值传给 Lua
}

// Lua函数：日志输出
static int l_log(lua_State* L)
{
    const char* msg = luaL_checkstring(L, 1);
    ESP_LOGI("LUA", "%s", msg);
    return 0;
}

// 注册绑定函数
void register_lua_bindings(lua_State* L)
{

    lua_newtable(L);
    // 注册gpio.set_mode函数
    lua_pushcfunction(L, l_gpio_set_mode);
    lua_setfield(L, -2, "set_mode");

    // 注册gpio.set_level函数
    lua_pushcfunction(L, l_gpio_set_level);
    lua_setfield(L, -2, "set_level");

    // 注册gpio.get_level函数
    lua_pushcfunction(L, l_gpio_get_level);
    lua_setfield(L, -2, "get_level");

    // 注册gpio常量
    lua_pushinteger(L, GPIO_MODE_INPUT);
    lua_setfield(L, -2, "MODE_INPUT");

    lua_pushinteger(L, GPIO_MODE_OUTPUT);
    lua_setfield(L, -2, "MODE_OUTPUT");

    lua_pushinteger(L, GPIO_MODE_INPUT_OUTPUT);
    lua_setfield(L, -2, "MODE_INPUT_OUTPUT");

    lua_pushinteger(L, GPIO_MODE_INPUT_OUTPUT_OD);
    lua_setfield(L, -2, "MODE_INPUT_OUTPUT_OD");

    lua_pushinteger(L, GPIO_MODE_OUTPUT_OD);
    lua_setfield(L, -2, "MODE_OUTPUT_OD");

    lua_setglobal(L, "gpio");

    // 注册 settimeout/delay，绑定当前协程
    // lua_pushthread(L);
    // lua_pushcclosure(L, l_settimeout, 1);
    // lua_setglobal(L, "settimeout");
    // lua_pushthread(L);
    // lua_pushcclosure(L, l_delay, 1);
    // lua_setglobal(L, "delay");

    // lua_pushthread(L); // 当前协程线程

    // 创建pwm表
    // lua_newtable(L);

    // // 注册pwm.set_duty函数
    // lua_pushcfunction(L, l_pwm_set_duty);
    // lua_setfield(L, -2, "set_duty");

    // lua_setglobal(L, "pwm");

    // 注册read_adc函数
    lua_register(L, "read_adc", l_read_adc);

    // 注册file.read函数
    lua_register(L, "file.read", l_file_read);

    // 注册file.write函数
    lua_register(L, "file.write", l_file_write);

    lua_register(L, "read_sensor", l_read_sensor);

    lua_register(L, "log", l_log);

    // 注册 log 到全局，确保所有协程都能访问
    lua_pushcfunction(L, l_log);
    lua_setglobal(L, "log");

    // lua_pushcfunction(L, l_delay);
    // lua_setglobal(L, "delay");

    // 初始化ADC
    adc1_config_width(SOC_ADC_RTC_MAX_BITWIDTH);
    adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_0); // 根据需要配置ADC通道

    // 启动 esp_timer 定时器，每 10ms 调用一次 timer_process
    if (!lua_timer_handle) {
        const esp_timer_create_args_t timer_args = {
            .callback = &lua_timer_callback,
            .name = "lua_timer"
        };
        esp_timer_create(&timer_args, &lua_timer_handle);
        esp_timer_start_periodic(lua_timer_handle, 10 * 1000); // 10ms
    }
}
