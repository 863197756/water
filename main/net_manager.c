// net_manager.c
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "net_manager.h"
#include "app_storage.h" // 引用之前定义的存储组件
#include "mqtt_manager.h"

static const char *TAG = "NET_MGR";
// #include "modem_4g.h" // 你的4G组件头文件

static int s_current_mode = 0; // 0:WiFi, 1:4G

// =======================================================
// [关键修复] 定义一个事件处理回调函数
// =======================================================
static void net_event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data)
{
    // 处理 Wi-Fi 获取到 IP 的事件 (说明连网成功了)
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Wi-Fi Got IP! 网络已就绪，准备启动 MQTT...");
        
        // 网络通了，启动 MQTT 客户端
        mqtt_manager_start();
    }
    
    // 将来在这里处理 4G 的 IP_EVENT_PPP_GOT_IP 事件
}


void net_manager_init(void) {
    ESP_LOGI(TAG, "Initializing Net Manager...");
    // 1. 读取配置
    net_config_t cfg;
    if (app_storage_load_net_config(&cfg) == ESP_OK) {
        s_current_mode = cfg.mode;
    }

   // 2. 注册事件监听器 (监听 IP 获取事件)
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        net_event_handler,
                                                        NULL,
                                                        NULL));

    // 3. 根据模式初始化网络
    if (s_current_mode == 0) {
        // --- 初始化 Wi-Fi ---
        // 注意：esp_netif_init 和 esp_event_loop_create_default 通常在 main.c 里只需调一次
        // 如果 main.c 已经调过了，这里可以根据 handle 判断是否跳过，或者为了安全起见保留
        // 这里假设 main.c 里没有重复初始化，或者 ESP-IDF 允许重复调用(会返回错误但无害)
        
        // 建议：把 netif 和 event_loop 的初始化保留在 main.c 的最开始，这里只做 wifi init
        esp_netif_create_default_wifi_sta();
        
        wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        
        // 尝试连接 (密码由底层存储管理)
        esp_wifi_connect();
        
    } else {
        // --- 初始化 4G ---
        ESP_LOGI(TAG, "Mode is 4G (Init skipped)");
        // modem_4g_init(); 
    }
}

esp_err_t net_manager_send_data(const char *payload) {
    if (s_current_mode == 0) {
        // Wi-Fi 模式下，通常走 MQTT 发送
        return mqtt_manager_publish(NULL, payload);
    } else {
        // 4G 模式
        // return modem_4g_send_at(payload);
    }
    return ESP_FAIL;
}