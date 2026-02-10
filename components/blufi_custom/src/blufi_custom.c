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
#include "blufi_custom.h"
#include "mqtt_manager.h"


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

static const char *TAG = "BLUFI";

static wifi_config_t sta_config;
static bool gl_sta_connected = false;

// --- NimBLE 必须的辅助函数与变量 ---
#ifdef CONFIG_BT_NIMBLE_ENABLED
// Blufi GATT Server 初始化函数 (在 IDF 组件内部)
extern int esp_blufi_gatt_svr_init(void);
// 其它回调
void ble_store_config_init(void);

void blufi_send_mqtt_status(int status) {
    char payload[64];
    int len = snprintf(payload, sizeof(payload), "{\"statusMQTT\":%d}", status);
    esp_blufi_send_custom_data((uint8_t *)payload, len);
    ESP_LOGI(TAG, "Sent via BLE: %s", payload);
}


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

static void send_json_status(const char *key, int value) {
    char payload[64];
    int len = snprintf(payload, sizeof(payload), "{\"%s\":%d}", key, value);
    esp_blufi_send_custom_data((uint8_t *)payload, len);
    ESP_LOGI(TAG, "Reply: %s", payload);
}


// ---------------------------------------------------------
//  自定义数据处理 (核心业务逻辑)
// ---------------------------------------------------------
static void handle_custom_data(uint8_t *data, int len) {
    BLUFI_INFO("收到自定义数据 (Len: %d): %s", len, data);

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
    if (json_parse_start(&jctx, json_str, len) == 0) {
        int val = 0;
        char str_buf[128] = {0};
        
        // 1. statusBLE (握手/关闭)
        if (json_obj_get_int(&jctx, "statusBLE", &val) == 0) {
            ESP_LOGI(TAG, "Step 1/5: StatusBLE Check: %d", val);
            send_json_status("statusBLE", val);
            if (val == 1) {
                // 收到关闭请求，断开蓝牙
                ESP_LOGW(TAG, "Step 5: Closing BLE connection...");
                vTaskDelay(pdMS_TO_TICKS(1000)); // 给一点时间让回复发出去
                esp_blufi_disconnect(); 
            }
        }

        // 2. statusNet (配网模式)
        if (json_obj_get_int(&jctx, "statusNet", &val) == 0) {
            ESP_LOGI(TAG, "Step 2: Set Net Mode: %d", val);
            // 保存模式到 NVS
            net_config_t cfg;
            if (app_storage_load_net_config(&cfg) != ESP_OK) memset(&cfg, 0, sizeof(cfg));
            cfg.mode = val;
            app_storage_save_net_config(&cfg);
            
            // 返回确认
            send_json_status("statusNet", 1);
        }

        // 4. MQTT 配置
        har mqtt_server_buf[64] = {0};
        char username_buf[64] = {0};
        char password_buf[64] = {0};

        // 解析 mqttserver: "IP:Port"
        if (json_obj_get_string(&jctx, "mqttserver", mqtt_server_buf, sizeof(mqtt_server_buf)) == 0) {
            ESP_LOGI(TAG, "[Step 4] Recv MQTT Config: %s", mqtt_server_buf);
            
            // 获取用户名和密码
            json_obj_get_string(&jctx, "username", username_buf, sizeof(username_buf));
            json_obj_get_string(&jctx, "password", password_buf, sizeof(password_buf));

            // 保存到 NVS
            net_config_t cfg;
            if (app_storage_load_net_config(&cfg) != ESP_OK) memset(&cfg, 0, sizeof(cfg));

            // 解析 IP 和 Port
            char *colon = strchr(mqtt_server_buf, ':');
            if (colon) {
                *colon = '\0'; // 截断字符串
                strncpy(cfg.mqtt_host, mqtt_server_buf, sizeof(cfg.mqtt_host) - 1);
                cfg.mqtt_port = atoi(colon + 1);
            } else {
                strncpy(cfg.mqtt_host, mqtt_server_buf, sizeof(cfg.mqtt_host) - 1);
                cfg.mqtt_port = 1883; // 默认端口
            }

            strncpy(cfg.username, username_buf, sizeof(cfg.username) - 1);
            strncpy(cfg.password_mqtt, password_buf, sizeof(cfg.password_mqtt) - 1);

            // 构造完整 URL: mqtt://ip:port (MQTT Client库会自动处理用户名密码，不需要拼在URL里，但在配置结构体里拼一下也没事)
            snprintf(cfg.full_url, sizeof(cfg.full_url), "mqtt://%s:%d", cfg.mqtt_host, cfg.mqtt_port);

            app_storage_save_net_config(&cfg);
            ESP_LOGI(TAG, "Config Saved. Restarting MQTT...");

            // * 关键修改 *
            // 1. 这里不要立即回复 statusMQTT:0
            // 2. 而是重启 MQTT 客户端
            mqtt_manager_start(); 
            
            // 3. 等待 mqtt_manager 在连接并 Init 成功后，
            //    自动回调 blufi_send_mqtt_status(0)
        }

        json_parse_end(&jctx);
    }
    free(json_str);

}




// ---------------------------------------------------------
//  Wi-Fi 事件回调
// ---------------------------------------------------------
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*) event_data;
        gl_sta_connected = false;
        
        // 1. 报告失败给 App
        send_json_status("statusWiFi", 2); 
        
        // 2. 使用 Blufi 标准报告失败信息 (可以带上具体的 Reason)
        esp_blufi_extra_info_t info = {0};
        // 这里的 reason 可以让手机 App 知道是密码错还是找不到 SSID
        esp_blufi_send_wifi_conn_report(WIFI_MODE_STA, ESP_BLUFI_STA_CONN_FAIL, event->reason, &info);
        
        // 3. 策略性重连（避免密码错误时的无限死循环）
        if (event->reason != WIFI_REASON_AUTH_FAIL) {
            esp_wifi_connect();
        }
    } 
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        gl_sta_connected = true;
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        
        // 只有拿到 IP 才算真正的“成功”
        send_json_status("statusWiFi", 1);
        
        esp_blufi_extra_info_t info = {0};
        esp_blufi_send_wifi_conn_report(WIFI_MODE_STA, ESP_BLUFI_STA_CONN_SUCCESS, 0, &info);
        
        ESP_LOGI(TAG, "Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
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
        // 1. 清零并拷贝，注意防止越界
        memset(&sta_config, 0, sizeof(sta_config));
        memset(sta_config.sta.ssid, 0, sizeof(sta_config.sta.ssid));
        uint8_t ssid_len = (param->sta_ssid.ssid_len < 32) ? param->sta_ssid.ssid_len : 31;
        memcpy(sta_config.sta.ssid, param->sta_ssid.ssid, ssid_len);
        
        send_json_status("statusWiFi", 0);
        break;
    case ESP_BLUFI_EVENT_RECV_STA_PASSWD:
        // 2. 同样的清零并拷贝
        memset(sta_config.sta.password, 0, sizeof(sta_config.sta.password));
        uint8_t pwd_len = (param->sta_passwd.passwd_len < 64) ? param->sta_passwd.passwd_len : 63;
        memcpy(sta_config.sta.password, param->sta_passwd.passwd, pwd_len);
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
        esp_wifi_start();

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