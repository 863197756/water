#pragma once

#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(APP_EVENTS);

typedef enum {
    APP_EVENT_NET_MODE_REQUEST = 1,
    APP_EVENT_MQTT_CONFIG_UPDATED,
    APP_EVENT_MQTT_PLAN_RECEIVED,
    APP_EVENT_NET_READY,
    APP_EVENT_NET_LOST,
    APP_EVENT_MQTT_CONNECTED,
    APP_EVENT_MQTT_DISCONNECTED,

    // --- 真实硬件传感器事件 (由 bsp_sensor 或外部中断触发) ---
    APP_EVENT_HW_LOW_PRESSURE_ALARM,   // 进水水压低 (低压开关断开)
    APP_EVENT_HW_LOW_PRESSURE_RECOVER, // 进水水压恢复 (低压开关闭合)
    APP_EVENT_HW_HIGH_PRESSURE_ALARM,  // 储水桶满 (高压开关闭合)
    APP_EVENT_HW_HIGH_PRESSURE_RECOVER,// 水压下降请求制水 (高压开关断开)
    
    // --- 业务控制事件 ---
    APP_EVENT_CMD_START_WASH,          // 云端下发或按键触发的冲洗指令
    APP_EVENT_OTA_START,               // 触发OTA升级
} app_event_id_t;

typedef struct {
    int mode; // 0: Wi-Fi, 1: 4G
} app_event_net_mode_t;

typedef enum {
    APP_NET_TYPE_WIFI = 0,
    APP_NET_TYPE_PPP = 1,
} app_net_type_t;

typedef struct {
    int type; // app_net_type_t
} app_event_net_status_t;

esp_err_t app_events_post_net_mode_request(int mode);
esp_err_t app_events_post_mqtt_config_updated(void);
esp_err_t app_events_post_mqtt_plan_received(void);
esp_err_t app_events_post_net_ready(int net_type);
esp_err_t app_events_post_net_lost(int net_type);
esp_err_t app_events_post_mqtt_connected(void);
esp_err_t app_events_post_mqtt_disconnected(void);

#ifdef __cplusplus
}
#endif
