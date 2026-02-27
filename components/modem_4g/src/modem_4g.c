#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_modem_api.h"
#include "esp_log.h"
#include "esp_event.h"  // 必须包含
#include "modem_4g.h"
#include "driver/gpio.h"    

static const char *TAG = "MODEM_4G";


#define MODEM_TX_PIN        26
#define MODEM_RX_PIN        27
#define MODEM_CTS_PIN       25
#define MODEM_RTS_PIN       18

#define MODEM_RST_PIN       5
#define MODEM_PWR_PIN       2
#define MODEM_UART_PORT     UART_NUM_1

// APN 设置
// #define MODEM_APN           "cmnet"
#define MODEM_APN           "iot.10086.cn" 
#define MODEM_PPP_USER      ""
#define MODEM_PPP_PASSWORD  ""

static esp_modem_dce_t *s_dce = NULL;
static esp_netif_t *s_esp_netif = NULL;

// IP/PPP 事件，而不是 ESP_MODEM_EVENT
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

// 硬件开机/复位时序控制
static void modem_power_on(void) {
    ESP_LOGI(TAG, "Initializing Modem Power sequence...");

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << MODEM_PWR_PIN) | (1ULL << MODEM_RST_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = 0,
        .pull_up_en = 0,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    // 1. 复位引脚初始保持高电平 (外部有上拉)
    gpio_set_level(MODEM_RST_PIN, 1);
    
    // 2. PWRKEY 初始保持低电平 (三极管截止，PWRKEY 悬空/内部上拉)
    gpio_set_level(MODEM_PWR_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    // 3. 复位模组 (拉低 100ms)
    ESP_LOGI(TAG, "Resetting Air780E...");
    gpio_set_level(MODEM_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(MODEM_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(200));

    // 4. 控制 PWRKEY 开机
    // Air780E 硬件手册要求 PWRKEY 拉低(三极管导通)至少 1.2 秒才能开机
    ESP_LOGI(TAG, "Powering on Air780E...");
    gpio_set_level(MODEM_PWR_PIN, 1); // 给高电平，三极管导通，拉低 PWRKEY
    vTaskDelay(pdMS_TO_TICKS(1500));  // 维持 1.5 秒
    gpio_set_level(MODEM_PWR_PIN, 0); // 恢复低电平，三极管截止

    // 等待模组系统启动完成并驻网
    ESP_LOGI(TAG, "Waiting for Air780E to boot...");
    vTaskDelay(pdMS_TO_TICKS(3000)); 
}

void modem_4g_init(void) {
    if (s_dce != NULL) {
        ESP_LOGW(TAG, "Modem already initialized");
        return;
    }

    ESP_LOGI(TAG, "Initializing 4G Modem...");
    // 0. 控制 PWRKEY 开机
    modem_power_on();

    // 1. 初始化网络接口 (PPP)
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_PPP();
    s_esp_netif = esp_netif_new(&cfg);
    assert(s_esp_netif);

    // 2. 配置 DTE (UART 参数)
    esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_config.uart_config.tx_io_num = MODEM_TX_PIN;
    dte_config.uart_config.rx_io_num = MODEM_RX_PIN;
    // dte_config.uart_config.cts_io_num = MODEM_CTS_PIN;
    // dte_config.uart_config.rts_io_num = MODEM_RTS_PIN;
    dte_config.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_NONE; 
    //dte_config.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_HW; 
    dte_config.uart_config.port_num = MODEM_UART_PORT;
    dte_config.uart_config.baud_rate = 115200;
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
    ESP_LOGI(TAG, "================ AT COMMAND TESTS ================");

    // 1. 同步模组 (内部会连续发送 AT，直到模组回复 OK)
    // 这是测试串口连线、波特率和模组是否正常启动的最基本测试
    if (esp_modem_sync(s_dce) != ESP_OK) {
        ESP_LOGE(TAG, "Modem sync failed! Check UART connections and power.");
        // 如果连最基础的 AT 都没回复，就没有拨号的必要了，直接返回
        return; 
    }
    ESP_LOGI(TAG, "Modem sync OK (AT responds).");

    // 2. 获取 IMEI (模组串号) - 验证模组核心工作正常
    char imei[32] = {0};
    if (esp_modem_get_imei(s_dce, imei) == ESP_OK) {
        ESP_LOGI(TAG, "Modem IMEI: %s", imei);
    } else {
        ESP_LOGE(TAG, "Failed to get IMEI");
    }

    // 3. 获取 IMSI (SIM卡串号) - 验证 SIM 卡是否插好、是否能正常读取
    char imsi[32] = {0};
    if (esp_modem_get_imsi(s_dce, imsi) == ESP_OK) {
        ESP_LOGI(TAG, "SIM IMSI (SIM Card OK): %s", imsi);
    } else {
        ESP_LOGW(TAG, "Failed to get IMSI, please check SIM card!");
    }

    // 4. 获取信号质量 (CSQ) - 验证天线是否接好 (rssi范围通常是 0-31，99表示无信号)
    int rssi = 0, ber = 0;
    if (esp_modem_get_signal_quality(s_dce, &rssi, &ber) == ESP_OK) {
        ESP_LOGI(TAG, "Signal Quality: rssi=%d, ber=%d", rssi, ber);
        if (rssi == 99) {
            ESP_LOGW(TAG, "WARNING: No signal or poor antenna connection!");
        }
    } else {
        ESP_LOGW(TAG, "Failed to get Signal Quality");
    }
    
    // 5. [可选] 使用原生 AT 接口发送自定义指令 (查询 4G 网络注册状态)
    // +CEREG: 0,1 或 +CEREG: 0,5 都表示注册网络成功
    char cmd_res[128] = {0};
    if (esp_modem_at(s_dce, "AT+CEREG?\r\n", cmd_res, 2000) == ESP_OK) {
        // 由于返回带有换行符，手动处理一下打印格式
        char *clean_res = strtok(cmd_res, "\r\n");
        if (clean_res) {
            ESP_LOGI(TAG, "Network Registration (AT+CEREG?): %s", clean_res);
        }
    }

    ESP_LOGI(TAG, "==================================================");

    ESP_LOGI(TAG, "Modem Start Dialing...");
    
    // 6. 启动数据模式 (切换为透传/PPP拨号模式)
    esp_err_t err = esp_modem_set_mode(s_dce, ESP_MODEM_MODE_DATA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to switch to DATA mode for dialing.");
    }
}

void modem_4g_stop(void) {
    if (s_dce) {
        esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND);
        ESP_LOGI(TAG, "Modem stopped (switched to command mode).");
    }
}