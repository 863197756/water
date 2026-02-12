// components/protocol/include/protocol.h
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// 产品 ID (根据文档 topic 结构: product_id/device_id/...)
#define PRODUCT_ID "01_water" 


// --- 1. 枚举定义 ---

// 下发指令的方法 (method)
typedef enum {
    CMD_METHOD_POWER       = 0, // 开关机
    CMD_METHOD_RESET       = 1, // 重置
    CMD_METHOD_UPDATE_PLAN = 2, // 更新套餐/滤芯
    CMD_METHOD_SET_WASH    = 3  // 冲洗
} cmd_method_t;

// 报警代码 (AlertCode)
typedef enum {
    ALERT_LEAKAGE      = 0, // 漏水
    ALERT_LOW_PRESSURE = 1, // 原水缺水/低压
    ALERT_TIMEOUT      = 2, // 制水超时
    ALERT_TDS_ERR      = 3, // TDS 异常
    ALERT_TEMP_ERR     = 4, // 温度异常
    ALERT_PUMP_ERR     = 5  // 水泵异常
} alert_code_t;

// --- 2. 数据结构体 ---
typedef struct {
    char fw_version[16];
    char hw_version[16];
    char net_mode[8];
    char mac_str[20];
    // UID 不需要传参，函数内部读取
} init_data_t;


// 服务器下发指令 (Command)
typedef struct {
    char cmd_id[32];      // 用于回执
    int method;           // 对应 cmd_method_t
    
    // 参数集合 (解析 param 对象)
    struct {
        int switch_status; // method=0, 0:关, 1:开
        
        // method=2 (套餐信息)
        int sale_mode;     // 0:家用, 1:售水
        int pay_mode;      // 0:计时, 1:计量
        int days;          // 剩余天数
        int capacity;      // 剩余流量
        int filter[5];     // 5级滤芯剩余天数 (filter01 - filter05)
        
        // method=3
        int wash_duration; // 冲洗时长 (秒)
    } param;
} server_cmd_t;

// 状态上报 (Status) - 主要是tds、流量、套餐
typedef struct {
    // --- 根节点字段 ---
    int tds_in;          // tdsIn
    int tds_out;         // tdsOut
    int tds_backup;      // tdsBackup
    int total_water;     // totalWater (累计用水量)

    // --- param 对象字段 ---
    int switch_status;   // param.switch
    int pay_mode;        // param.payMode
    int days;            // param.days
    int capacity;        // param.capacity
    int filter[5];       // param.filter01 - 05
} status_report_t;

// 日志上报 (Log) - 主要是制水数据
typedef struct {
    int production_vol;  // productionVol (本次制水量)
    int tds_in;          // tdsIn
    int tds_out;         // tdsOut
    int tds_backup;      // tdsBackup
} log_report_t;

// 报警上报 (Alert)
typedef struct {
    int alert_code;
    long long timestamp;
} alert_report_t;

// --- 3. 函数声明 ---

/**
 * @brief 获取 DeviceID (使用 MAC 地址的 Hex 字符串，无冒号，小写)
 * 用于 MQTT Topic: yincheng_water/{device_id}/cmd
 * 示例: "aabbccddeeff"
 */
void protocol_get_device_id(char *out_id, size_t max_len);
/**
 * @brief 获取 eFuse 中的唯一 UID (通常也是出厂 MAC，但作为硬件唯一标识)
 * 示例: "AA:BB:CC:DD:EE:FF" (大写带冒号，或者 Hex 字符串，看后端需求)
 */
void protocol_get_uid(char *out_uid, size_t max_len);
/**
 * @brief 获取格式化的 MAC 地址
 * 示例: "AA:BB:CC:DD:EE:FF"
 */
void protocol_get_mac_str(char *out_mac, size_t max_len);
// 打包函数 (生成 JSON 字符串，调用者需 free)
char* protocol_pack_init(const init_data_t *data);
char* protocol_pack_status(const status_report_t *data);
char* protocol_pack_log(const log_report_t *data);
char* protocol_pack_alert(const alert_report_t *data);

// 解析函数
esp_err_t protocol_parse_cmd(const char *json_str, int len, server_cmd_t *out_cmd);