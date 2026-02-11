#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_modem_api.h"
#include "esp_log.h"
#include "esp_event.h"  // 必须包含
#include "modem_4g.h"

static const char *TAG = "MODEM_4G";

// --- 引脚定义 (请根据原理图修改) ---
#define MODEM_TX_PIN        17
#define MODEM_RX_PIN        18
#define MODEM_RST_PIN       5
#define MODEM_PWR_PIN       4
#define MODEM_UART_PORT     UART_NUM_1

// APN 设置
#define MODEM_APN           "cmnet" 
#define MODEM_PPP_USER      ""
#define MODEM_PPP_PASSWORD  ""

static esp_modem_dce_t *s_dce = NULL;
static esp_netif_t *s_esp_netif = NULL;

// 【修复】监听 IP/PPP 事件，而不是 ESP_MODEM_EVENT
static void on_ppp_changed(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data)
{
    if (event_id == NETIF_PPP_ERRORNONE) {
        ESP_LOGI(TAG, "PPP Connected (Phase Authenticate/Running)");
    } else if (event_id == NETIF_PPP_PHASE_DEAD) {
        ESP_LOGW(TAG, "PPP Connection Dead");
    }
}

static void on_ip_event(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data)
{
    if (event_id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Modem Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(TAG, "Modem Lost IP");
    }
}

void modem_4g_init(void) {
    if (s_dce != NULL) {
        ESP_LOGW(TAG, "Modem already initialized");
        return;
    }

    ESP_LOGI(TAG, "Initializing 4G Modem...");

    // 1. 初始化网络接口 (PPP)
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_PPP();
    s_esp_netif = esp_netif_new(&cfg);
    assert(s_esp_netif);

    // 2. 配置 DTE (UART 参数)
    esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_config.uart_config.tx_io_num = MODEM_TX_PIN;
    dte_config.uart_config.rx_io_num = MODEM_RX_PIN;
    dte_config.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_NONE; 
    dte_config.uart_config.port_num = MODEM_UART_PORT;
    dte_config.task_stack_size = 4096; // 稍微调大栈空间

    // 3. 配置 DCE (设备参数)
    esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG(MODEM_APN);

    // 4. 创建 Modem 对象
    s_dce = esp_modem_new(&dte_config, &dce_config, s_esp_netif);
    assert(s_dce);

    // 5. 注册事件监听 (可选，方便调试)
    esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP, &on_ip_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_LOST_IP, &on_ip_event, NULL);
    // 监听 PPP 底层状态 (需要 include esp_netif_ppp.h)
    esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, &on_ppp_changed, NULL);

    ESP_LOGI(TAG, "Modem Initialized.");
}

void modem_4g_start(void) {
    if (!s_dce) {
        ESP_LOGE(TAG, "Modem not initialized!");
        return;
    }
    // 启动 CMUX 模式拨号，如果失败则回退到 DATA 模式
    // 注意：部分模组（如 EC20）默认支持 CMUX，部分需要先发 AT 开启
    esp_err_t err = esp_modem_set_mode(s_dce, ESP_MODEM_MODE_CMUX);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CMUX failed, fallback to DATA mode");
        esp_modem_set_mode(s_dce, ESP_MODEM_MODE_DATA);
    }
    ESP_LOGI(TAG, "Modem Start Dialing...");
}

void modem_4g_stop(void) {
    if (s_dce) {
        esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND);
    }
}