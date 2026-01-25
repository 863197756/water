#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "app_storage.h"  
#include "blufi_custom.h"
#include "protocol.h"



// 未实现的组件
// #include "bsp_pump_valve.h" 
// #include "modem_4g.h"

static const char *TAG = "MAIN";


#define GPIO_KEY_RESET 0 // 假设 BOOT 键

void key_scan_task(void *arg);
void test_protocol_function(void);

void app_main(void)
{


    
   
    // 存储与系统初始化
    ESP_ERROR_CHECK(app_storage_init());

    // app_storage_erase(RESET_LEVEL_NET);


    // 网络基础设施初始化
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // test_protocol_function();
    

    // 读取配置，决定启动模式
    net_config_t net_cfg = {0};
    
    // 尝试读取配置
    if (app_storage_load_net_config(&net_cfg) == ESP_OK) {
        ESP_LOGI(TAG, "读取到配置 -> Mode: %d (0:WiFi, 1:4G), URL: %s", net_cfg.mode, net_cfg.url);
        
        // 简单检查配置是否有效
        if (strlen(net_cfg.url) > 5) {
            if (net_cfg.mode == 1) {
                // --- 4G 模式 ---
                ESP_LOGI(TAG, "进入 4G 模式...");
                // modem_4g_init(); // 暂未实现
            } else {
                // --- Wi-Fi 模式 ---
                ESP_LOGI(TAG, "进入 Wi-Fi 模式，开始连接...");
                ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
                ESP_ERROR_CHECK(esp_wifi_start());
                
                // 尝试连接 (Blufi 配网时已将 SSID/PWD 存入底层 NVS，这里直接 Connect 即可)
                esp_err_t conn_ret = esp_wifi_connect();
                if (conn_ret != ESP_OK) {
                    ESP_LOGE(TAG, "Wi-Fi 连接启动失败: %s", esp_err_to_name(conn_ret));
                }
            }
            return; // 业务启动后退出
        }
    }

    // 4. 无配置 -> 启动配网
    ESP_LOGI(TAG, "未检测到有效配置，启动 Blufi 配网模式...");
    blufi_custom_init();
}


void test_protocol_function(void) {
    // --- 测试打包 ---
    report_data_t report = {
        .tds_value = 50,
        .total_flow = 1200,
        .filter_life = 95,
        .error_code = 0
    };
    strcpy(report.device_id, "TEST_DEV_001");

    char *json_out = protocol_pack_status(&report);
    if (json_out) {
        ESP_LOGI("TEST", "Generated JSON: %s", json_out);
        free(json_out); // 记得释放！
    }

    // --- 测试解析 ---
    const char *test_json = "{\"cmd\": \"wash\", \"param\": 15}";
    server_cmd_t cmd_out;
    
    if (protocol_parse_cmd(test_json, strlen(test_json), &cmd_out) == ESP_OK) {
        ESP_LOGI("TEST", "Parsed CMD: %s, Param: %d", cmd_out.cmd, cmd_out.param);
    } else {
        ESP_LOGE("TEST", "Parse failed");
    }
}



// 按键重置等任务
void key_scan_task(void *arg) {
    int press_time = 0;
    while (1) {
        if (gpio_get_level(GPIO_KEY_RESET) == 0) { // 按下
            press_time++;
            if (press_time == 30) { // 3秒 (假设 100ms 扫一次)
                ESP_LOGI("KEY", "长按3秒：触发配网重置");
                // 可以在这里让 LED 快闪提示用户
            }
            if (press_time == 100) { // 10秒
                ESP_LOGI("KEY", "长按10秒：触发恢复出厂");
                // 可以在这里让 LED 变红提示用户
            }
        } else {
            // 抬起时判断
            if (press_time > 100) {
                app_storage_erase(RESET_LEVEL_FACTORY);
                esp_restart();
            } else if (press_time > 30) {
                app_storage_erase(RESET_LEVEL_NET);
                esp_restart();
            }
            press_time = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}