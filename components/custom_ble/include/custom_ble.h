#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================
// 自定义蓝牙通信协议 UUID (请确保与微信小程序端一致)
// =============================================================
// 主服务 UUID
#define CUSTOM_BLE_SVC_UUID         0xAAAA
// 小程序发指令给 ESP32 的通道 (Write)
#define CUSTOM_BLE_CHR_RX_UUID      0xBBBB
// ESP32 主动推送状态给小程序的通道 (Notify)
#define CUSTOM_BLE_CHR_TX_UUID      0xCCCC

/**
 * @brief 初始化自定义 BLE GATT 服务
 * @note 请在 nimble_port_freertos_init 之后，或者 sync_cb 中调用
 */
void custom_ble_gatts_init(void);

/**
 * @brief 向微信小程序发送 JSON 数据
 * @param json_str 要发送的 JSON 字符串
 * @return 0 成功，非 0 失败
 */
int custom_ble_send_notify(const char *json_str);

#ifdef __cplusplus
}
#endif