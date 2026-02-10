#include "mqtt_manager.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "app_storage.h"
#include "protocol.h"
#include "app_logic.h"
#include "blufi_custom.h" // 需要引用 blufi 来发送状态

static const char *TAG = "MQTT_MGR";
static esp_mqtt_client_handle_t s_client = NULL;

// 动态 Topic
static char s_topic_cmd[64];  // 订阅: iot_purifier/{ID}/cmd
static char s_topic_init[64]; // 发布: iot_purifier/init

// 记录 Init 消息的 msg_id，用于确认发送完成
static int s_init_msg_id = -1;

static void generate_topics(void) {
    char dev_id[32];
    protocol_get_device_id(dev_id);
    
    // 1. 订阅主题
    snprintf(s_topic_cmd, sizeof(s_topic_cmd), "iot_purifier/%s/cmd", dev_id);
    // 2. 发布主题
    snprintf(s_topic_init, sizeof(s_topic_init), "iot_purifier/init");

    ESP_LOGI(TAG, "Sub Topic: %s", s_topic_cmd);
    ESP_LOGI(TAG, "Pub Topic: %s", s_topic_init);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT Connected. Starting Init Sequence...");
        
        generate_topics();

        // 步骤 A: 订阅 CMD 主题 (QoS 2)
        esp_mqtt_client_subscribe(s_client, s_topic_cmd, 2);

        // 步骤 B: 发布 Init 消息 (QoS 2)
        char *init_payload = protocol_pack_init_data();
        if (init_payload) {
            ESP_LOGI(TAG, "Publishing Init Data: %s", init_payload);
            // 保存 msg_id 以便在 PUBLISHED 事件中校验
            s_init_msg_id = esp_mqtt_client_publish(s_client, s_topic_init, init_payload, 0, 2, 0);
            free(init_payload);
        }
        break;

    case MQTT_EVENT_PUBLISHED:
        // 步骤 C: 确认 Init 消息已发送成功
        if (event->msg_id == s_init_msg_id) {
            ESP_LOGI(TAG, "Init Message Published (MsgID=%d). MQTT Ready!", event->msg_id);
            
            // * 关键：通知 Blufi 发送 {"statusMQTT": 0} *
            blufi_send_mqtt_status(0); 
            
            s_init_msg_id = -1; // 重置
        }
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "Recv Data on: %.*s", event->topic_len, event->topic);
        // 处理下发指令 (复用之前的逻辑)
        // server_cmd_t cmd;
        // if (protocol_parse_cmd(...) == ESP_OK) { app_logic_handle_cmd(&cmd); }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT Error: %d", event->error_handle->error_type);
        // 如果连接失败，也可以通知 Blufi 发送 statusMQTT: 1
        // blufi_send_mqtt_status(1); 
        break;
        
    default: break;
    }
}

void mqtt_manager_init(void) {
    // 可以在这里预生成 ID，或者留到 Connected
}

void mqtt_manager_start(void) {
    if (s_client != NULL) {
        mqtt_manager_stop(); // 如果之前有连接，先停止
    }

    net_config_t cfg;
    if (app_storage_load_net_config(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "No MQTT Config found");
        return;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = cfg.full_url, // mqtt://ip:port
        .credentials.username = cfg.username,
        .credentials.authentication.password = cfg.password_mqtt,
        // 如果需要客户端证书，在此处添加
    };

    ESP_LOGI(TAG, "Connecting MQTT: %s, User: %s", cfg.full_url, cfg.username);

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);
}

void mqtt_manager_stop(void) {
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
}