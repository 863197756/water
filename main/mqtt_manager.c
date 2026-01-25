#include "mqtt_manager.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "app_storage.h"
#include "protocol.h"
#include "app_logic.h"

static const char *TAG = "MQTT";

static esp_mqtt_client_handle_t s_client = NULL;
static bool s_connected = false;
static char s_topic_cmd[64];    // 订阅: device/MAC/cmd
static char s_topic_report[64]; // 发布: device/MAC/report

// 生成 Topic: device/AABBCCDDEEFF/cmd
static void generate_topics(void) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    
    snprintf(s_topic_cmd, sizeof(s_topic_cmd), "device/%02X%02X%02X%02X%02X%02X/cmd",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
             
    snprintf(s_topic_report, sizeof(s_topic_report), "device/%02X%02X%02X%02X%02X%02X/report",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
             
    ESP_LOGI(TAG, "Topics generated:\nSub: %s\nPub: %s", s_topic_cmd, s_topic_report);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT Connected");
        s_connected = true;
        // 连接成功后，立刻订阅指令主题
        esp_mqtt_client_subscribe(s_client, s_topic_cmd, 1);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT Disconnected");
        s_connected = false;
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT Data received on %.*s", event->topic_len, event->topic);
        // 1. 处理接收到的 JSON
        server_cmd_t cmd;
        if (protocol_parse_cmd(event->data, event->data_len, &cmd) == ESP_OK) {
            // 2. 转交业务逻辑层
            app_logic_handle_cmd(&cmd);
        } else {
            ESP_LOGW(TAG, "Failed to parse command");
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT Error");
        break;
        
    default:
        break;
    }
}

void mqtt_manager_init(void) {
    generate_topics();
}

void mqtt_manager_start(void) {
    if (s_client != NULL) {
        return; // 已经启动
    }

    // 1. 读取配置
    net_config_t cfg;
    if (app_storage_load_net_config(&cfg) != ESP_OK || strlen(cfg.url) < 5) {
        ESP_LOGE(TAG, "No valid MQTT URL in storage");
        return;
    }

    ESP_LOGI(TAG, "Starting MQTT client to %s", cfg.url);

    // 2. 配置客户端
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = cfg.url,
        // 如果有用户名密码，可以在这里从 cfg 读取并赋值
        // .credentials.username = ... 
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_client){
        esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
        esp_mqtt_client_start(s_client); 
    }
    
}

void mqtt_manager_stop(void) {
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        s_connected = false;
    }
}

esp_err_t mqtt_manager_publish(const char *topic, const char *payload) {
    if (!s_connected || !s_client) {
        return ESP_FAIL;
    }
    // 如果 topic 为空，默认发到 report 主题
    const char *target_topic = topic ? topic : s_topic_report;
    
    int msg_id = esp_mqtt_client_publish(s_client, target_topic, payload, 0, 1, 0);
    return (msg_id != -1) ? ESP_OK : ESP_FAIL;
}

bool mqtt_manager_is_connected(void) {
    return s_connected;
}