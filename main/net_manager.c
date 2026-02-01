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

// 1. 注册事件监听 (不管有没有配置，先准备好监听)
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        net_event_handler, NULL, NULL));

    // 2. 初始化 TCP/IP 堆栈
    // 注意：esp_netif_init() 已在 main.c 调用，这里不需要重复
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM)); // 密码我们自己存 NVS，不让 WiFi 驱动存

    // 3. 读取 NVS 配置 (这就是您问的那段逻辑)
    net_config_t net_cfg = {0};
    esp_err_t err = app_storage_load_net_config(&net_cfg);
    
    // 4. 判断配置是否有效
    bool has_valid_config = (err == ESP_OK) && (strlen(net_cfg.url) > 5);

    if (has_valid_config) {
        ESP_LOGI(TAG, "读取到有效配置 -> Mode: %d (0:WiFi, 1:4G), URL: %s", net_cfg.mode, net_cfg.url);
        
        if (net_cfg.mode == 1) {
            // --- 4G 模式 ---
            ESP_LOGI(TAG, "进入 4G 模式...");
            // modem_4g_init(); 
            // modem_4g_start();
        } else {
            // --- Wi-Fi 模式 ---
            ESP_LOGI(TAG, "进入 Wi-Fi 模式，开始连接 SSID: %s", net_cfg.ssid);
            
            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
            ESP_ERROR_CHECK(esp_wifi_start());
            
            // 设置账号密码
            wifi_config_t wifi_config = {0};
            strncpy((char *)wifi_config.sta.ssid, net_cfg.ssid, sizeof(wifi_config.sta.ssid));
            strncpy((char *)wifi_config.sta.password, net_cfg.password, sizeof(wifi_config.sta.password));
            
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
            ESP_ERROR_CHECK(esp_wifi_connect());
        }
    } else {
        // 5. 无配置 -> 仅启动 Wi-Fi 硬件但不连接，等待 Blufi
        ESP_LOGW(TAG, "未检测到有效配置 (或 NVS 为空)，等待蓝牙配网...");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }
//     ESP_LOGI(TAG, "Initializing Net Manager...");
//     net_config_t cfg;
//     esp_err_t err = app_storage_load_net_config(&cfg);
    
//     if (err != ESP_OK) {
//         ESP_LOGW(TAG, "No net config found. Waiting for Blufi provisioning...");
//         // 这里可以选择只初始化 WiFi 但不连接
//         return;
//     }

//     ESP_LOGI(TAG, "Net Config Loaded. Mode: %d (0:WiFi, 1:4G)", cfg.mode);

//    // 2. 注册事件监听器 (监听 IP 获取事件)
//     ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
//                                                         IP_EVENT_STA_GOT_IP,
//                                                         net_event_handler,
//                                                         NULL,
//                                                         NULL));

//     // 3. 根据模式初始化网络
//     if (s_current_mode == 0) {
//         // --- 初始化 Wi-Fi ---
//         // 注意：esp_netif_init 和 esp_event_loop_create_default 通常在 main.c 里只需调一次
//         // 如果 main.c 已经调过了，这里可以根据 handle 判断是否跳过，或者为了安全起见保留
//         // 这里假设 main.c 里没有重复初始化，或者 ESP-IDF 允许重复调用(会返回错误但无害)
        
//         // 建议：把 netif 和 event_loop 的初始化保留在 main.c 的最开始，这里只做 wifi init
//         // esp_netif_create_default_wifi_sta();
        
//         // wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
//         // ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
//         // ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
//         // ESP_ERROR_CHECK(esp_wifi_start());
        
//         // // 尝试连接 (密码由底层存储管理)
//         // esp_wifi_connect();





//         esp_netif_create_default_wifi_sta();
//         wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
//         esp_wifi_init(&wifi_init_cfg);
        
//         wifi_config_t wifi_cfg = {0};
//         if (strlen(cfg.ssid) > 0) {
//             strncpy((char*)wifi_cfg.sta.ssid, cfg.ssid, sizeof(wifi_cfg.sta.ssid));
//             strncpy((char*)wifi_cfg.sta.password, cfg.password, sizeof(wifi_cfg.sta.password));
            
//             ESP_LOGI(TAG, "Connecting to WiFi: %s", cfg.ssid);
//             esp_wifi_set_mode(WIFI_MODE_STA);
//             esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
//             esp_wifi_start();
//             esp_wifi_connect();
//         } else {
//             ESP_LOGW(TAG, "WiFi mode set but no SSID configured.");
//             esp_wifi_set_mode(WIFI_MODE_STA);
//             esp_wifi_start();
//         }
//     } else {
//         // --- 初始化 4G ---
//         ESP_LOGI(TAG, "Mode is 4G (Init skipped)");
//         // modem_4g_init(); 
//     }
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