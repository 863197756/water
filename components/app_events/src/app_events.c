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

esp_err_t app_events_post_net_ready(int net_type) {
    app_event_net_status_t evt = { .type = net_type };
    return esp_event_post(APP_EVENTS, APP_EVENT_NET_READY, &evt, sizeof(evt), portMAX_DELAY);
}

esp_err_t app_events_post_net_lost(int net_type) {
    app_event_net_status_t evt = { .type = net_type };
    return esp_event_post(APP_EVENTS, APP_EVENT_NET_LOST, &evt, sizeof(evt), portMAX_DELAY);
}

esp_err_t app_events_post_mqtt_connected(void) {
    return esp_event_post(APP_EVENTS, APP_EVENT_MQTT_CONNECTED, NULL, 0, portMAX_DELAY);
}

esp_err_t app_events_post_mqtt_disconnected(void) {
    return esp_event_post(APP_EVENTS, APP_EVENT_MQTT_DISCONNECTED, NULL, 0, portMAX_DELAY);
}
