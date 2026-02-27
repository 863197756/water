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
#include "time_manager.h"
#include "net_manager.h"
#include "app_logic.h"
#include "mqtt_manager.h"
#include "app_fsm.h"


// 未实现的组件
// #include "bsp_pump_valve.h" 
// #include "modem_4g.h"

static const char *TAG = "MAIN";


#define GPIO_KEY_RESET 0 // 假设 BOOT 键

void key_scan_task(void *arg);
void test_protocol_function(void);

void app_main(void)
{
    esp_log_level_set("wifi", ESP_LOG_ERROR);
    esp_log_level_set("BLUFI_EXAMPLE", ESP_LOG_ERROR);  
    esp_log_level_set("NimBLE", ESP_LOG_ERROR);  
    esp_log_level_set("wifi_init", ESP_LOG_ERROR);  

    // ESP_LOGW(TAG, "=========================================");
    // ESP_LOGW(TAG, "  这是通过 OTA 升级后的 V2.0 版本！ ");
    // ESP_LOGW(TAG, "=========================================");
    
   
    // 存储与系统初始化
    ESP_ERROR_CHECK(app_storage_init());

    // 注意：不要在每次启动时清空网络配置。
    // 网络/出厂重置应由按键或云端命令触发。
    // app_storage_erase(RESET_LEVEL_NET);


    // 网络基础设施初始化
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 启动连接状态机（统一编排网络 / MQTT 生命周期）
    app_fsm_init();

    // 启动 Blufi (蓝牙配网)
    // 即使有网也启动，方便随时重新配置。内部会自动处理蓝牙协议栈初始化。
    ESP_ERROR_CHECK(blufi_custom_init());
    // MQTT 管理器由状态机按事件触发 Start/Stop
    mqtt_manager_init();
    // 3. 启动网络管理器
    // 它会读取 NVS，尝试连接 WiFi 或 4G。
    // 如果 NVS 为空，它会待机，等待 Blufi 配网写入配置。
    net_manager_init();

    // 4. 启动应用逻辑
    // 它会根据配置，初始化传感器、控制板等组件。
    app_logic_init();
    
    
 



    // test_protocol_function();
    time_manager_init();



    // // 读取配置，决定启动模式
    // net_config_t net_cfg = {0};
    
    // // 尝试读取配置
    // if (app_storage_load_net_config(&net_cfg) == ESP_OK) {
    //     ESP_LOGI(TAG, "读取到配置 -> Mode: %d (0:WiFi, 1:4G), URL: %s", net_cfg.mode, net_cfg.url);
        
    //     // 简单检查配置是否有效
    //     if (strlen(net_cfg.url) > 5) {
    //         if (net_cfg.mode == 1) {
    //             // --- 4G 模式 ---
    //             ESP_LOGI(TAG, "进入 4G 模式...");
    //             // modem_4g_init(); // 暂未实现
    //         } else {
    //             // --- Wi-Fi 模式 ---
    //             ESP_LOGI(TAG, "进入 Wi-Fi 模式，开始连接...");
    //             ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    //             ESP_ERROR_CHECK(esp_wifi_start());
                
    //             // 尝试连接 (Blufi 配网时已将 SSID/PWD 存入底层 NVS，这里直接 Connect 即可)
    //             esp_err_t conn_ret = esp_wifi_connect();
    //             if (conn_ret != ESP_OK) {
    //                 ESP_LOGE(TAG, "Wi-Fi 连接启动失败: %s", esp_err_to_name(conn_ret));
    //             }
    //         }
    //         char time_str[32];
    //         time_manager_get_time_str(time_str, sizeof(time_str));
    //         ESP_LOGI(TAG, "当前时间: %s", time_str);    
    //         return; // 业务启动后退出
    //     }
    // }

    // // 4. 无配置 -> 启动配网
    // ESP_LOGI(TAG, "未检测到有效配置，启动 Blufi 配网模式...");
    // blufi_custom_init();
}


// void test_protocol_function(void) {
//     // --- 测试打包 ---
//     report_data_t report = {
//         .tds_value = 50,
//         .total_flow = 1200,
//         .filter_life = 95,
//         .error_code = 0
//     };
//     strcpy(report.device_id, "TEST_DEV_001");

//     char *json_out = protocol_pack_status(&report);
//     if (json_out) {
//         ESP_LOGI("TEST", "Generated JSON: %s", json_out);
//         free(json_out); // 记得释放！
//     }

//     // --- 测试解析 ---
//     const char *test_json = "{\"cmd\": \"wash\", \"param\": 15}";
//     server_cmd_t cmd_out;
    
//     if (protocol_parse_cmd(test_json, strlen(test_json), &cmd_out) == ESP_OK) {
//         ESP_LOGI("TEST", "Parsed CMD: %s, Param: %d", cmd_out.cmd, cmd_out.param);
//     } else {
//         ESP_LOGE("TEST", "Parse failed");
//     }
// }



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
