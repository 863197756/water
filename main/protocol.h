#pragma once
#include "esp_err.h"

// --- 1. 上报数据结构体 (Device -> Cloud) ---
typedef struct {
    char device_id[32];   // 设备唯一ID
    int tds_value;        // TDS 值
    int total_flow;       // 累计流量 (L)
    int filter_life;      // 滤芯剩余 (%)
    int error_code;       // 故障码 (0=正常)
} report_data_t;

// --- 2. 下发指令结构体 (Cloud -> Device) ---
typedef struct {
    char cmd[16];         // 指令: "wash", "reset", "update"
    int param;            // 参数: 1, 0, 100...
    char str_param[64];   // 字符串参数 (如新的 URL)
} server_cmd_t;

/**
 * @brief [打包] 将设备状态打包为 JSON 字符串
 * @param data 设备状态结构体
 * @return char* JSON 字符串 (注意：使用完必须 free!)
 */
char* protocol_pack_status(const report_data_t *data);

/**
 * @brief [解析] 解析服务器下发的 JSON 指令
 * @param json_str 原始 JSON 字符串
 * @param len 字符串长度
 * @param out_cmd 输出的指令结构体
 * @return esp_err_t
 */
esp_err_t protocol_parse_cmd(const char *json_str, int len, server_cmd_t *out_cmd);