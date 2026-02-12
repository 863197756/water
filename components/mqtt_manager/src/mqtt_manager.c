#include "mqtt_manager.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "app_storage.h"
#include "protocol.h"

#include "app_events.h"
#include "esp_event.h"
#include "esp_netif.h"

static const char *TAG = "MQTT_MGR";
static esp_mqtt_client_handle_t s_client = NULL;
static bool s_waiting_for_plan = false; // 新增：等待套餐下发标志

// 记录 Init 消息的 msg_id，用于确认发送完成
static int s_init_msg_id = -1;

// Topic 缓冲区
static char s_topic_init[64];
static char s_topic_cmd[64];
static char s_topic_status[64];
static char s_topic_log[64];
static char s_topic_alert[64];


// 生成所有 Topic
static void generate_topics(void) {
    char dev_id[32];
    protocol_get_device_id(dev_id, sizeof(dev_id));
    
    // 1. Init Topic (全局，无 DeviceID)
    snprintf(s_topic_init, sizeof(s_topic_init), "%s/init", PRODUCT_ID);

    // 2. 其他业务 Topic (带 DeviceID)
    snprintf(s_topic_cmd, sizeof(s_topic_cmd), "%s/%s/cmd", PRODUCT_ID, dev_id);
    snprintf(s_topic_status, sizeof(s_topic_status), "%s/%s/status", PRODUCT_ID, dev_id);
    snprintf(s_topic_log, sizeof(s_topic_log), "%s/%s/log", PRODUCT_ID, dev_id);
    snprintf(s_topic_alert, sizeof(s_topic_alert), "%s/%s/alert", PRODUCT_ID, dev_id);

    ESP_LOGI(TAG, "Init Topic: %s", s_topic_init);
    ESP_LOGI(TAG, "Cmd  Topic: %s", s_topic_cmd);
    ESP_LOGI(TAG, "Status Topic: %s", s_topic_status);
    ESP_LOGI(TAG, "Log   Topic: %s", s_topic_log);
    ESP_LOGI(TAG, "Alert Topic: %s", s_topic_alert);
}

// MQTT 事件处理
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT Connected");
        generate_topics();
        

        // 订阅指令
        esp_mqtt_client_subscribe(s_client, s_topic_cmd, 1);
        
        net_config_t cfg;
        // 如果读取失败，默认 mode=0 (WiFi)
        if (app_storage_load_net_config(&cfg) != ESP_OK) {
            cfg.mode = 0; 
        }
        
        // 【修正】初始化结构体，net_mode 留空或全0
        init_data_t init_d = {
            .fw_version = "1.0.0",
            .hw_version = "2.0",
            .net_mode = {0} // 先清零
        };

        // 【修正】使用 strcpy 手动赋值字符串到数组
        if (cfg.mode == 1) {
            strncpy(init_d.net_mode, "4G", sizeof(init_d.net_mode) - 1);
        } else {
            strncpy(init_d.net_mode, "WIFI", sizeof(init_d.net_mode) - 1);
        }

        // 获取 MAC
        protocol_get_mac_str(init_d.mac_str, sizeof(init_d.mac_str));

        char *json = protocol_pack_init(&init_d);
        if (json) {
            ESP_LOGI(TAG, "Sending Init: %s", json);
            s_init_msg_id = esp_mqtt_client_publish(s_client, s_topic_init, json, 0, 1, 0);
            free(json);
            
            // 标记等待套餐下发
            s_waiting_for_plan = true;
        }
        break;
        
    case MQTT_EVENT_PUBLISHED:
        if (event->msg_id == s_init_msg_id) {
            ESP_LOGI(TAG, "Init Published. Waiting for Cloud CMD (Plan Info)...");
            s_init_msg_id = -1;
        }
        break;

    case MQTT_EVENT_DATA:
        if (strncmp(event->topic, s_topic_cmd, event->topic_len) == 0) {
            // 处理指令
            server_cmd_t cmd;
            if (protocol_parse_cmd(event->data, event->data_len, &cmd) == ESP_OK) {
                if (s_waiting_for_plan) {
                    ESP_LOGI(TAG, "Received CMD (Plan Info). Step 4 Complete.");
                    app_events_post_mqtt_plan_received();
                    s_waiting_for_plan = false;
                }

                // 转发业务逻辑
                extern void app_logic_handle_cmd(server_cmd_t *cmd);
                app_logic_handle_cmd(&cmd);
            }
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT Error");
        // 可选：如果断开，可通过独立钩子通知上层状态
        break;
        
    default: break;
    }
}

// 封装发送函数
esp_err_t mqtt_manager_publish_status(const status_report_t *data) {
    if (!s_client) return ESP_FAIL;
    char *json = protocol_pack_status(data);
    if (!json) return ESP_FAIL;
    int msg_id = esp_mqtt_client_publish(s_client, s_topic_status, json, 0, 1, 0);
    free(json);
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t mqtt_manager_publish_log(const log_report_t *data) {
    if (!s_client) return ESP_FAIL;
    char *json = protocol_pack_log(data);
    if (!json) return ESP_FAIL;
    int msg_id = esp_mqtt_client_publish(s_client, s_topic_log, json, 0, 0, 0);
    free(json);
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t mqtt_manager_publish_alert(const alert_report_t *data) {
    if (!s_client) return ESP_FAIL;
    char *json = protocol_pack_alert(data);
    if (!json) return ESP_FAIL;
    int msg_id = esp_mqtt_client_publish(s_client, s_topic_alert, json, 0, 1, 0);
    free(json);
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}
esp_err_t mqtt_manager_publish(const char *topic, const char *payload) {
    if (!s_client) {
        ESP_LOGE(TAG, "MQTT not connected, cannot publish raw data");
        return ESP_FAIL;
    }
    
    // 如果调用者传了 NULL (比如 net_manager_send_data)，默认发到 log 主题
    const char *target_topic = (topic != NULL) ? topic : s_topic_log;
    
    // 如果 topic 还没生成（没连上过），也无法发送
    if (strlen(target_topic) == 0) {
         return ESP_FAIL;
    }

    int msg_id = esp_mqtt_client_publish(s_client, target_topic, payload, 0, 1, 0);
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}

//IP 事件回调：网络通了，我再启动
static void on_network_ip_event(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    ESP_LOGI(TAG, "检测到网络 IP 事件，正在启动 MQTT...");
    mqtt_manager_start();
}

static void on_app_event(void* arg, esp_event_base_t event_base,
                         int32_t event_id, void* event_data) {
    if (event_base == APP_EVENTS && event_id == APP_EVENT_MQTT_CONFIG_UPDATED) {
        ESP_LOGI(TAG, "检测到 MQTT 配置更新事件，重启 MQTT...");
        mqtt_manager_start();
    }
}


void mqtt_manager_init(void) {
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        on_network_ip_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_PPP_GOT_IP,
                                                        on_network_ip_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(APP_EVENTS, APP_EVENT_MQTT_CONFIG_UPDATED,
                                                        on_app_event, NULL, NULL));


    ESP_LOGI(TAG, "MQTT Manager 已初始化 (等待网络连接...)");
    
    
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
