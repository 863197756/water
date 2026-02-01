#pragma once
#include "esp_err.h"

#define NET_CONFIG_NAMESPACE "net_cfg"
#define NET_CONFIG_KEY       "config"


// 定义一些结构体方便数据交互
typedef struct {
    int mode;             // 0: WiFi, 1: 4G
    char ssid[32];        // WiFi SSID
    char password[64];    // WiFi Password
    
    // 新增 MQTT 配置字段
    char mqtt_host[64];   // 例如 "iot.example.com"
    int  mqtt_port;       // 例如 1883
    char mqtt_token[64];  // 例如 "abcdefg" (可能用于 username 或 clientID)
    
    // 兼容字段
    char url[128];        // 完整 URL, 由 host+port 拼接: "mqtt://host:port"
} net_config_t;

typedef struct {
    int total_flow;     // 总制水量
    int filter_life;    // 滤芯剩余
} device_status_t;

typedef enum {
    RESET_LEVEL_NET     = 1, // 仅重置网络 (保留滤芯数据)
    RESET_LEVEL_FACTORY = 9  // 恢复出厂 (清除所有)
} reset_level_t;


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


/**
 * @brief 执行重置操作
 * @param level 重置等级
 * @return esp_err_t
 */
esp_err_t app_storage_erase(reset_level_t level);