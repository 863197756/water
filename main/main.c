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
#include "bsp_pump_valve.h"
#include "bsp_sensor.h"
#include "bsp_led.h"


static const char *TAG = "MAIN";




void test_protocol_function(void);



static void led_hardware_test_task(void *pvParameters) {
    ESP_LOGW("LED_TEST", "=================================");
    ESP_LOGW("LED_TEST", "  启动 LED 自动化点灯测试序列");
    ESP_LOGW("LED_TEST", "=================================");

    while (1) {
        // --- 测试 1: 独立状态灯 (LED1~4) 常亮 ---
        ESP_LOGI("LED_TEST", ">> 测试 1: 冲洗、缺水、水满、漏水 (常亮)");
        bsp_led_set_mode(LED_MODE_STANDBY); // 关闭流水灯
        bsp_led_set_alarm(1, LED_ALARM_ON);
        bsp_led_set_alarm(2, LED_ALARM_ON);
        bsp_led_set_alarm(3, LED_ALARM_ON);
        bsp_led_set_alarm(4, LED_ALARM_ON);
        vTaskDelay(pdMS_TO_TICKS(3000));

        // --- 测试 2: 独立状态灯 (LED1~4) 闪烁 ---
        ESP_LOGI("LED_TEST", ">> 测试 2: 冲洗、缺水、水满、漏水 (闪烁)");
        bsp_led_set_alarm(1, LED_ALARM_BLINK);
        bsp_led_set_alarm(2, LED_ALARM_BLINK);
        bsp_led_set_alarm(3, LED_ALARM_BLINK);
        bsp_led_set_alarm(4, LED_ALARM_BLINK);
        vTaskDelay(pdMS_TO_TICKS(4000));

        // 清理状态灯，准备测试业务模式
        for(int i=1; i<=4; i++) bsp_led_set_alarm(i, LED_ALARM_OFF);

        // --- 测试 3: 配网模式 (全亮+闪烁) ---
        ESP_LOGI("LED_TEST", ">> 测试 3: 配网模式 (左右流水灯全亮并闪烁)");
        bsp_led_set_mode(LED_MODE_NETWORKING);
        vTaskDelay(pdMS_TO_TICKS(4000));

        // --- 测试 4: 正常制水模式 (跑马灯 + 双色灯) ---
        ESP_LOGI("LED_TEST", ">> 测试 4: 制水模式 (左右流水动画，LED13双色交替)");
        bsp_led_set_mode(LED_MODE_MAKING_WATER);
        vTaskDelay(pdMS_TO_TICKS(6000));

        // --- 测试 5: 余额不足 (左流水灯亮) ---
        ESP_LOGI("LED_TEST", ">> 测试 5: 余额不足 (左侧 LED5~8 常亮)");
        bsp_led_set_mode(LED_MODE_LOW_BALANCE);
        vTaskDelay(pdMS_TO_TICKS(3000));

        // --- 测试 6: 滤芯到期 (右流水灯亮) ---
        ESP_LOGI("LED_TEST", ">> 测试 6: 滤芯到期 (右侧 LED9~12 常亮)");
        bsp_led_set_mode(LED_MODE_FILTER_EXPIRE);
        vTaskDelay(pdMS_TO_TICKS(3000));
        
        // --- 测试 7: 待机全灭 ---
        ESP_LOGI("LED_TEST", ">> 测试 7: 待机 (全灭)");
        bsp_led_set_mode(LED_MODE_STANDBY);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}


void app_main(void)
{
    // esp_log_level_set("wifi", ESP_LOG_ERROR);
    // esp_log_level_set("BLUFI_EXAMPLE", ESP_LOG_ERROR);  
    // esp_log_level_set("NimBLE", ESP_LOG_ERROR);  
    // esp_log_level_set("wifi_init", ESP_LOG_ERROR);  

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

    bsp_pump_valve_init(); 
    bsp_sensor_init();

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

    // led
    bsp_led_init();
    

}


