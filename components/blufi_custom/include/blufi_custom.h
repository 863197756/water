#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化并开启 Blufi 广播
 * 注意：调用前请确保 Wi-Fi 栈 (esp_wifi_init) 和 NVS 已初始化
 * * @return esp_err_t ESP_OK 表示成功
 */
esp_err_t blufi_custom_init(void);

/**
 * @brief 停止 Blufi 并释放资源 (通常在配网成功后调用)
 */
void blufi_custom_deinit(void);

/**
 * @brief 通过蓝牙向小程序发送反馈信息 (Custom Data)
 * * @param success true=成功, false=失败
 * @param msg 附带的消息字符串
 * 格式推荐：JSON 或 [Status Byte] + [Message]
 */
void blufi_send_custom_result(bool success, const char* msg);

// 从 NVS 读取网络配置
// mode: 输出参数
// url: 输出 buffer
// max_len: buffer 大小
esp_err_t load_net_config(int *mode, char *url, size_t max_len);


void blufi_send_mqtt_status(int status);


#ifdef __cplusplus
}
#endif