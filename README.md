# Lua 规则引擎（基于 ESP-IDF + Lua 5.5.0）

在 ESP32 平台的物联网应用中，设备控制与响应规则常常需要灵活配置且便于迭代。为提高开发效率、降低耦合度，拟构建一个基于 Lua 的规则引擎框架。

本项目目标：

* 实现设备初始化与管理逻辑的 Lua 描述
* 支持事件发布-订阅机制（C ↔ Lua）
* 支持基于传感器阈值及定时器的自动化控制逻辑
* 提供可拓展的规则管理和热更新能力

---

## 核心功能需求

### 1. 设备初始化

* 统一定义所有传感器与执行器，避免重复初始化
* Lua 中定义设备类型、GPIO、ID，调用 C 接口注册
* C 端提供 `c_init_device(type, gpio, id)` 接口

### 2. 事件监听（发布订阅）

* C 端触发事件，通过 `dispatcher.dispatch(event, payload)` 通知 Lua
* Lua 可使用 `dispatcher.subscribe(event, callback)` 注册监听逻辑
* 支持传感器值变化、系统状态等事件类型

### 3. 规则定义与触发

* 阈值触发：基于传感器值自动控制执行器
* 定时触发：支持 Cron 表达式或周期性触发（interval）
* 规则模块分开管理（如 rule\_fan.lua、rule\_led.lua）

### 4. Lua 与 C 的接口

```lua
-- 注册的 C 函数
c_init_device(type, gpio, id)
c_set_device_state(id, bool)
c_get_sensor_value(id) -> float
c_log(level, message)
c_subscribe_event(event)
c_add_timer(timer_id, cron_expr)
```

---

## 结构划分

```tree
assets
├── device_templates.lua
├── dispatcher.lua        # 事件发布/订阅
├── init.lua              # 设备初始化定义
├── main.lua              # Lua 入口文件
├── rules                 # 执行规则：
│   ├── fan1.lua
│   └── led1.lua
└── scheduler.lua         # 定时调度器
```

---

## 高级及扩展功能

### 1. 热更新机制

* 通过 C 层支持文件热加载（重载指定 rule 模块）
* Lua 提供 `reload_rule(name)` 接口

### 2. 表达式解析引擎

* 支持 DSL 或 JSON 定义规则：如 `{ "if": "temp1 < 20", "then": "fan1:on" }`

### 3. 状态持久化

* 保存设备状态至 NVS 或 Flash，避免断电丢失

### 4. 日志记录与诊断

* 提供日志等级（info/warn/error）
* 持久化重要事件记录，支持远程上传
