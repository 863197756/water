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
} app_event_id_t;

typedef struct {
    int mode; // 0: Wi-Fi, 1: 4G
} app_event_net_mode_t;

esp_err_t app_events_post_net_mode_request(int mode);
esp_err_t app_events_post_mqtt_config_updated(void);
esp_err_t app_events_post_mqtt_plan_received(void);

#ifdef __cplusplus
}
#endif
