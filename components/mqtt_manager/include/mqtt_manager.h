#pragma once
#include "esp_err.h"
#include <stdbool.h>

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

/**
 * @brief 发布消息
 * @param topic 主题 (如果传 NULL，默认发到上报主题)
 * @param payload 数据内容 JSON
 * @return esp_err_t
 */
esp_err_t mqtt_manager_publish(const char *topic, const char *payload);

/**
 * @brief 检查 MQTT 是否已连接
 */
bool mqtt_manager_is_connected(void);