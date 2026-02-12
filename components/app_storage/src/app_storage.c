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
    nvs_set_i32(handle, "filter", status->filter_life);
    
    nvs_commit(handle);
    nvs_close(handle);
    return ESP_OK;
}


esp_err_t app_storage_load_status(device_status_t *status) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_DEV_STAT, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        // 如果读取失败（比如第一次运行），给默认值
        status->total_flow = 0;
        status->filter_life = 100; // 假设 100%
        return err;
    }

    int32_t val = 0;
    if (nvs_get_i32(handle, "flow", &val) == ESP_OK) status->total_flow = val;
    if (nvs_get_i32(handle, "filter", &val) == ESP_OK) status->filter_life = val;

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