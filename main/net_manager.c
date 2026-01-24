// net_manager.c
#include "app_storage.h" // 引用之前定义的存储组件
#include "esp_wifi.h"
// #include "modem_4g.h" // 你的4G组件头文件

static int s_current_mode = 0; // 0:WiFi, 1:4G

void net_manager_init(void) {
    // 1. 读取配置
    net_config_t cfg;
    if (app_storage_load_net_config(&cfg) == ESP_OK) {
        s_current_mode = cfg.mode;
    }

    // 2. 根据模式初始化
    if (s_current_mode == 0) {
           // 2. 网络栈初始化 (必须先做)
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    } else {
        // 初始化 4G
        // modem_4g_init(); 
    }
}

esp_err_t net_manager_send_data(const char *payload) {
    if (s_current_mode == 0) {
        // --- Wi-Fi 发送逻辑 ---
        // 使用 esp_http_client 或 esp_mqtt_client
        // return http_send(payload);
    } else {
        // --- 4G 发送逻辑 ---
        // 调用 4G 组件的发送接口
        // return modem_4g_send_at(payload);
    }
    return ESP_FAIL;
}