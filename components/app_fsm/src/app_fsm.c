#include "app_fsm.h"

#include <stdbool.h>
#include <string.h>
#include "esp_log.h"
#include "esp_event.h"
#include "app_events.h"
#include "app_storage.h"
#include "net_manager.h"
#include "mqtt_manager.h"

typedef enum {
    FSM_STATE_IDLE = 0,
    FSM_STATE_WAIT_NET,
    FSM_STATE_MQTT_CONNECTING,
    FSM_STATE_MQTT_READY,
    FSM_STATE_RUNNING,
} fsm_state_t;

static const char *TAG = "APP_FSM";
static fsm_state_t s_state = FSM_STATE_IDLE;
static bool s_net_ready = false;

static const char *state_name(fsm_state_t state) {
    switch (state) {
        case FSM_STATE_IDLE: return "IDLE";
        case FSM_STATE_WAIT_NET: return "WAIT_NET";
        case FSM_STATE_MQTT_CONNECTING: return "MQTT_CONNECTING";
        case FSM_STATE_MQTT_READY: return "MQTT_READY";
        case FSM_STATE_RUNNING: return "RUNNING";
        default: return "UNKNOWN";
    }
}

static const char *net_type_name(int type) {
    switch (type) {
        case APP_NET_TYPE_WIFI: return "WIFI";
        case APP_NET_TYPE_PPP: return "PPP";
        default: return "UNKNOWN";
    }
}

static void transition_to(fsm_state_t next, const char *reason) {
    if (s_state == next) {
        return;
    }
    ESP_LOGI(TAG, "State: %s -> %s (%s)", state_name(s_state), state_name(next), reason);
    s_state = next;
}

static bool has_valid_mqtt_config(void) {
    net_config_t cfg = {0};
    if (app_storage_load_net_config(&cfg) != ESP_OK) {
        return false;
    }
    return strlen(cfg.full_url) > 5;
}

static void try_start_mqtt(const char *reason) {
    if (!s_net_ready) {
        ESP_LOGI(TAG, "Skip MQTT start: network not ready (%s)", reason);
        return;
    }

    if (!has_valid_mqtt_config()) {
        ESP_LOGW(TAG, "Skip MQTT start: invalid MQTT config (%s)", reason);
        return;
    }

    ESP_LOGI(TAG, "Starting MQTT (%s)", reason);
    mqtt_manager_start();
    transition_to(FSM_STATE_MQTT_CONNECTING, reason);
}

static void on_app_event(void *arg, esp_event_base_t event_base,
                         int32_t event_id, void *event_data) {
    if (event_base != APP_EVENTS) {
        return;
    }

    switch (event_id) {
        case APP_EVENT_NET_MODE_REQUEST: {
            if (!event_data) {
                break;
            }
            const app_event_net_mode_t *req = (const app_event_net_mode_t *)event_data;
            ESP_LOGI(TAG, "Handle NET_MODE_REQUEST: %d", req->mode);
            s_net_ready = false;
            mqtt_manager_stop();
            net_manager_set_mode(req->mode);
            transition_to(FSM_STATE_WAIT_NET, "net mode request");
            break;
        }

        case APP_EVENT_MQTT_CONFIG_UPDATED:
            ESP_LOGI(TAG, "Handle MQTT_CONFIG_UPDATED");
            try_start_mqtt("mqtt config updated");
            break;

        case APP_EVENT_NET_READY:
            if (event_data) {
                const app_event_net_status_t *net = (const app_event_net_status_t *)event_data;
                ESP_LOGI(TAG, "Handle NET_READY: %s", net_type_name(net->type));
            }
            if (s_net_ready && (s_state == FSM_STATE_MQTT_CONNECTING ||
                                s_state == FSM_STATE_MQTT_READY ||
                                s_state == FSM_STATE_RUNNING)) {
                ESP_LOGI(TAG, "Ignore duplicated NET_READY in state %s", state_name(s_state));
                break;
            }
            s_net_ready = true;
            transition_to(FSM_STATE_WAIT_NET, "network ready");
            try_start_mqtt("network ready");
            break;

        case APP_EVENT_NET_LOST:
            if (event_data) {
                const app_event_net_status_t *net = (const app_event_net_status_t *)event_data;
                ESP_LOGW(TAG, "Handle NET_LOST: %s", net_type_name(net->type));
            }
            s_net_ready = false;
            mqtt_manager_stop();
            transition_to(FSM_STATE_WAIT_NET, "network lost");
            break;

        case APP_EVENT_MQTT_CONNECTED:
            transition_to(FSM_STATE_MQTT_READY, "mqtt connected");
            break;

        case APP_EVENT_MQTT_PLAN_RECEIVED:
            transition_to(FSM_STATE_RUNNING, "plan received");
            break;

        case APP_EVENT_MQTT_DISCONNECTED:
            if (s_net_ready) {
                transition_to(FSM_STATE_MQTT_CONNECTING, "mqtt disconnected");
            } else {
                transition_to(FSM_STATE_WAIT_NET, "mqtt disconnected no network");
            }
            break;

        default:
            break;
    }
}

void app_fsm_init(void) {
    ESP_ERROR_CHECK(esp_event_handler_register(APP_EVENTS, ESP_EVENT_ANY_ID, &on_app_event, NULL));
    transition_to(FSM_STATE_WAIT_NET, "fsm init");
}
