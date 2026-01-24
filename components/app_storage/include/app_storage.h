#pragma once
#include "esp_err.h"

// 定义一些结构体方便数据交互
typedef struct {
    int mode;           // 0: WiFi, 1: 4G
    char url[64];       // 服务器地址
} net_config_t;

typedef struct {
    int total_flow;     // 总制水量
    int filter_life;    // 滤芯剩余
} device_status_t;

/**
 * @brief 初始化 NVS (在 app_main 最开始调用)
 */
esp_err_t app_storage_init(void);

/**
 * @brief 网络配置相关
 */
esp_err_t app_storage_save_net_config(const net_config_t *cfg);
esp_err_t app_storage_load_net_config(net_config_t *cfg);

/**
 * @brief 设备状态相关 (制水量、滤芯等)
 */
esp_err_t app_storage_save_status(const device_status_t *status);
esp_err_t app_storage_load_status(device_status_t *status);

/**
 * @brief 记录用户离线操作 (追加模式，简单示例)
 * @param action 操作字符串，如 "start_wash"
 */
esp_err_t app_storage_log_action(const char *action);