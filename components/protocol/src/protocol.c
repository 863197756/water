#include "protocol.h"
#include "cJSON.h"          // 官方组件，用于生成
#include "json_parser.h"    // 你的组件，用于解析
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include "esp_mac.h"
#include <sys/time.h>
#include "esp_efuse.h"
#include "esp_efuse_table.h"

static const char *TAG = "PROTO";

// 获取毫秒级时间戳
static long long get_timestamp_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void protocol_get_device_id(char *out_id, size_t max_len) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    // 使用 MAC 地址 hex 字符串作为 Device ID
    snprintf(out_id, 13, "%02x%02x%02x%02x%02x%02x", 
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
// 2. UID = eFuse Base MAC (这里演示为 Hex 字符串)
void protocol_get_uid(char *out_uid, size_t max_len) {
    uint8_t mac[6];
    // 读取出厂烧录在 eFuse 中的 MAC，不受软件修改影响
    esp_efuse_mac_get_default(mac); 
    snprintf(out_uid, max_len, "%02x%02x%02x%02x%02x%02x", 
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// 3. MAC String (AA:BB:CC...)
void protocol_get_mac_str(char *out_mac, size_t max_len) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out_mac, max_len, "%02X:%02X:%02X:%02X:%02X:%02X", 
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
// 1. 打包 Init
// --- Init 包打包 (UID + MAC + DeviceID) ---
char* protocol_pack_init(const init_data_t *data) {
    cJSON *root = cJSON_CreateObject();
    
    char uid[32];
    protocol_get_uid(uid, sizeof(uid));
    

    cJSON_AddStringToObject(root, "uid", uid);           // eFuse UID
    cJSON_AddStringToObject(root, "fwVersion", data->fw_version);
    cJSON_AddStringToObject(root, "hwVersion", data->hw_version);
    cJSON_AddStringToObject(root, "mac", data->mac_str); // 格式化 MAC
    cJSON_AddStringToObject(root, "netMode", data->net_mode); // "WIFI" or "4G"
    cJSON_AddNumberToObject(root, "timestamp", (double)get_timestamp_ms());

    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return str;
}

// 2. 打包 Status (套餐、滤芯)
char* protocol_pack_status(const status_report_t *data) {
    cJSON *root = cJSON_CreateObject();
    
    // 参数对象
    // 1. 根节点字段
    cJSON_AddNumberToObject(root, "tdsIn", data->tds_in);
    cJSON_AddNumberToObject(root, "tdsOut", data->tds_out);
    cJSON_AddNumberToObject(root, "tdsBackup", data->tds_backup);
    cJSON_AddNumberToObject(root, "totalWater", data->total_water);

    // 2. param 对象
    cJSON *param = cJSON_CreateObject();
    cJSON_AddNumberToObject(param, "switch", data->switch_status);
    cJSON_AddNumberToObject(param, "payMode", data->pay_mode);
    cJSON_AddNumberToObject(param, "days", data->days);
    cJSON_AddNumberToObject(param, "capacity", data->capacity);
    // 滤芯 01-05
    char key[16];
    for(int i=0; i<5; i++) {
        snprintf(key, sizeof(key), "filter%02d", i+1);
        cJSON_AddNumberToObject(param, key, data->filter[i]);
    }
    
    cJSON_AddItemToObject(root, "param", param);
    // cJSON_AddNumberToObject(root, "timestamp", (double)get_timestamp_ms());

    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return str;
}

// 3. 打包 Log (制水数据)
char* protocol_pack_log(const log_report_t *data) {
    cJSON *root = cJSON_CreateObject();

    cJSON_AddNumberToObject(root, "timestamp", (double)get_timestamp_ms());
    cJSON_AddNumberToObject(root, "productionVol", data->production_vol);
    cJSON_AddNumberToObject(root, "tdsIn", data->tds_in);
    cJSON_AddNumberToObject(root, "tdsOut", data->tds_out);
    cJSON_AddNumberToObject(root, "tdsBackup", data->tds_backup);

    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return str;
}

// 4. 打包 Alert
char* protocol_pack_alert(const alert_report_t *data) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "AlertCode", data->alert_code);
    cJSON_AddNumberToObject(root, "timestamp", (double)get_timestamp_ms());
    
    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return str;
}

// 5. 解析指令
esp_err_t protocol_parse_cmd(const char *json_str, int len, server_cmd_t *out_cmd) {
    if (!json_str || len <= 0 || !out_cmd) return ESP_ERR_INVALID_ARG;

    jparse_ctx_t jctx;
    // 1. 初始化解析器
    if (json_parse_start(&jctx, json_str, len) != 0) {
        ESP_LOGE(TAG, "JSON Parse Start Failed");
        return ESP_FAIL;
    }

    memset(out_cmd, 0, sizeof(server_cmd_t));

    // 2. 先解析第一层 (根节点) 字段
    // 【关键】因为稍后会进入 param 内部，所以外层字段必须在此刻先读取完毕！
    if (json_obj_get_string(&jctx, "cmdId", out_cmd->cmd_id, sizeof(out_cmd->cmd_id)) != 0) {
        out_cmd->cmd_id[0] = '\0';
    }

    int val = 0;
    if (json_obj_get_int(&jctx, "method", &val) == 0) {
        out_cmd->method = val;
    }

    // OTA 下载链接在某些协议定义下可能放在第一层
    if (out_cmd->method == CMD_METHOD_OTA) {
        json_obj_get_string(&jctx, "url", out_cmd->param.ota_url, sizeof(out_cmd->param.ota_url));
    }

    // 3. 进入嵌套对象 param
    // 注意：这个函数只有两个参数，执行成功后，jctx 的指针会“进入” param 内部
    if (json_obj_get_object(&jctx, "param") == 0) {
        
        // 现在 jctx 的上下文已经在 param 里面了，直接读取内部字段
        if (json_obj_get_int(&jctx, "switch", &val) == 0) out_cmd->param.switch_status = val;
        if (json_obj_get_int(&jctx, "saleMode", &val) == 0) out_cmd->param.sale_mode = val;
        if (json_obj_get_int(&jctx, "payMode", &val) == 0) out_cmd->param.pay_mode = val;
        if (json_obj_get_int(&jctx, "days", &val) == 0) out_cmd->param.days = val;
        if (json_obj_get_int(&jctx, "capacity", &val) == 0) out_cmd->param.capacity = val;

        // 如果 OTA url 是放在 param 里面的，在这里再尝试获取一次
        if (out_cmd->method == CMD_METHOD_OTA && strlen(out_cmd->param.ota_url) == 0) {
            json_obj_get_string(&jctx, "url", out_cmd->param.ota_url, sizeof(out_cmd->param.ota_url));
        }

        // 解析滤芯 filter01 - filter05
        char key[16];
        for (int i = 0; i < 5; i++) {
            snprintf(key, sizeof(key), "filter%02d", i + 1);
            if (json_obj_get_int(&jctx, key, &val) == 0) {
                out_cmd->param.filter[i] = val;
            }
        }
        
        // 如果底层组件有提供退出对象的接口 (如 json_obj_leave_object(&jctx)) 可以调用，
        // 但对于单次单向解析，直接走到 json_parse_end 也是安全的。
    }

    // 4. 结束解析，释放资源
    json_parse_end(&jctx);
    return ESP_OK;
}
