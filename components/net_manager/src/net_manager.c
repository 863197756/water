// net_manager.c
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "net_manager.h"
#include "app_storage.h" // 引用之前定义的存储组件
#include "mqtt_manager.h"

static const char *TAG = "NET_MGR";
#include "modem_4g.h" // 你的4G组件头文件

static int s_current_mode = 0; // 0:WiFi, 1:4G

// =======================================================
// [关键修复] 定义一个事件处理回调函数
// =======================================================
static void net_event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data)
{
    // 处理 Wi-Fi 获取到 IP 的事件 (说明连网成功了)
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Wi-Fi Got IP! 网络已就绪,IP事件已广播");
        
        // 网络通了，启动 MQTT 客户端
        // mqtt_manager_start();
    }// 情况 B: 4G (PPP) 连上了
    else if (event_base == IP_EVENT && event_id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "4G PPP Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        
        // 注意：MQTT Manager 目前只监听了 IP_EVENT_STA_GOT_IP
        // 你需要去 mqtt_manager.c 里，把 IP_EVENT_PPP_GOT_IP 也加到监听列表里！
        // 或者，我们可以这里手动触发一个通用事件，但修改 MQTT 监听是最干净的。
    }
    
    // 将来在这里处理 4G 的 IP_EVENT_PPP_GOT_IP 事件
}


void net_manager_init(void) {

// 1. 注册事件监听 (不管有没有配置，先准备好监听)
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        net_event_handler, NULL, NULL));

    // 注册 4G 事件监听 (IP_EVENT_PPP_GOT_IP)
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_PPP_GOT_IP,
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
    bool has_valid_config = (err == ESP_OK) && (strlen(net_cfg.full_url) > 5);

    if (has_valid_config) {
        ESP_LOGI(TAG, "读取到有效配置 -> Mode: %d (0:WiFi, 1:4G), URL: %s", net_cfg.mode, net_cfg.full_url);
        
        if (net_cfg.mode == 1) {
            // --- 4G 模式 ---
            ESP_LOGI(TAG, "进入 4G 模式...");
            modem_4g_init(); 
            modem_4g_start();
            s_current_mode = 1;
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
}
void net_manager_set_mode(int mode) {
    s_current_mode = mode;
    if (mode == 1) {
        // 切换到 4G
        ESP_LOGI(TAG, "Switching to 4G Mode...");
        esp_wifi_stop(); // 关闭 WiFi 节省资源
        modem_4g_init(); // 初始化模组
        modem_4g_start(); // 开始拨号
    } else {
        // 切换到 WiFi (Blufi 会自动处理 WiFi 连接，这里主要是状态标记)
        ESP_LOGI(TAG, "Switching to WiFi Mode...");
        modem_4g_stop();
        // WiFi 由 Blufi 或 NVS 启动
    }
}
esp_err_t net_manager_send_data(const char *payload) {
        return mqtt_manager_publish(NULL, payload);
    

    return ESP_FAIL;
}