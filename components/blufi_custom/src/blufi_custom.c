// blufi_custom.c
// 说明：已移除 ESP-IDF BLUFI 协议栈，改为“自定义 BLE GATT + JSON”配网通道。
// 配网窗口：每次上电/重启后开放 120 秒；超时后停止广播并拒绝写入。
// 单连接：sdkconfig 已设置 NimBLE/BLE 最大连接数为 1。
//
// 协议步骤（与《蓝牙通信协议.xlsx》一致，并增加“必须按步骤”门禁）：
// 1) {"statusBLE":0}            -> 设备回 {"statusBLE":0}
// 2) {"statusNet":0/1}          -> 设备回 {"statusNet":1} （0=WiFi,1=4G）
// 3) WiFi 模式：{"ssid":"..","password":".."}
//    -> 设备开始连接时回 {"statusWiFi":0}
//    -> 成功（拿到 IP）回 {"statusWiFi":1}
//    -> 失败回 {"statusWiFi":2}
// 4) {"mqttserver":"ip:port","username":"..","password":".."}
//    -> 设备在“MQTT 已完成登录并成功发送 init 且收到套餐 cmd（plan）”时回 {"statusMQTT":0}
//       （当前沿用 APP_EVENT_MQTT_PLAN_RECEIVED 触发）
// 5) {"statusBLE":1}            -> 设备回 {"statusBLE":1} 并主动断开 BLE
//
// 注意：
// - 该通道为明文 JSON；安全策略依赖“配网窗口 + 步骤门禁”。
// - 小程序侧建议请求 MTU（例如 256），避免 mqtt 配置 JSON 超过 20 字节限制。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "esp_bt.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "json_parser.h"

#include "app_events.h"
#include "app_storage.h"
#include "blufi_custom.h"

// 某些 ESP-IDF/NimBLE 组合环境下，该函数的声明没有被默认头文件暴露出来，
// 但链接阶段仍由 NimBLE store 组件提供实现。这里显式声明以避免编译报错：
// "implicit declaration of function 'ble_store_config_init'".
void ble_store_config_init(void);

// -----------------------------
// 日志
// -----------------------------
#define PROV_TAG "BLE_PROV"
#define PROV_I(fmt, ...) ESP_LOGI(PROV_TAG, fmt, ##__VA_ARGS__)
#define PROV_W(fmt, ...) ESP_LOGW(PROV_TAG, fmt, ##__VA_ARGS__)
#define PROV_E(fmt, ...) ESP_LOGE(PROV_TAG, fmt, ##__VA_ARGS__)

// -----------------------------
// 参数
// -----------------------------
#define PROV_WINDOW_SEC          120
#define PROV_JSON_MAX_LEN        512   // 单次写入最大 JSON 长度（需 <= MTU-3；建议小程序侧请求 MTU=256）
#define PROV_WIFI_RETRY_MAX      3

// -----------------------------
// 协议步骤门禁
// -----------------------------
typedef enum {
    PROV_STEP_WAIT_BLE_OPEN = 0,  // 等待 {"statusBLE":0}
    PROV_STEP_WAIT_NET_MODE,      // 等待 {"statusNet":0/1}
    PROV_STEP_WAIT_WIFI_CFG,      // 等待 {"ssid":...,"password":...}（仅 WiFi 模式）
    PROV_STEP_WAIT_WIFI_RESULT,   // 等待 WiFi 结果（IP / fail），期间不接收 MQTT
    PROV_STEP_WAIT_MQTT_CFG,      // 等待 MQTT 配置 JSON
    PROV_STEP_WAIT_BLE_CLOSE,     // 等待 {"statusBLE":1}
    PROV_STEP_DONE,
} prov_step_t;

// 状态机主状态（需在 prov_set_step 使用前声明）
// 静态全局变量默认 0 初始化，正好对应 PROV_STEP_WAIT_BLE_OPEN=0
static prov_step_t s_step;

static const char *prov_step_str(prov_step_t s) {
    switch (s) {
    case PROV_STEP_WAIT_BLE_OPEN:   return "WAIT_BLE_OPEN";
    case PROV_STEP_WAIT_NET_MODE:   return "WAIT_NET_MODE";
    case PROV_STEP_WAIT_WIFI_CFG:   return "WAIT_WIFI_CFG";
    case PROV_STEP_WAIT_WIFI_RESULT:return "WAIT_WIFI_RESULT";
    case PROV_STEP_WAIT_MQTT_CFG:   return "WAIT_MQTT_CFG";
    case PROV_STEP_WAIT_BLE_CLOSE:  return "WAIT_BLE_CLOSE";
    case PROV_STEP_DONE:           return "DONE";
    default:                       return "UNKNOWN";
    }
}

static void prov_set_step(prov_step_t next, const char *reason) {
    if (s_step == next) return;
    if (reason) {
        PROV_I("STEP %s -> %s (%s)", prov_step_str(s_step), prov_step_str(next), reason);
    } else {
        PROV_I("STEP %s -> %s", prov_step_str(s_step), prov_step_str(next));
    }
    s_step = next;
}

// -----------------------------
// BLE 相关全局
// -----------------------------
static uint8_t s_own_addr_type;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_val_handle = 0;
static bool s_notify_enabled = false;

// 配网窗口
static esp_timer_handle_t s_prov_timer = NULL;
static bool s_prov_window_open = false;

// 状态机
static int s_net_mode = -1; // 0=WiFi, 1=4G

// Wi-Fi 配网过程状态
static bool s_wifi_prov_connecting = false;
static bool s_wifi_ignore_disconnect_once = false; // 主动 stop 造成的断开，忽略一次
static int  s_wifi_prov_retry = 0;

// MQTT 反馈（沿用原工程：收到 APP_EVENT_MQTT_PLAN_RECEIVED 视为成功）
static bool s_mqtt_waiting = false;
static esp_timer_handle_t s_mqtt_timer = NULL;

// -----------------------------
// 自定义 GATT UUID（128-bit）
// 说明：请在小程序侧使用同样的 UUID 进行发现/读写。
// -----------------------------
// 注意：BLE_UUID128_INIT 传入的是“字节序（little-endian）”，以下对应的小程序侧 UUID 字符串是：
// Service: 9e3c0001-2c6b-4f9b-8b5a-6f686e3d110a
// RX(Write): 9e3c0002-2c6b-4f9b-8b5a-6f686e3d110a
// TX(Notify): 9e3c0003-2c6b-4f9b-8b5a-6f686e3d110a
static const ble_uuid128_t g_prov_svc_uuid =
    BLE_UUID128_INIT(0x0a,0x11,0x3d,0x6e,0x68,0x6f,0x5a,0x8b,0x9b,0x4f,0x6b,0x2c,0x01,0x00,0x3c,0x9e);
static const ble_uuid128_t g_prov_rx_uuid =
    BLE_UUID128_INIT(0x0a,0x11,0x3d,0x6e,0x68,0x6f,0x5a,0x8b,0x9b,0x4f,0x6b,0x2c,0x02,0x00,0x3c,0x9e);
static const ble_uuid128_t g_prov_tx_uuid =
    BLE_UUID128_INIT(0x0a,0x11,0x3d,0x6e,0x68,0x6f,0x5a,0x8b,0x9b,0x4f,0x6b,0x2c,0x03,0x00,0x3c,0x9e);

// -----------------------------
// 前置声明
// -----------------------------
static void prov_start_advertising(void);
static void prov_stop_advertising(void);
static void prov_send_json_raw(const char *json_str);
static void prov_send_kv_int(const char *key, int value);
static void prov_handle_json(uint8_t *data, int len);

// -----------------------------
// 工具：安全拷贝 os_mbuf -> buf
// -----------------------------
static int om_to_buf(const struct os_mbuf *om, uint8_t *dst, int dst_sz) {
    int len = OS_MBUF_PKTLEN(om);
    if (len <= 0 || len >= dst_sz) return -1;
    os_mbuf_copydata(om, 0, len, dst);
    dst[len] = 0;
    return len;
}

// -----------------------------
// BLE Notify：发送 JSON
// -----------------------------
static void prov_send_json_raw(const char *json_str) {
    if (!json_str) return;
    // 为了方便串口观察：无论是否真正发出，都打印要回复的 JSON
    PROV_I("TX JSON: %s", json_str);

    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        PROV_W("TX dropped: not connected");
        return;
    }
    if (!s_notify_enabled) {
        PROV_W("TX dropped: notify not enabled (need subscribe on TX characteristic)");
        return;
    }
    if (!s_tx_val_handle) {
        PROV_W("TX dropped: tx handle not ready");
        return;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(json_str, strlen(json_str));
    if (!om) return;
    int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_val_handle, om);
    if (rc != 0) {
        PROV_W("notify failed rc=%d", rc);
    }
}

static void prov_send_kv_int(const char *key, int value) {
    char payload[64];
    int n = snprintf(payload, sizeof(payload), "{\"%s\":%d}", key, value);
    if (n > 0) prov_send_json_raw(payload);
}

// -----------------------------
// 配网窗口控制
// -----------------------------
static void prov_window_close_cb(void *arg) {
    (void)arg;
    s_prov_window_open = false;
    PROV_I("Provision window closed");
    prov_stop_advertising();

    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        // 主动断开
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static void prov_window_open(void) {
    s_prov_window_open = true;
    s_step = PROV_STEP_WAIT_BLE_OPEN;
    s_net_mode = -1;
    s_wifi_prov_connecting = false;
    s_wifi_prov_retry = 0;
    s_wifi_ignore_disconnect_once = false;
    s_mqtt_waiting = false;

    if (s_prov_timer) esp_timer_stop(s_prov_timer);
    esp_timer_start_once(s_prov_timer, (uint64_t)PROV_WINDOW_SEC * 1000000ULL);

    PROV_I("Provision window open for %d sec (step=%s)", PROV_WINDOW_SEC, prov_step_str(s_step));
    prov_start_advertising();
}

// -----------------------------
// Wi-Fi 事件：仅用于返回 statusWiFi
// -----------------------------
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        int reason = event ? event->reason : -1;
        PROV_I("Wi-Fi disconnected, reason=%d", reason);

        // 主动 stop/disconnect 触发的断开事件，忽略一次，避免误报 statusWiFi=2
        if (s_wifi_ignore_disconnect_once) {
            s_wifi_ignore_disconnect_once = false;
            return;
        }

        if (!s_wifi_prov_connecting) return;

        // 密码错：直接失败
        if (event && event->reason == WIFI_REASON_AUTH_FAIL) {
            s_wifi_prov_connecting = false;
            prov_set_step(PROV_STEP_WAIT_WIFI_CFG, "wifi auth fail");
            prov_send_kv_int("statusWiFi", 2);
            return;
        }

        // 其它原因：允许少量重试，避免瞬时断开就误判为失败
        if (s_wifi_prov_retry < PROV_WIFI_RETRY_MAX) {
            s_wifi_prov_retry++;
            PROV_I("Wi-Fi provisioning retry %d/%d", s_wifi_prov_retry, PROV_WIFI_RETRY_MAX);
            esp_wifi_connect();
            return;
        }

        s_wifi_prov_connecting = false;
        prov_set_step(PROV_STEP_WAIT_WIFI_CFG, "wifi retry exhausted");
        prov_send_kv_int("statusWiFi", 2);
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    (void)arg;
    (void)event_base;
    (void)event_data;

    if (event_id == IP_EVENT_STA_GOT_IP) {
        if (s_wifi_prov_connecting) {
            s_wifi_prov_connecting = false;
            prov_set_step(PROV_STEP_WAIT_MQTT_CFG, "wifi got ip");
            prov_send_kv_int("statusWiFi", 1);
        }
    }
}

// -----------------------------
// MQTT 事件：返回 statusMQTT
// -----------------------------
static void mqtt_timeout_cb(void *arg) {
    (void)arg;
    if (!s_mqtt_waiting) return;
    s_mqtt_waiting = false;
    prov_send_kv_int("statusMQTT", 1);
}

static void on_app_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    (void)arg;
    (void)event_data;
    if (event_base != APP_EVENTS) return;

    if (event_id == APP_EVENT_MQTT_PLAN_RECEIVED) {
        // 协议文档：连接成功（且已发送 init 并收到套餐 cmd）才算 0
        if (s_mqtt_waiting) {
            s_mqtt_waiting = false;
            if (s_mqtt_timer) esp_timer_stop(s_mqtt_timer);
            prov_send_kv_int("statusMQTT", 0);
        }
    } else if (event_id == APP_EVENT_MQTT_DISCONNECTED) {
        if (s_mqtt_waiting) {
            s_mqtt_waiting = false;
            if (s_mqtt_timer) esp_timer_stop(s_mqtt_timer);
            prov_send_kv_int("statusMQTT", 1);
        }
    }
}

// -----------------------------
// GATT Access 回调：接收 JSON
// -----------------------------
static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (!s_prov_window_open) {
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }

    uint8_t buf[PROV_JSON_MAX_LEN + 1];
    int len = om_to_buf(ctxt->om, buf, sizeof(buf));
    if (len <= 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    prov_handle_json(buf, len);
    return 0;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &g_prov_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &g_prov_rx_uuid.u,
                .access_cb = gatt_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &g_prov_tx_uuid.u,
                // NimBLE 要求 characteristic 必须提供 access_cb（即使仅用于 notify），否则 ble_gatts_count_cfg 可能返回 EINVAL。
                .access_cb = gatt_access_cb,
                .val_handle = &s_tx_val_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            {0}
        },
    },
    {0},
};

// -----------------------------
// JSON 处理（步骤门禁）
// -----------------------------
static void prov_handle_json(uint8_t *data, int len) {
    // data 已确保以 '\0' 结尾
    PROV_I("RX JSON (len=%d, step=%s, net_mode=%d): %s", len, prov_step_str(s_step), s_net_mode, (char *)data);

    jparse_ctx_t jctx;
    if (json_parse_start(&jctx, (char *)data, len) != 0) {
        return;
    }

    int ival = 0;

    // Step 1/5: statusBLE
    if (json_obj_get_int(&jctx, "statusBLE", &ival) == 0) {
        if (ival == 0) {
            if (s_step != PROV_STEP_WAIT_BLE_OPEN) {
                PROV_W("step mismatch for statusBLE=0, cur=%s", prov_step_str(s_step));
                json_parse_end(&jctx);
                return;
            }
            prov_send_kv_int("statusBLE", 0);
            prov_set_step(PROV_STEP_WAIT_NET_MODE, "statusBLE open");
            json_parse_end(&jctx);
            return;
        }
        if (ival == 1) {
            if (s_step != PROV_STEP_WAIT_BLE_CLOSE) {
                PROV_W("step mismatch for statusBLE=1, cur=%s", prov_step_str(s_step));
                json_parse_end(&jctx);
                return;
            }
            prov_send_kv_int("statusBLE", 1);
            prov_set_step(PROV_STEP_DONE, "statusBLE close");
            // 提前关闭配网窗口：停止广播并停止计时器
            s_prov_window_open = false;
            if (s_prov_timer) esp_timer_stop(s_prov_timer);
            prov_stop_advertising();
            // 主动断开 BLE（对齐文档“请求关闭”）
            if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            json_parse_end(&jctx);
            return;
        }
    }

    // Step 2: statusNet
    if (json_obj_get_int(&jctx, "statusNet", &ival) == 0) {
        if (s_step != PROV_STEP_WAIT_NET_MODE) {
            PROV_W("step mismatch for statusNet, cur=%s", prov_step_str(s_step));
            json_parse_end(&jctx);
            return;
        }

        s_net_mode = ival;
        net_config_t cfg;
        if (app_storage_load_net_config(&cfg) != ESP_OK) memset(&cfg, 0, sizeof(cfg));
        cfg.mode = s_net_mode;
        app_storage_save_net_config(&cfg);

        prov_send_kv_int("statusNet", 1);
        app_events_post_net_mode_request(s_net_mode);

        if (s_net_mode == 0) {
            prov_set_step(PROV_STEP_WAIT_WIFI_CFG, "net mode=wifi");
        } else {
            prov_set_step(PROV_STEP_WAIT_MQTT_CFG, "net mode=4g");
        }

        json_parse_end(&jctx);
        return;
    }

    // Step 3: WiFi 配置（仅 WiFi 模式）
    char ssid[33] = {0};
    char password[65] = {0};
    int has_ssid = (json_obj_get_string(&jctx, "ssid", ssid, sizeof(ssid)) == 0);
    int has_pwd  = (json_obj_get_string(&jctx, "password", password, sizeof(password)) == 0);
    // 注意：MQTT 配置同样包含字段 "password"，
    // WiFi 配置必须带 ssid 才认为是 WiFi 配置，避免把 MQTT JSON 误判成 WiFi JSON。
    if (has_ssid) {
        if (s_step != PROV_STEP_WAIT_WIFI_CFG || s_net_mode != 0) {
            PROV_W("step mismatch for wifi cfg, cur=%s net_mode=%d", prov_step_str(s_step), s_net_mode);
            json_parse_end(&jctx);
            return;
        }
        if (!has_ssid || !has_pwd) {
            PROV_W("wifi cfg missing field: has_ssid=%d has_pwd=%d", has_ssid, has_pwd);
            json_parse_end(&jctx);
            return;
        }

        // 保存到 NVS
        net_config_t cfg;
        if (app_storage_load_net_config(&cfg) != ESP_OK) memset(&cfg, 0, sizeof(cfg));
        cfg.mode = 0;
        strncpy(cfg.ssid, ssid, sizeof(cfg.ssid) - 1);
        strncpy(cfg.password, password, sizeof(cfg.password) - 1);
        app_storage_save_net_config(&cfg);

        // 开始连接 Wi-Fi：statusWiFi=0
        s_wifi_prov_connecting = true;
        s_wifi_prov_retry = 0;
        prov_set_step(PROV_STEP_WAIT_WIFI_RESULT, "wifi connect start");
        prov_send_kv_int("statusWiFi", 0);

        wifi_config_t sta_config = {0};
        strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
        strncpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password) - 1);

        // 关键：避免 “sta is connecting, cannot set config”
        s_wifi_ignore_disconnect_once = true;
        esp_wifi_stop();
        esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        esp_wifi_start();
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            PROV_E("esp_wifi_connect failed: %s", esp_err_to_name(err));
        }

        json_parse_end(&jctx);
        return;
    }

    // Step 4: MQTT 配置
    char mqtt_server_buf[64] = {0};
    if (json_obj_get_string(&jctx, "mqttserver", mqtt_server_buf, sizeof(mqtt_server_buf)) == 0) {
        if (s_step != PROV_STEP_WAIT_MQTT_CFG) {
            PROV_W("step mismatch for mqtt cfg, cur=%s", prov_step_str(s_step));
            json_parse_end(&jctx);
            return;
        }

        char username_buf[64] = {0};
        char password_buf[64] = {0};
        json_obj_get_string(&jctx, "username", username_buf, sizeof(username_buf));
        json_obj_get_string(&jctx, "password", password_buf, sizeof(password_buf));

        net_config_t cfg;
        if (app_storage_load_net_config(&cfg) != ESP_OK) memset(&cfg, 0, sizeof(cfg));

        char *colon = strchr(mqtt_server_buf, ':');
        if (colon) {
            *colon = '\0';
            strncpy(cfg.mqtt_host, mqtt_server_buf, sizeof(cfg.mqtt_host) - 1);
            cfg.mqtt_port = atoi(colon + 1);
        } else {
            strncpy(cfg.mqtt_host, mqtt_server_buf, sizeof(cfg.mqtt_host) - 1);
            cfg.mqtt_port = 1883;
        }

        strncpy(cfg.username, username_buf, sizeof(cfg.username) - 1);
        strncpy(cfg.password_mqtt, password_buf, sizeof(cfg.password_mqtt) - 1);
        snprintf(cfg.full_url, sizeof(cfg.full_url), "mqtt://%s:%d", cfg.mqtt_host, cfg.mqtt_port);

        app_storage_save_net_config(&cfg);
        app_storage_set_pending_init(1);
        app_events_post_mqtt_config_updated();

        // 等待 MQTT 结果反馈
        s_mqtt_waiting = true;
        if (s_mqtt_timer) {
            esp_timer_stop(s_mqtt_timer);
            esp_timer_start_once(s_mqtt_timer, 20000000ULL); // 20s 超时
        }

        prov_set_step(PROV_STEP_WAIT_BLE_CLOSE, "mqtt cfg received");
        json_parse_end(&jctx);
        return;
    }

    json_parse_end(&jctx);
}

// -----------------------------
// GAP 事件
// -----------------------------
static int gap_event_cb(struct ble_gap_event *event, void *arg) {
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_notify_enabled = false; // 等待 subscribe 事件
            PROV_I("BLE connected; conn_handle=%d", s_conn_handle);
        } else {
            PROV_W("BLE connect failed; status=%d", event->connect.status);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            if (s_prov_window_open) prov_start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        PROV_I("BLE disconnected; reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_notify_enabled = false;
        prov_set_step(PROV_STEP_WAIT_BLE_OPEN, "ble disconnected"); // 下次连接重新走流程
        s_net_mode = -1;
        s_wifi_prov_connecting = false;
        s_wifi_prov_retry = 0;
        s_wifi_ignore_disconnect_once = false;
        s_mqtt_waiting = false;
        if (s_mqtt_timer) esp_timer_stop(s_mqtt_timer);

        if (s_prov_window_open) prov_start_advertising();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_val_handle) {
            s_notify_enabled = event->subscribe.cur_notify;
            PROV_I("subscribe tx notify=%d", s_notify_enabled);
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        PROV_I("mtu update; conn_handle=%d mtu=%d", event->mtu.conn_handle, event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

// -----------------------------
// Advertising
// -----------------------------
static void prov_start_advertising(void) {
    if (!s_prov_window_open) return;
    if (ble_gap_adv_active()) return;

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        PROV_E("ble_gap_adv_set_fields rc=%d", rc);
        return;
    }

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, gap_event_cb, NULL);
    if (rc != 0) {
        PROV_E("ble_gap_adv_start rc=%d", rc);
        return;
    }
    PROV_I("advertising started");
}

static void prov_stop_advertising(void) {
    if (ble_gap_adv_active()) {
        ble_gap_adv_stop();
    }
}

// -----------------------------
// NimBLE 回调
// -----------------------------
static void on_reset(int reason) {
    PROV_E("NimBLE reset; reason=%d", reason);
}

static void on_sync(void) {
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        PROV_E("ble_hs_id_infer_auto rc=%d", rc);
        return;
    }
    prov_window_open();
}

static void host_task(void *param) {
    (void)param;
    PROV_I("BLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// -----------------------------
// 对外接口
// -----------------------------
esp_err_t blufi_custom_init(void) {
    esp_err_t ret = ESP_OK;
    PROV_I("blufi_custom_init: start (BLE JSON provisioning, %ds window)", PROV_WINDOW_SEC);
    // 注册 Wi-Fi/IP 事件，用于 statusWiFi
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL));

    // MQTT 状态反馈事件
    ESP_ERROR_CHECK(esp_event_handler_register(APP_EVENTS, APP_EVENT_MQTT_PLAN_RECEIVED, &on_app_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(APP_EVENTS, APP_EVENT_MQTT_DISCONNECTED, &on_app_event, NULL));

    // 配网窗口 timer
    if (!s_prov_timer) {
        esp_timer_create_args_t tcfg = {
            .callback = &prov_window_close_cb,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "prov_win",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&tcfg, &s_prov_timer));
    }

    // MQTT 超时 timer（可选）
    if (!s_mqtt_timer) {
        esp_timer_create_args_t mcfg = {
            .callback = &mqtt_timeout_cb,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "mqtt_to",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&mcfg, &s_mqtt_timer));
    }

    // 初始化 BLE Controller（NimBLE Host 依赖 Controller 提供 HCI）
    // 之前 BLUFI 版本这里做过 controller init/enable；去掉 BLUFI 后仍需保留。
#if CONFIG_BT_CONTROLLER_ENABLED
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        PROV_E("esp_bt_controller_init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK) {
        PROV_E("esp_bt_controller_enable failed: %s", esp_err_to_name(ret));
        return ret;
    }
#endif

    // 初始化 NimBLE
    PROV_I("init nimble host...");
    ret = esp_nimble_init();
    if (ret != ESP_OK) {
        PROV_E("esp_nimble_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    // 设备名（用于广播）
    ble_svc_gap_device_name_set("WATER_PROV");

    // 初始化默认 GAP/GATT 服务（0x1800/0x1801）
    ble_svc_gap_init();
    ble_svc_gatt_init();

    // 注册自定义服务
    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) {
        PROV_E("ble_gatts_count_cfg failed rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) {
        PROV_E("ble_gatts_add_svcs failed rc=%d", rc);
        return ESP_FAIL;
    }

    // 初始化 store（即使不做 bonding，也建议初始化，避免部分行为差异）
    ble_store_config_init();

    // 启动 host task（使用 ESP-IDF 封装，避免与 esp_nimble_init 初始化流程冲突）
    PROV_I("enable nimble host task...");
    ret = esp_nimble_enable(host_task);
    if (ret != ESP_OK) {
        PROV_E("esp_nimble_enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    PROV_I("BLE provisioning service init done");
    return ESP_OK;
}

void blufi_custom_deinit(void) {
    // 停止广播/断开连接
    s_prov_window_open = false;
    prov_stop_advertising();
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }

    if (s_prov_timer) esp_timer_stop(s_prov_timer);
    if (s_mqtt_timer) esp_timer_stop(s_mqtt_timer);
}

void blufi_send_custom_result(bool success, const char *msg) {
    // 保留旧接口：简单封装为 JSON 返回
    if (!msg) msg = "";
    char payload[PROV_JSON_MAX_LEN];
    snprintf(payload, sizeof(payload), "{\"ok\":%d,\"msg\":\"%s\"}", success ? 1 : 0, msg);
    prov_send_json_raw(payload);
}

void blufi_send_mqtt_status(int status) {
    // 保留旧接口：按文档字段名 statusMQTT
    prov_send_kv_int("statusMQTT", status);
}
