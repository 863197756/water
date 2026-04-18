// app_storage.c NVS 存取实现
#include "app_storage.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include "esp_wifi.h"

static const char *TAG = "STORAGE";

// 定义不同的 Namespace，防止 Key 冲突

#define NS_DEV_STAT  "dev_stat"
#define NS_ACTION_LOG "act_log"
#define NS_DEV_ID    "dev_id"

esp_err_t app_storage_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

// --- 网络配置实现 ---
esp_err_t app_storage_save_net_config(const net_config_t *cfg) {
    nvs_handle_t handle;
    esp_err_t err;
    
    err = nvs_open(NET_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(handle, NET_CONFIG_KEY, cfg, sizeof(net_config_t));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
        ESP_LOGI(TAG, "Config saved to NVS. Mode: %s", (cfg->mode == 1) ? "4G" : "WiFi");
    }
    nvs_close(handle);
    return err;
}

esp_err_t app_storage_load_net_config(net_config_t *cfg) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NET_CONFIG_NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) return err;

    size_t required_size = sizeof(net_config_t);
    err = nvs_get_blob(my_handle, NET_CONFIG_KEY, cfg, &required_size);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Config loaded. Mode: %d, MQTT: %s", cfg->mode, cfg->full_url );
    } else {
        ESP_LOGW(TAG, "No config found in NVS");
    }

    nvs_close(my_handle);
    return err;
}


// --- 设备状态实现 ---
esp_err_t app_storage_save_status(const device_status_t *status) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_DEV_STAT, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    nvs_set_i32(handle, "flow", status->total_flow);
    nvs_set_i32(handle, "switch", status->switch_state);
    nvs_set_i32(handle, "pay_mode", status->pay_mode);
    nvs_set_i32(handle, "days", status->days);
    nvs_set_i32(handle, "capacity", status->capacity);
    
    char key_val[16], key_typ[16], key_days[16], key_cap[16];
    for (int i = 0; i < 9; i++) {
        snprintf(key_val, sizeof(key_val), "f%d_val", i+1);
        snprintf(key_typ, sizeof(key_typ), "f%d_typ", i+1);
        snprintf(key_days, sizeof(key_days), "f%d_days", i+1);
        snprintf(key_cap, sizeof(key_cap), "f%d_cap", i+1);
        
        nvs_set_u8(handle, key_val, status->filter_valid[i]?1:0); // bool 存为 u8
        nvs_set_i32(handle, key_typ, status->filter_type[i]);
        nvs_set_i32(handle, key_days, status->filter_days[i]);
        nvs_set_i32(handle, key_cap, status->filter_capacity[i]);
    }
    
    nvs_commit(handle);
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t app_storage_load_status(device_status_t *status) {
    if (!status) return ESP_ERR_INVALID_ARG;
    memset(status, 0, sizeof(device_status_t));
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_DEV_STAT, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        // 读取失败（全新刷机或被擦除），给极其严格的安全默认值防“白嫖”
        memset(status, 0, sizeof(device_status_t));
        status->switch_state = 1; // 允许开机，但会因为额度为 0 被状态机拦截制水
        status->pay_mode = 0;     // 默认计时
        status->days = 0;         // 【修改为0】等云端下发真实额度
        status->capacity = 0;     // 【修改为0】
        // 滤芯的 valid 默认全为 false (0)，无需额外赋初值
        return err;
    }

    int32_t val = 0;
    if (nvs_get_i32(handle, "flow", &val) == ESP_OK) status->total_flow = val;
    if (nvs_get_i32(handle, "switch", &val) == ESP_OK) status->switch_state = val;
    if (nvs_get_i32(handle, "pay_mode", &val) == ESP_OK) status->pay_mode = val;
    if (nvs_get_i32(handle, "days", &val) == ESP_OK) status->days = val;
    if (nvs_get_i32(handle, "capacity", &val) == ESP_OK) status->capacity = val;
    
    char key_val[16], key_typ[16], key_days[16], key_cap[16];
    uint8_t b_val = 0;
    for (int i = 0; i < 9; i++) {
        snprintf(key_val, sizeof(key_val), "f%d_val", i+1);
        snprintf(key_typ, sizeof(key_typ), "f%d_typ", i+1);
        snprintf(key_days, sizeof(key_days), "f%d_days", i+1);
        snprintf(key_cap, sizeof(key_cap), "f%d_cap", i+1);
        if (nvs_get_u8(handle, key_val, &b_val) == ESP_OK) status->filter_valid[i] = (b_val != 0);
        if (nvs_get_i32(handle, key_typ, &val) == ESP_OK) status->filter_type[i] = val;
        if (nvs_get_i32(handle, key_days, &val) == ESP_OK) status->filter_days[i] = val;
        if (nvs_get_i32(handle, key_cap, &val) == ESP_OK) status->filter_capacity[i] = val;
    }

    nvs_close(handle);
    return ESP_OK;
}

// TODO 待完善
// --- 操作日志实现 ---
// 使用 NVS Blob / SPIFFS 
// 需要讨论一下要存多少条日志，以及每条日志的长度（都存什么操作数据）


// TODO 待完善
// --- 清除配置实现 ---
// 网络重置：清除Wi-Fi配置、联网模式、服务器地址
// 解绑重置：网络重置 + 清除用户ID / Token  ————————这个还没写
// 恢复出厂：清除所有配置，包括网络、设备状态、操作日志、累计数据（滤芯、流量）
// [新增] 辅助函数：擦除指定 Namespace
static void erase_namespace(const char* ns) {
    nvs_handle_t handle;
    if (nvs_open(ns, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle); // 擦除该空间下所有 Key
        nvs_commit(handle);
        nvs_close(handle);
        ESP_LOGW(TAG, "Namespace '%s' erased", ns);
    }
}

esp_err_t app_storage_erase(reset_level_t level) {
    // 1. Level 1: 网络重置 (最常用)
    if (level >= RESET_LEVEL_NET) {
        // A. 清除自定义的网络配置 (Mode, URL)
        erase_namespace(NET_CONFIG_NAMESPACE);
        
        // B. 清除 ESP32 底层存储的 Wi-Fi SSID/密码
        // 注意：这会把 esp_wifi_set_config 存的数据清掉
        esp_wifi_restore(); 
        
        ESP_LOGI(TAG, "=== Network Config Reset Done ===");
    }

    // 2. Level 3: 恢复出厂 (慎用)
    if (level >= RESET_LEVEL_FACTORY) {
        // A. 清除设备状态 (滤芯、流量)
        erase_namespace(NS_DEV_STAT);
        
        // B. 清除日志
        erase_namespace(NS_ACTION_LOG);
        
        ESP_LOGW(TAG, "!!! FACTORY RESET COMPLETED !!!");
    }

    return ESP_OK;
}




esp_err_t app_storage_set_pending_init(uint8_t val) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NET_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    
    err = nvs_set_u8(handle, "pending_init", val);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

uint8_t app_storage_get_pending_init(void) {
    nvs_handle_t handle;
    uint8_t val = 0; // 默认不发
    if (nvs_open(NET_CONFIG_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, "pending_init", &val);
        nvs_close(handle);
    }
    return val;
}

esp_err_t app_storage_set_sn(const char *sn) {
    if (!sn || !sn[0]) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_DEV_ID, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_str(handle, "sn", sn);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t app_storage_get_sn(char *out_sn, size_t max_len) {
    if (!out_sn || max_len == 0) return ESP_ERR_INVALID_ARG;
    out_sn[0] = 0;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_DEV_ID, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t required = 0;
    err = nvs_get_str(handle, "sn", NULL, &required);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }
    if (required > max_len) {
        nvs_close(handle);
        return ESP_ERR_INVALID_SIZE;
    }

    err = nvs_get_str(handle, "sn", out_sn, &required);
    nvs_close(handle);
    return err;
}
