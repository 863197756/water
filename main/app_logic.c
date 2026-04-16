#include <string.h>
#include "esp_log.h"
#include "app_storage.h"
#include "esp_system.h" 
#include "protocol.h"
#include "app_logic.h"
#include "mqtt_manager.h"
#include "bsp_sensor.h"      // 引入真实的底层传感器接口
#include "app_events.h"      // 引入事件总线，用于将云端指令下发给状态机
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"

static const char *TAG = "LOGIC";

// ============================================================================
// 定时数据上报任务 (使用真实的传感器数据)
// ============================================================================
static void app_logic_report_task(void *pvParameters) {
    ESP_LOGI(TAG, "Report Task Started. Interval: 60s");

    while (1) {
        // 等待 60 秒定时周期
        vTaskDelay(pdMS_TO_TICKS(60000));

        // 从 NVS 读取最新的计费、滤芯和累计总流量
        device_status_t status;
        app_storage_load_status(&status);

        // 1. 打包并发送 Log 数据 (调用真实的传感器 ADC 读取)
        log_report_t log_data = {
            .production_vol = 0, // 定时上报时单次水量填0，单次制水量在FSM水满停机时单独结算上报
            .tds_in = bsp_sensor_get_tds_in(),       // 真实 原水 TDS
            .tds_out = bsp_sensor_get_tds_out(),     // 真实 纯水 TDS
            .tds_backup = bsp_sensor_get_tds_backup(),// 真实 备用 TDS
        };

        ESP_LOGI(TAG, "Uploading Log Data... TDS: %d | %d", log_data.tds_in, log_data.tds_out);
        if (mqtt_manager_publish_log(&log_data) == ESP_OK) {
            ESP_LOGI(TAG, "Log Upload Success");
        } else {
            ESP_LOGW(TAG, "Log Upload Failed (MQTT not ready?)");
        }
    }
}

// ============================================================================
// 主动上报 Status 数据
// ============================================================================
void app_logic_report_status(void) {
    device_status_t status;
    app_storage_load_status(&status);

    status_report_t status_data = {
        .tds_in = bsp_sensor_get_tds_in(),
        .tds_out = bsp_sensor_get_tds_out(),
        .tds_backup = bsp_sensor_get_tds_backup(),
        .total_water = status.total_flow / 1000, // 毫升转为升(L)
        
        .switch_status = status.switch_state,
        .pay_mode = status.pay_mode,
        .days = status.days,
        .capacity = status.capacity,
        .timestamp = 0, // 设为 0 时底层自动取当前时间
    };
    for (int i = 0; i < 9; i++) {
        status_data.filters[i].valid = true; // 状态全量上报
        status_data.filters[i].days = status.filter_days[i];
        status_data.filters[i].capacity = status.filter_capacity[i];
    }
    mqtt_manager_publish_status(&status_data);
    ESP_LOGI(TAG, "Status Reported");
}

// ============================================================================
// OTA 更新任务
// ============================================================================
static void ota_task(void *pvParameter) {
    char *url = (char *)pvParameter;
    ESP_LOGI(TAG, "开始执行 OTA, 下载地址: %s", url);

    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
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
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA 失败: %s", esp_err_to_name(ret));
    }

    free(url);
    vTaskDelete(NULL);
}

void app_logic_trigger_ota(const char *url) {
    char *url_copy = strdup(url); 
    if (url_copy == NULL) {
        ESP_LOGE(TAG, "内存不足，无法启动 OTA");
        return;
    }
    xTaskCreate(&ota_task, "ota_task", 8192, url_copy, 5, NULL);
}

// ============================================================================
// MQTT 云端指令分发枢纽
// ============================================================================
void app_logic_handle_cmd(server_cmd_t *cmd) {
    ESP_LOGI(TAG, "Received Cloud Command Method: %d", cmd->method);

    device_status_t status; // 用于暂存从 NVS 读取的当前状态

    switch (cmd->method) {
        case CMD_METHOD_POWER:
            ESP_LOGI(TAG, "Action: Power Switch -> %d", cmd->param.switch_status);
            // 这里不需要自己写关机代码，直接触发状态机评估，状态机会自动拦截并关断所有阀门
            // 1. 读取当前状态
            app_storage_load_status(&status);
            // 2. 修改开关机状态
            status.switch_state = cmd->param.switch_status;
            // 3. 真正保存到 Flash
            app_storage_save_status(&status);
            
            // 4. 刺激状态机更新
            esp_event_post(APP_EVENTS, APP_EVENT_CMD_EVALUATE, NULL, 0, 0); 
            break;
            
        case CMD_METHOD_RESET:
            ESP_LOGW(TAG, "Action: Reset Device");
            app_storage_erase(RESET_LEVEL_FACTORY); // 擦除数据
            esp_restart();                          // 重启设备
            break;
            
        case CMD_METHOD_UPDATE_PLAN:
            ESP_LOGI(TAG, "Action: Update Plan (Days: %d, Cap: %d)", cmd->param.days, cmd->param.capacity);
            
            // 1. 读取当前状态 (保留原有的 total_flow 制水量不被覆盖)
            app_storage_load_status(&status);
            
            // 2. 覆盖下发的套餐和滤芯参数
            status.pay_mode = cmd->param.pay_mode;
            status.days     = cmd->param.days;
            status.capacity = cmd->param.capacity;
            for (int i = 0; i < 9; i++) {
                if (cmd->filters[i].valid) {
                    status.filter_days[i] = cmd->filters[i].days;
                    status.filter_capacity[i] = cmd->filters[i].capacity;
                }
            }
            
            // 3. 真正保存到 Flash
            app_storage_save_status(&status);
            ESP_LOGI(TAG, "新套餐参数已成功写入 NVS Flash！");
            
            // 4. 通知状态机重新鉴权是否需要恢复制水
            esp_event_post(APP_EVENTS, APP_EVENT_CMD_EVALUATE, NULL, 0, 0); 
            break;
            
        case CMD_METHOD_SET_WASH:
            ESP_LOGI(TAG, "Action: Force Wash");
            // 向 FSM 抛出强制冲洗事件，剩下的时间倒计时和硬件控制交给 FSM
            esp_event_post(APP_EVENTS, APP_EVENT_CMD_START_WASH, NULL, 0, 0);
            break;
        
        case CMD_METHOD_OTA:
            ESP_LOGI(TAG, "Action: OTA Update");
            app_logic_report_status(); // OTA前也可上报一次
            app_logic_trigger_ota(cmd->param.ota_url);
            break;
            
        case CMD_METHOD_QUERY_STATUS:
            ESP_LOGI(TAG, "Action: Query Status");
            app_logic_report_status();
            break;

        default:
            ESP_LOGW(TAG, "Unknown Method: %d", cmd->method);
            break;
    }
    
    // 除重置和OTA以外，收到指令处理完成后主动上报一次最新状态
    if (cmd->method != CMD_METHOD_RESET && cmd->method != CMD_METHOD_OTA && cmd->method != CMD_METHOD_QUERY_STATUS) {
        app_logic_report_status();
    }
}

// ============================================================================
// 初始化入口
// ============================================================================
void app_logic_init(void) {
    ESP_LOGI(TAG, "App Logic Initialized");

    // 启动定时上报任务
    xTaskCreate(app_logic_report_task, "report_task", 4096, NULL, 5, NULL);
}