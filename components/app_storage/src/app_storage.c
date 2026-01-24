// app_storage.c NVS 存取实现
#include "app_storage.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "STORAGE";

// 定义不同的 Namespace，防止 Key 冲突
#define NS_NET_CFG   "net_cfg"
#define NS_DEV_STAT  "dev_stat"

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
    
    err = nvs_open(NS_NET_CFG, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_i32(handle, "mode", cfg->mode);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "url", cfg->url);
    }

    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Net config saved");
    return err;
}

esp_err_t app_storage_load_net_config(net_config_t *cfg) {
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open(NS_NET_CFG, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    int32_t mode = 0;
    size_t url_len = sizeof(cfg->url);
    
    nvs_get_i32(handle, "mode", &mode);
    cfg->mode = (int)mode;
    
    if (nvs_get_str(handle, "url", cfg->url, &url_len) != ESP_OK) {
        // 默认值
        strcpy(cfg->url, "");
    }

    nvs_close(handle);
    return ESP_OK;
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

// ... load_status 同理 ...
// TODO 待完善