// blufi_custom.c 自定义 BLUFI 实现
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_blufi_api.h"
#include "esp_blufi.h"



#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
#include "esp_bt.h"
#endif

#ifdef CONFIG_BT_BLUEDROID_ENABLED
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#endif

#ifdef CONFIG_BT_NIMBLE_ENABLED
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "console/console.h"
#endif

// 引入组件头文件
#include "blufi_custom.h"
#include "blufi_custom_priv.h"
#include "json_parser.h" 
#include "app_storage.h"

static wifi_config_t sta_config;
static bool gl_sta_connected = false;

// --- NimBLE 必须的辅助函数与变量 ---
#ifdef CONFIG_BT_NIMBLE_ENABLED
// Blufi GATT Server 初始化函数 (在 IDF 组件内部)
extern int esp_blufi_gatt_svr_init(void);
// 其它回调
void ble_store_config_init(void);

static void blufi_on_reset(int reason) {
    BLUFI_ERROR("NimBLE Reset: reason=%d", reason);
}

static void blufi_on_sync(void) {
    // 只有在 sync 之后，才算协议栈准备好，但 Blufi 初始化通常在后面
}

// NimBLE 主机任务 (必须一直运行)
static void bleprph_host_task(void *param) {
    BLUFI_INFO("BLE Host Task Started");
    nimble_port_run(); // 这个函数不会返回
    nimble_port_freertos_deinit();
}
#endif

// ---------------------------------------------------------
//  自定义数据处理 (核心业务逻辑)
// ---------------------------------------------------------
static void handle_custom_data(uint8_t *data, int len) {
    BLUFI_INFO("收到自定义数据 (Len: %d)", len);

    // 1. 安全处理：确保是字符串
    char *json_str = (char *)malloc(len + 1);
    if (!json_str) {
        BLUFI_ERROR("内存分配失败");
        return;
    }
    memcpy(json_str, data, len);
    json_str[len] = '\0';
    
    BLUFI_INFO("解析 JSON: %s", json_str);

    // 2. 使用 json_parser 进行解析
    jparse_ctx_t jctx;
    if (json_parse_start(&jctx, json_str, len) != 0) {
        BLUFI_ERROR("JSON 格式错误");
        free(json_str);
        // 反馈错误给 App
        blufi_send_custom_result(false, "JSON Error");
        return;
    }

    net_config_t cfg = {0};

    int mode_val = 0; // 0=WiFi, 1=4G
    // 提取字段 "mode"
    if (json_obj_get_int(&jctx, "mode", &mode_val) == 0) {
        cfg.mode = mode_val;
        BLUFI_INFO("设置联网模式: %s", cfg.mode == 1 ? "4G" : "Wi-Fi");
    }
    // 提取字段 "url"
    if (json_obj_get_string(&jctx, "url", cfg.url, sizeof(cfg.url)) == 0) {
        BLUFI_INFO("设置服务器地址: %s", cfg.url);
    }

    // 3. 清理资源
    json_parse_end(&jctx);
    free(json_str);

    // NVS 保存逻辑
    if (app_storage_save_net_config(&cfg) == ESP_OK) {
        blufi_send_custom_result(true, "Config Saved");
    } else {
        blufi_send_custom_result(false, "Save Failed");
    }
    // TODO: 这里可以发送一个 Event 或者信号量，通知主程序切换网络状态
}




// ---------------------------------------------------------
//  Blufi 回调处理
// ---------------------------------------------------------
static void example_event_callback(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param) {
    switch (event) {
    case ESP_BLUFI_EVENT_INIT_FINISH:
        esp_blufi_adv_start();
        BLUFI_INFO("Blufi 初始化完成，开始广播");
        break;
    case ESP_BLUFI_EVENT_BLE_CONNECT:
        BLUFI_INFO("蓝牙已连接");
        blufi_security_init();
        break;
    case ESP_BLUFI_EVENT_BLE_DISCONNECT:
        BLUFI_INFO("蓝牙已断开");
        blufi_security_deinit();
        esp_blufi_adv_start(); 
        break;
    case ESP_BLUFI_EVENT_RECV_STA_SSID:
        strncpy((char *)sta_config.sta.ssid, (char *)param->sta_ssid.ssid, param->sta_ssid.ssid_len);
        sta_config.sta.ssid[param->sta_ssid.ssid_len] = '\0';
        BLUFI_INFO("收到 SSID: %s", sta_config.sta.ssid);
        break;
    case ESP_BLUFI_EVENT_RECV_STA_PASSWD:
        strncpy((char *)sta_config.sta.password, (char *)param->sta_passwd.passwd, param->sta_passwd.passwd_len);
        sta_config.sta.password[param->sta_passwd.passwd_len] = '\0';
        BLUFI_INFO("收到 Password");
        break;
    case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA:
        // *** 修正点 ***
        handle_custom_data(param->custom_data.data, param->custom_data.data_len);
        break;
    case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP:
        BLUFI_INFO("收到连接请求，开始连接 Wi-Fi");
        esp_wifi_disconnect();
        esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            BLUFI_ERROR("Wi-Fi 连接启动失败! 错误码: %s", esp_err_to_name(err));
            // 如果你之前没加 esp_wifi_start，这里原本会打印 ESP_ERR_WIFI_NOT_STARTED
        }
        break;
    case ESP_BLUFI_EVENT_REPORT_ERROR:
        BLUFI_ERROR("Blufi 错误代码: %d", param->report_error.state);
        esp_blufi_send_error_info(param->report_error.state);
        break;
    default:
        break;
    }
}

static esp_blufi_callbacks_t example_callbacks = {
    .event_cb = example_event_callback,
    .negotiate_data_handler = blufi_dh_negotiate_data_handler,
    .encrypt_func = blufi_aes_encrypt,
    .decrypt_func = blufi_aes_decrypt,
    .checksum_func = blufi_crc_checksum,
};

// ---------------------------------------------------------
//  Wi-Fi 事件回调
// ---------------------------------------------------------
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);

    if (event_id == WIFI_EVENT_STA_CONNECTED) {
        gl_sta_connected = true;
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        gl_sta_connected = false;
        esp_blufi_extra_info_t info = {0};
        esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL, 0, &info);
    }
}

static void ip_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_id == IP_EVENT_STA_GOT_IP) {
        wifi_mode_t mode;
        esp_wifi_get_mode(&mode);
        esp_blufi_extra_info_t info = {0};
        esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_SUCCESS, 0, &info);
        BLUFI_INFO("Wi-Fi 连接成功，已反馈给 APP");
    }
}

// ---------------------------------------------------------
//  对外接口实现
// ---------------------------------------------------------
esp_err_t blufi_custom_init(void) {
    esp_err_t ret;

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL));

    // 1. 控制器层初始化 (Controller)
#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT)); // 释放经典蓝牙内存

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) { BLUFI_ERROR("BT Controller Init Failed"); return ret; }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) { BLUFI_ERROR("BT Controller Enable Failed"); return ret; }
#endif

    // 2. 主机层初始化 (Host Stack)
#ifdef CONFIG_BT_BLUEDROID_ENABLED
    // ---> Bluedroid 路径
    ret = esp_bluedroid_init();
    if (ret) return ret;
    ret = esp_bluedroid_enable();
    if (ret) return ret;
    BLUFI_INFO("Bluedroid Stack Initialized");
#endif

#ifdef CONFIG_BT_NIMBLE_ENABLED
    // ---> NimBLE 路径
    ret = esp_nimble_init();
    if (ret) { BLUFI_ERROR("NimBLE init failed");return ret; }

    // (A) 配置回调
    ble_hs_cfg.reset_cb = blufi_on_reset;
    ble_hs_cfg.sync_cb = blufi_on_sync;
    ble_hs_cfg.gatts_register_cb = esp_blufi_gatt_svr_register_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = 4; // DisplayOnly
    #ifdef CONFIG_EXAMPLE_BONDING
    ble_hs_cfg.sm_bonding = 1;
    #endif

    // (B) 初始化 Blufi GATT 服务
    ret = esp_blufi_gatt_svr_init(); 
    if (ret) {
        BLUFI_ERROR("Blufi GATT Svr init failed");
        return ret;
    }

    // (C) 设置默认设备名
    ret = ble_svc_gap_device_name_set("BLUFI_DEVICE");
    if (ret) return ret;

    // (D) 初始化 BTC 任务 ***
    esp_blufi_btc_init();

    // (E) 启动 NimBLE Host 任务
    ret = esp_nimble_enable(bleprph_host_task);
    if (ret) return ret;
    
    BLUFI_INFO("NimBLE Stack & BTC Task Initialized");
#endif

    // 3. 注册 Blufi 回调
    ret = esp_blufi_register_callbacks(&example_callbacks);
    if (ret) return ret;

    // 4. 启动 Blufi
    ret = esp_blufi_profile_init();
    return ret;
}

void blufi_custom_deinit(void) {
    esp_blufi_profile_deinit();

#ifdef CONFIG_BT_BLUEDROID_ENABLED
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
#endif

// 在 ESP-IDF 的 NimBLE 架构中，实现“优雅且安全”的 Deinit（反初始化）比初始化要麻烦得多。
// #ifdef CONFIG_BT_NIMBLE_ENABLED
//     esp_nimble_deinit();
// #endif

#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
#endif
}

void blufi_send_custom_result(bool success, const char* msg) {
    if (!msg) msg = "";
    int len = strlen(msg);
    // 分配 buffer: 1 byte status + message
    uint8_t *data = (uint8_t*)malloc(len + 1);
    
    if (data) {
        data[0] = success ? 0x01 : 0x00;
        memcpy(data + 1, msg, len);
        
        esp_blufi_send_custom_data(data, len + 1);
        free(data);
    }
}