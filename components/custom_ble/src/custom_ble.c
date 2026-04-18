#include "custom_ble.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "cJSON.h"
#include "esp_wifi.h"       // 用于配网
#include "app_storage.h"    // 您的 NVS 存储接口
#include "esp_event.h"      // 用于抛出状态机事件

static const char *TAG = "CUSTOM_BLE";

// 保存当前连接的句柄，用于 Notify 推送
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

// 声明处理函数
static int custom_ble_rx_handler(uint16_t conn_handle, uint16_t attr_handle, 
                                 struct ble_gatt_access_ctxt *ctxt, void *arg);

// =============================================================
// 1. GATT 路由表定义
// =============================================================
static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(CUSTOM_BLE_SVC_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                // RX: 接收小程序数据 (支持 Write 和 Write No Response)
                .uuid = BLE_UUID16_DECLARE(CUSTOM_BLE_CHR_RX_UUID),
                .access_cb = custom_ble_rx_handler,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                // TX: 向小程序发送数据 (Notify)
                .uuid = BLE_UUID16_DECLARE(CUSTOM_BLE_CHR_TX_UUID),
                .access_cb = custom_ble_rx_handler, // 读操作也走这个回调（按需处理）
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            {0} // 结束符
        }
    },
    {0} // 结束符
};

// =============================================================
// 2. 接收小程序下发数据的解析中枢 (防 20 字节截断、防缺 \0)
// =============================================================
static int custom_ble_rx_handler(uint16_t conn_handle, uint16_t attr_handle, 
                                 struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return 0; // 忽略非写操作
    }

    // 巨坑一与巨坑二的完美解法：提取真实长度，并手动添加 '\0'
    uint16_t rx_len = OS_MBUF_PKTLEN(ctxt->om);
    if (rx_len == 0 || rx_len > 1024) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    char *json_buf = (char *)malloc(rx_len + 1);
    if (!json_buf) {
        ESP_LOGE(TAG, "内存不足，无法接收蓝牙数据");
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    // 拷贝 mbuf 里的裸字节流
    os_mbuf_copydata(ctxt->om, 0, rx_len, json_buf);
    json_buf[rx_len] = '\0'; // 【极其关键】赋予 C 字符串的灵魂

    ESP_LOGI(TAG, "收到小程序 BLE 数据: %s", json_buf);

    // ==========================================
    // 开始解析 JSON 并分发业务逻辑
    // ==========================================
    cJSON *root = cJSON_Parse(json_buf);
    if (root) {
        cJSON *method_item = cJSON_GetObjectItem(root, "method");
        
        if (method_item && cJSON_IsNumber(method_item)) {
            int method = method_item->valueint;
            
            // ---> 业务 1: Wi-Fi 配网 (假设您表格中配网 method 为 1)
            if (method == 1) {
                cJSON *param = cJSON_GetObjectItem(root, "param");
                cJSON *ssid = cJSON_GetObjectItem(param, "ssid");
                cJSON *pwd = cJSON_GetObjectItem(param, "password");
                
                if (ssid && pwd) {
                    ESP_LOGW(TAG, "执行配网 -> SSID: %s, PWD: %s", ssid->valuestring, pwd->valuestring);
                    
                    wifi_config_t wifi_config = {0};
                    strncpy((char *)wifi_config.sta.ssid, ssid->valuestring, sizeof(wifi_config.sta.ssid) - 1);
                    strncpy((char *)wifi_config.sta.password, pwd->valuestring, sizeof(wifi_config.sta.password) - 1);
                    
                    esp_wifi_disconnect();
                    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
                    esp_wifi_connect();
                    
                    // 回复小程序：收到配网指令
                    custom_ble_send_notify("{\"event\":\"wifi_connecting\"}");
                }
            }
            // ---> 业务 2: 下发净水器套餐 (您之前提供的 method 2)
            else if (method == 2) {
                cJSON *param = cJSON_GetObjectItem(root, "param");
                if (param) {
                    device_status_t status;
                    app_storage_load_status(&status); // 读取原有数据防止全覆盖
                    
                    // 提取并更新参数
                    cJSON *cap = cJSON_GetObjectItem(param, "capacity");
                    if(cap) status.capacity = cap->valueint;
                    
                    cJSON *f1 = cJSON_GetObjectItem(param, "filter01");
                    if(f1) status.filter01 = f1->valueint;
                    
                    // ... (补充 f2 到 f5 的解析) ...

                    app_storage_save_status(&status); // 真正落盘
                    ESP_LOGI(TAG, "蓝牙下发套餐更新成功，已写入 NVS！");

                    // 通知状态机重新鉴权 (解除缺水/欠费锁定)
                    esp_event_post(APP_EVENTS, APP_EVENT_CMD_EVALUATE, NULL, 0, 0);
                    
                    custom_ble_send_notify("{\"event\":\"plan_updated\",\"code\":200}");
                }
            }
        }
        cJSON_Delete(root);
    } else {
        ESP_LOGE(TAG, "JSON 解析失败!");
    }

    free(json_buf);
    return 0; // 必须返回 0 表示底层接收处理完毕
}

// =============================================================
// 3. 向小程序发送数据的 API
// =============================================================
int custom_ble_send_notify(const char *json_str) {
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(TAG, "未连接手机，取消 Notify");
        return -1;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(json_str, strlen(json_str));
    if (!om) {
        return -1;
    }

    // 查找我们的 TX 特征句柄
    uint16_t attr_handle;
    int rc = ble_gatts_find_chr(BLE_UUID16_DECLARE(CUSTOM_BLE_SVC_UUID),
                                BLE_UUID16_DECLARE(CUSTOM_BLE_CHR_TX_UUID),
                                NULL, &attr_handle);
    if (rc == 0) {
        rc = ble_gatts_notify_custom(s_conn_handle, attr_handle, om);
        if (rc != 0) {
            ESP_LOGE(TAG, "BLE Notify 发送失败: %d", rc);
        }
    }
    return rc;
}

// =============================================================
// 4. 连接状态回调与广播管理
// =============================================================
static int custom_ble_gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                ESP_LOGI(TAG, "微信小程序已连接!");
                s_conn_handle = event->connect.conn_handle;
            } else {
                ESP_LOGW(TAG, "连接失败，重启广播...");
                // 启动广播逻辑 (略，可单独封装 start_adv)
            }
            break;
            
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "微信小程序已断开连接!");
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            // 可以在此处重新启动蓝牙广播，等待下次连接
            break;
            
        case BLE_GAP_EVENT_MTU:
            // 解决巨坑一：见证小程序申请的大包传输
            ESP_LOGI(TAG, "MTU 协商成功，当前 MTU = %d", event->mtu.value);
            break;
    }
    return 0;
}

// =============================================================
// 5. 初始化入口
// =============================================================
void custom_ble_gatts_init(void) {
    int rc;
    
    // 1. 注册 GATT 服务表
    ble_gatts_count_cfg(gatt_svr_svcs);
    ble_gatts_add_svcs(gatt_svr_svcs);
    
    // 2. 注册连接事件回调 (非常重要，否则连不上)
    // 通常您需要在 sync_cb 里调用 ble_gap_adv_start，并在参数里把 custom_ble_gap_event 传进去
    ESP_LOGI(TAG, "自定义 BLE GATT 服务加载完成");
}