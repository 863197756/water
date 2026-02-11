#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include "protocol.h"

/**
 * @brief 初始化 MQTT (读取配置并配置客户端，但暂不连接)
 */
void mqtt_manager_init(void);

/**
 * @brief 启动 MQTT 连接 (在 Wi-Fi 或 4G 连网成功后调用)
 */
void mqtt_manager_start(void);

/**
 * @brief 停止 MQTT (网络断开时调用)
 */
void mqtt_manager_stop(void);

esp_err_t mqtt_manager_publish_status(const status_report_t *data);

esp_err_t mqtt_manager_publish_log(const log_report_t *data);

esp_err_t mqtt_manager_publish_alert(const alert_report_t *data);
esp_err_t mqtt_manager_publish(const char *topic, const char *payload);