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
#include "app_storage.h"

static const char *TAG = "PROTO";

// 获取毫秒级时间戳
static long long get_timestamp_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void protocol_get_device_id(char *out_id, size_t max_len) {
    if (!out_id || max_len == 0) return;
    out_id[0] = 0;
    if (app_storage_get_sn(out_id, max_len) == ESP_OK && out_id[0]) {
        return;
    }
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out_id, max_len, "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
void protocol_get_uid(char *out_uid, size_t max_len) {
    if (!out_uid || max_len == 0) return;
    out_uid[0] = 0;
    if (app_storage_get_sn(out_uid, max_len) == ESP_OK && out_uid[0]) {
        return;
    }
    uint8_t mac[6];
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
    

    cJSON_AddStringToObject(root, "uid", uid);
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
    if (data->timestamp > 0) {
        cJSON_AddNumberToObject(root, "timestamp", (double)data->timestamp);
    } else {
        cJSON_AddNumberToObject(root, "timestamp", (double)get_timestamp_ms());
    }
    cJSON_AddNumberToObject(root, "tdsIn", data->tds_in);
    cJSON_AddNumberToObject(root, "tdsOut", data->tds_out);
    cJSON_AddNumberToObject(root, "tdsBackup", data->tds_backup);
    cJSON_AddNumberToObject(root, "totalWater", data->total_water);

    // 2. currentStatus 对象
    cJSON *currentStatus = cJSON_CreateObject();
    cJSON_AddNumberToObject(currentStatus, "switch", data->switch_status);
    cJSON_AddNumberToObject(currentStatus, "saleMode", data->sale_mode);
    cJSON_AddNumberToObject(currentStatus, "payMode", data->pay_mode);
    cJSON_AddNumberToObject(currentStatus, "days", data->days);
    cJSON_AddNumberToObject(currentStatus, "capacity", data->capacity);
    cJSON_AddItemToObject(root, "currentStatus", currentStatus);

    // 3. filters 数组打包 (新格式)
    cJSON *filters_arr = cJSON_CreateArray();
    for (int i = 0; i < 9; i++) {
        // 只上传有效（已配置）的滤芯
        if (data->filters[i].valid) {
            // 使用 C99 复合字面量快速创建 cJSON 整数数组
            int filter_data[4] = {
                i + 1, 
                data->filters[i].type, 
                data->filters[i].days, 
                data->filters[i].capacity
            };
            cJSON *item_arr = cJSON_CreateIntArray(filter_data, 4);
            cJSON_AddItemToArray(filters_arr, item_arr);
        }
    }
    cJSON_AddItemToObject(root, "filters", filters_arr);

    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return str;
}

// 3. 打包 Log (制水数据)
char* protocol_pack_log(const log_report_t *data) {
    cJSON *root = cJSON_CreateObject();

    if (data->timestamp > 0) {
        cJSON_AddNumberToObject(root, "timestamp", (double)data->timestamp);
    } else {
        cJSON_AddNumberToObject(root, "timestamp", (double)get_timestamp_ms());
    }
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
    if (data->timestamp > 0) {
        cJSON_AddNumberToObject(root, "timestamp", (double)data->timestamp);
    } else {
        cJSON_AddNumberToObject(root, "timestamp", (double)get_timestamp_ms());
    }
    cJSON_AddNumberToObject(root, "alertCode", data->alert_code);
    
    // 增加 status 字段，如果未指定则默认为 "triggered"
    if (data->status[0] != '\0') {
        cJSON_AddStringToObject(root, "status", data->status);
    } else {
        cJSON_AddStringToObject(root, "status", "triggered");
    }
    
    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return str;
}

// 5. 解析指令
esp_err_t protocol_parse_cmd(const char *json_str, int len, server_cmd_t *out_cmd) {
    if (!json_str || len <= 0 || !out_cmd) return ESP_ERR_INVALID_ARG;


    memset(out_cmd, 0, sizeof(server_cmd_t));
// 1. 解析整棵 JSON 树
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "cJSON Parse Failed!");
        return ESP_FAIL;
    }

    // 2. 解析根节点字段
    cJSON *item = NULL;
    if ((item = cJSON_GetObjectItem(root, "cmdId")) != NULL && cJSON_IsString(item)) {
        strncpy(out_cmd->cmd_id, item->valuestring, sizeof(out_cmd->cmd_id) - 1);
    }
    if ((item = cJSON_GetObjectItem(root, "method")) != NULL && cJSON_IsNumber(item)) {
        out_cmd->method = item->valueint;
    }
    if ((item = cJSON_GetObjectItem(root, "timestamp")) != NULL && cJSON_IsNumber(item)) {
        out_cmd->timestamp = (long long)item->valuedouble;
    }
    if (out_cmd->method == CMD_METHOD_OTA) {
        if ((item = cJSON_GetObjectItem(root, "url")) != NULL && cJSON_IsString(item)) {
            strncpy(out_cmd->param.ota_url, item->valuestring, sizeof(out_cmd->param.ota_url) - 1);
        }
    }
    
    

    
   

    cJSON *param = cJSON_GetObjectItem(root, "param");
    if (param && cJSON_IsObject(param)) {
        cJSON *item = NULL;
        if ((item = cJSON_GetObjectItem(param, "switch")) != NULL) out_cmd->param.switch_status = item->valueint;
        if ((item = cJSON_GetObjectItem(param, "saleMode")) != NULL) out_cmd->param.sale_mode = item->valueint;
        if ((item = cJSON_GetObjectItem(param, "payMode")) != NULL) out_cmd->param.pay_mode = item->valueint;
        if ((item = cJSON_GetObjectItem(param, "days")) != NULL) out_cmd->param.days = item->valueint;
        if ((item = cJSON_GetObjectItem(param, "capacity")) != NULL) out_cmd->param.capacity = item->valueint;
        if ((item = cJSON_GetObjectItem(param, "otaUrl")) != NULL && cJSON_IsString(item)) {
            strncpy(out_cmd->param.ota_url, item->valuestring, sizeof(out_cmd->param.ota_url) - 1);
        }
    }


    // 解析嵌套的 filters 数组 (新格式: [[1, 0, 150, 1200], [2, 0, 150, 1200]])
    cJSON *filters = cJSON_GetObjectItem(root, "filters");
    if (filters && cJSON_IsArray(filters)) {
        int size = cJSON_GetArraySize(filters);
        for (int i = 0; i < size; i++) {
            cJSON *f_item = cJSON_GetArrayItem(filters, i);
            
            // 确保这是一个数组，并且至少有 4 个元素
            if (f_item && cJSON_IsArray(f_item) && cJSON_GetArraySize(f_item) >= 4) {
                int level = cJSON_GetArrayItem(f_item, 0)->valueint;
                int type  = cJSON_GetArrayItem(f_item, 1)->valueint;
                int days  = cJSON_GetArrayItem(f_item, 2)->valueint;
                int cap   = cJSON_GetArrayItem(f_item, 3)->valueint;

                // 校验级数是否合法 (1~9级)
                if (level >= 1 && level <= 9) {
                    int idx = level - 1;
                    out_cmd->filters[idx].valid = true;
                    out_cmd->filters[idx].type = type;
                    out_cmd->filters[idx].days = days;
                    out_cmd->filters[idx].capacity = cap;
                }
            }
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}
