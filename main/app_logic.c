
#include <string.h>
#include "esp_log.h"
#include "app_storage.h"
#include "esp_system.h" 
#include "protocol.h"
#include "app_logic.h"
#include "mqtt_manager.h"
#include "bsp_pump_valve.h" // 引用驱动
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"


static const char *TAG = "LOGIC";

// 配置参数
#define FLUSH_DURATION_SEC     18   // 冲洗时长 18秒
#define MAKE_WATER_FLUSH_LIMIT 3600 // 制水累计多少秒后触发冲洗 (例如 1小时)

static TimerHandle_t s_flush_timer = NULL;
static int s_total_make_water_time = 0; // 累计制水时间(秒)
static bool s_is_flushing = false;

// 冲洗结束回调
static void flush_timer_callback(TimerHandle_t xTimer) {
    ESP_LOGI(TAG, "Flush Finished. Closing valves.");
    s_is_flushing = false;
    
    // 关闭冲洗阀，恢复正常状态 (或关机)
    bsp_set_flush_valve(false); 
    bsp_set_pump(false);
    // bsp_set_inlet_valve(false); // 进水阀视情况而定
}
/**
 * @brief 执行冲洗逻辑
 * @param reason 冲洗原因 (0:开机, 1:制水累积, 2:云端指令)
 */
void app_logic_start_flush(int reason) {
    if (s_is_flushing) {
        ESP_LOGW(TAG, "Already flushing, ignore request.");
        return;
    }

    ESP_LOGI(TAG, "Start Flushing... Reason: %d", reason);
    s_is_flushing = true;

    // 动作：开进水阀、开冲洗阀、开泵
    bsp_set_inlet_valve(true);
    bsp_set_flush_valve(true);
    vTaskDelay(pdMS_TO_TICKS(500)); // 延时保护
    bsp_set_pump(true);

    // 启动定时器自动停止
    if (s_flush_timer == NULL) {
        s_flush_timer = xTimerCreate("FlushTmr", pdMS_TO_TICKS(FLUSH_DURATION_SEC * 1000), 
                                     pdFALSE, NULL, flush_timer_callback);
    }
    xTimerReset(s_flush_timer, 0);
    
    // 如果是制水累积触发的，清零计数器
    if (reason == 1) {
        s_total_make_water_time = 0;
    }
}

// 模拟：制水过程每秒调用一次
void app_logic_making_water_tick(void) {
    s_total_make_water_time++;
    if (s_total_make_water_time >= MAKE_WATER_FLUSH_LIMIT) {
        ESP_LOGI(TAG, "Accumulated water time reached limit. Trigger flush.");
        // 停止制水，开始冲洗
        // ... 停止制水逻辑 ...
        app_logic_start_flush(1);
    }
}


// --- 【新增】模拟获取传感器数据 (后续替换为 bsp_sensor 读取) ---
static int get_tds_raw(void) { return 148; }  // 模拟原水 TDS
static int get_tds_pure(void) { return 12; }  // 模拟净水 TDS
static int get_total_water(void) { return 12345; } // 模拟累计流量
static int get_tds_backup(void) { return 10; }

// --- 【新增】定时上报任务 ---
static void app_logic_report_task(void *pvParameters) {
    ESP_LOGI(TAG, "Report Task Started. Interval: 60s");

    while (1) {
        // 1. 等待 60 秒 (根据需求调整时间)
        vTaskDelay(pdMS_TO_TICKS(60000));

        // 2. 准备 Log 数据 (制水信息)
        log_report_t log_data = {
            .production_vol = 150,        // 本次制水量 (ml)
            .tds_in = get_tds_raw(),
            .tds_out = get_tds_pure(),
            .tds_backup = get_tds_backup(),
        };


        // 3. 发送 Log 到 MQTT
        ESP_LOGI(TAG, "Uploading Log Data...");
        if (mqtt_manager_publish_log(&log_data) == ESP_OK) {
            ESP_LOGI(TAG, "Log Upload Success");
        } else {
            ESP_LOGW(TAG, "Log Upload Failed (MQTT not ready?)");
        }

        // // 4. (可选) 顺便上报 Status (状态信息: 滤芯、套餐)
        // status_report_t status_data = {
        //     // 根节点数据
        //     .tds_in = get_tds_raw(),
        //     .tds_out = get_tds_pure(),
        //     .tds_backup = get_tds_backup(),
        //     .total_water = get_total_water(), // 累计流量
            
        //     // Param 数据
        //     .switch_status = 1, // 开机
        //     .pay_mode = 0,      // 计时
        //     .days = 365,
        //     .capacity = 0,
        //     .filter = {100, 100, 100, 365, 180} // 示例寿命
        // };
        // mqtt_manager_publish_status(&status_data);
    }
}



// OTA 任务
static void ota_task(void *pvParameter) {
    char *url = (char *)pvParameter;
    ESP_LOGI(TAG, "开始执行 OTA, 下载地址: %s", url);

    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach, // 使用系统自带的证书包校验 HTTPS
        .timeout_ms = 10000,
        .keep_alive_enable = true,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    ESP_LOGI(TAG, "正在下载固件并烧录...");
    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA 成功！准备重启...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart(); // 烧录成功，主动重启
    } else {
        ESP_LOGE(TAG, "OTA 失败: %s", esp_err_to_name(ret));
        // TODO: 可以通过 MQTT 上报 OTA 失败的 Alert
    }

    free(url); // 释放参数内存
    vTaskDelete(NULL);
}

// 供 MQTT 收到指令后调用的触发函数
void app_logic_trigger_ota(const char *url) {
    // 拷贝 URL，防止指针在任务外被释放
    char *url_copy = strdup(url); 
    if (url_copy == NULL) {
        ESP_LOGE(TAG, "内存不足，无法启动 OTA 任务");
        return;
    }
    xTaskCreate(&ota_task, "ota_task", 8192, url_copy, 5, NULL);
}






void app_logic_init(void) {
    // 可以在这里创建制水任务
    ESP_LOGI(TAG, "App Logic Initialized");

    bsp_pump_valve_init();

    // 1. 开机自动冲洗
    app_logic_start_flush(0);

    // 栈大小 4096 字节，优先级 5
    xTaskCreate(app_logic_report_task, "report_task", 4096, NULL, 5, NULL);

}

void app_logic_handle_cmd(server_cmd_t *cmd) {
    ESP_LOGI(TAG, "Received Method: %d", cmd->method);

    switch (cmd->method) {
        case CMD_METHOD_POWER:
            ESP_LOGI(TAG, "Action: Power Switch -> %d", cmd->param.switch_status);
            // TODO: 控制设备待机/开机
            break;
            
        case CMD_METHOD_RESET:
            ESP_LOGW(TAG, "Action: Reset Device");
            // 建议：重置 NVS 或重启
            break;
            
        case CMD_METHOD_UPDATE_PLAN:
            ESP_LOGI(TAG, "Action: Update Plan");
            ESP_LOGI(TAG, "  Days: %d, Capacity: %d", cmd->param.days, cmd->param.capacity);
            ESP_LOGI(TAG, "  Filter1: %d, Filter5: %d", cmd->param.filter[0], cmd->param.filter[4]);
            // TODO: 保存到 NVS (app_storage_save_status)
            break;
            
        case CMD_METHOD_SET_WASH:
            ESP_LOGI(TAG, "Action: Force Wash");
            // TODO: 调用 bsp_pump_valve 开启冲洗
            break;
        
        case CMD_METHOD_OTA:
            ESP_LOGI(TAG, "Action: OTA Update");
            app_logic_trigger_ota(cmd->param.ota_url);
            break;
            
        default:
            ESP_LOGW(TAG, "Unknown Method: %d", cmd->method);
            break;
    }
}



// TODO 待完善
