#include "protocol.h"
#include "cJSON.h"          // 官方组件，用于生成
#include "json_parser.h"    // 你的组件，用于解析
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "PROTO";

// ==========================================
// 1. 打包 (使用 cJSON)
// ==========================================
char* protocol_pack_status(const report_data_t *data) {
    // 创建根对象
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to create cJSON object");
        return NULL;
    }

    // 添加字段 (这里定义 JSON 的 Key)
    cJSON_AddStringToObject(root, "uid", data->device_id);
    cJSON_AddNumberToObject(root, "tds", data->tds_value);
    cJSON_AddNumberToObject(root, "flow", data->total_flow);
    cJSON_AddNumberToObject(root, "filter", data->filter_life);
    cJSON_AddNumberToObject(root, "err", data->error_code);

    // 生成紧凑的 JSON 字符串 (无空格换行，节省流量)
    char *payload = cJSON_PrintUnformatted(root);
    
    // 如果需要调试打印，可以用 cJSON_Print(root) 生成带格式的字符串
    
    // 释放 cJSON 对象内存 (非常重要，否则内存泄漏!)
    cJSON_Delete(root);

    return payload; 
}

// ==========================================
// 2. 解析 (使用 json_parser / JSMN)
// ==========================================
esp_err_t protocol_parse_cmd(const char *json_str, int len, server_cmd_t *out_cmd) {
    if (json_str == NULL || len <= 0 || out_cmd == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    jparse_ctx_t jctx;
    // 初始化解析器
    if (json_parse_start(&jctx, json_str, len) != 0) {
        ESP_LOGE(TAG, "JSON parse start failed");
        return ESP_FAIL;
    }

    // 清空输出结构体
    memset(out_cmd, 0, sizeof(server_cmd_t));

    // 1. 提取 'cmd' (必须存在)
    if (json_obj_get_string(&jctx, "cmd", out_cmd->cmd, sizeof(out_cmd->cmd)) != 0) {
        ESP_LOGE(TAG, "Missing 'cmd' field");
        json_parse_end(&jctx);
        return ESP_FAIL;
    }

    // 2. 提取 'param' (可选，默认0)
    if (json_obj_get_int(&jctx, "param", &out_cmd->param) != 0) {
        out_cmd->param = 0; 
    }

    // 3. 提取 'str_param' (可选)
    // 例如：{"cmd":"update_url", "str_param":"http://..."}
    if (json_obj_get_string(&jctx, "str_param", out_cmd->str_param, sizeof(out_cmd->str_param)) != 0) {
        out_cmd->str_param[0] = '\0';
    }

    json_parse_end(&jctx);
    return ESP_OK;
}