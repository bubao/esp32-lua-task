#include "lua_bindings.h"
#include "driver/adc.h"
#include "driver/gpio.h"
#include "driver/mcpwm.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_spi_flash.h"
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

int64_t get_now_ms()
{
    return esp_timer_get_time() / 1000;
}
int l_delay(lua_State* L)
{
    int delay_ms = luaL_checkinteger(L, 1);
    lua_State* co = lua_tothread(L, lua_upvalueindex(1));
    if (!co)
        co = L;
    remove_timer_task(co); // 只保留一个同协程的任务
    timer_task_t* task = malloc(sizeof(timer_task_t));
    task->co = co;
    task->wakeup_time_ms = get_now_ms() + delay_ms;
    insert_timer_task(task);
    lua_pushinteger(L, delay_ms);
    return lua_yield(L, 1);
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
    lua_State* co = lua_tothread(L, lua_upvalueindex(1));
    ESP_LOGI("LUA", "l_settimeout called, co=%p, ms=%d", co, delay_ms);
    if (!co)
        co = L;
    remove_timer_task(co); // 只保留一个同协程的任务
    timer_task_t* task = malloc(sizeof(timer_task_t));
    task->co = co;
    task->wakeup_time_ms = get_now_ms() + delay_ms;
    ESP_LOGI("LUA", "settimeout: now_ms=%lld, wakeup_time_ms=%lld", get_now_ms(), task->wakeup_time_ms);
    // 插入到链表有序位置
    insert_timer_task(task);
    lua_pushinteger(L, delay_ms);
    return lua_yield(L, 1);
}

void insert_timer_task(timer_task_t* task)
{
    timer_task_t** current = &timer_list;

    // 找到第一个唤醒时间晚于当前 task 的位置
    while (*current && (*current)->wakeup_time_ms <= task->wakeup_time_ms) {
        current = &(*current)->next;
    }

    // 插入 task
    task->next = *current;
    *current = task;
}

void timer_process(lua_State* L)
{
    int64_t now = get_now_ms();
    timer_task_t** current = &timer_list;

    while (*current) {
        timer_task_t* task = *current;
        if (task->wakeup_time_ms > now) {
            break;
        }
        lua_State* co = task->co;
        // 获取 table 名
        LuaCoroutine* coro = get_current_coroutine(co);
        if (!coro) {
            free(task);
            *current = task->next;
            continue;
        }
        lua_getglobal(co, coro->rule_table_name); // table
        lua_getfield(co, -1, "loop"); // table.loop
        lua_remove(co, -2); // remove table
        if (!lua_isfunction(co, -1)) {
            ESP_LOGW("LUA", "Coroutine loop() not found, skip");
            lua_pop(co, 1);
            *current = task->next;
            free(task);

            continue;
        }
        int nresults = 0;
        int status = lua_resume(co, NULL, 0, &nresults);
        ESP_LOGI("LUA", "timer_process: resume status=%d, nresults=%d", status, nresults);
        if (status != LUA_OK && status != LUA_YIELD) {
            const char* err = lua_tostring(co, -1);
            ESP_LOGE("LUA", "Coroutine error: %s", err);
            lua_pop(co, 1);
            *current = task->next;
            free(task);

            continue;
        }
        // 如果有返回值且为数字，表示要重新调度
        if (nresults > 0 && lua_isnumber(co, -1)) {
            int delay_ms = lua_tointeger(co, -1);
            ESP_LOGI("LUA", "timer_process: got delay_ms=%d, will reschedule", delay_ms);
            lua_pop(co, 1);
            task->wakeup_time_ms = now + delay_ms;
            insert_timer_task(task);
            *current = task->next;
        } else {
            ESP_LOGI("LUA", "timer_process: no delay, coroutine finished");
            free(task);
            *current = task->next;
        }
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
