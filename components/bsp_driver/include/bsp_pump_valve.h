#pragma once
#include <stdbool.h>

// 初始化 GPIO
void bsp_pump_valve_init(void);

// 控制进水阀 (false:关, true:开)
void bsp_set_inlet_valve(bool open);

// 控制增压泵
void bsp_set_pump(bool open);

// 控制冲洗阀 (如果有废水阀/冲洗阀)
void bsp_set_flush_valve(bool open);

// 控制传感器电源 (false: 关闭, true: 开启)
void bsp_set_sensor_power(bool enable);

// 获取外设当前状态 (true = 开启，false = 关闭)
bool bsp_get_inlet_valve_state(void);
bool bsp_get_flush_valve_state(void);
bool bsp_get_pump_state(void);