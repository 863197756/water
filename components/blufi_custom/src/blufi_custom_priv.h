#pragma once
#include "esp_log.h"
#include "esp_blufi_api.h"

// 定义组件内部使用的 Log 标签
#define BLUFI_TAG "BLUFI_CUSTOM"
#define BLUFI_INFO(fmt, ...)   ESP_LOGI(BLUFI_TAG, fmt, ##__VA_ARGS__)
#define BLUFI_ERROR(fmt, ...)  ESP_LOGE(BLUFI_TAG, fmt, ##__VA_ARGS__)

// 安全相关函数声明 (在 blufi_security.c 中实现)
void blufi_dh_negotiate_data_handler(uint8_t *data, int len, uint8_t **output_data, int *output_len, bool *need_free);
int blufi_aes_encrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len);
int blufi_aes_decrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len);
uint16_t blufi_crc_checksum(uint8_t iv8, uint8_t *data, int len);

esp_err_t blufi_security_init(void);
void blufi_security_deinit(void);

// 错误报告辅助函数
void btc_blufi_report_error(esp_blufi_error_state_t state);