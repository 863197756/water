#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- 独立指示灯的状态枚举 ---
typedef enum {
    LED_ALARM_OFF = 0,
    LED_ALARM_ON,
    LED_ALARM_BLINK
} led_alarm_state_t;

// --- 面板整体业务工作模式枚举 ---
typedef enum {
    LED_MODE_STANDBY = 0,     // 正常待机 (流水灯灭)
    LED_MODE_MAKING_WATER,    // 正常制水 (流水灯跑动，LED13交替闪)
    LED_MODE_NETWORKING,      // 配网/联网 (流水灯全亮并闪烁)
    LED_MODE_LOW_BALANCE,     // 余额不足 (左侧流水灯全亮)
    LED_MODE_FILTER_EXPIRE    // 滤芯到期 (右侧流水灯全亮)
} led_work_mode_t;

/**
 * @brief 初始化 TM1650 LED 驱动及后台刷新任务
 */
void bsp_led_init(void);

/**
 * @brief 设置独立警报灯/状态灯 (冲洗、缺水、水满、漏水)
 * @param led_idx  1=冲洗, 2=缺水, 3=水满, 4=漏水
 * @param state    OFF, ON, 或 BLINK (闪烁)
 */
void bsp_led_set_alarm(uint8_t led_idx, led_alarm_state_t state);

/**
 * @brief 设置面板整体流水灯及制水灯的工作模式
 * @param mode 业务模式 (待机、制水、配网、余额不足、滤芯到期)
 */
void bsp_led_set_mode(led_work_mode_t mode);

#ifdef __cplusplus
}
#endif