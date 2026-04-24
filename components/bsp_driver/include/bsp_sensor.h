#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 初始化所有传感器 (ADC, PCNT, GPIO 轮询任务)
void bsp_sensor_init(void);

// 获取温度值 (摄氏度)
float bsp_sensor_get_temperature(void);

// 获取 TDS 值 (ppm)，内部已做温度补偿
int bsp_sensor_get_tds_in(void);
int bsp_sensor_get_tds_out(void);
int bsp_sensor_get_tds_backup(void);

// 流量计接口
uint32_t bsp_sensor_get_flow_pulses(void);
void bsp_sensor_clear_flow_pulses(void);

float bsp_sensor_get_pump_current(void);
void bsp_sensor_pump_current_calibrate_zero(void);

#ifdef __cplusplus
}
#endif
