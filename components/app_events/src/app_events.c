#include "app_events.h"
#include "freertos/FreeRTOS.h"

ESP_EVENT_DEFINE_BASE(APP_EVENTS);

esp_err_t app_events_post_net_mode_request(int mode) {
    app_event_net_mode_t evt = { .mode = mode };
    return esp_event_post(APP_EVENTS, APP_EVENT_NET_MODE_REQUEST, &evt, sizeof(evt), portMAX_DELAY);
}

esp_err_t app_events_post_mqtt_config_updated(void) {
    return esp_event_post(APP_EVENTS, APP_EVENT_MQTT_CONFIG_UPDATED, NULL, 0, portMAX_DELAY);
}

esp_err_t app_events_post_mqtt_plan_received(void) {
    return esp_event_post(APP_EVENTS, APP_EVENT_MQTT_PLAN_RECEIVED, NULL, 0, portMAX_DELAY);
}
