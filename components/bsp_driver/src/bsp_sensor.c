#include "bsp_sensor.h"
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_events.h"
#include <math.h>
#include "bsp_pump_valve.h"

static const char *TAG = "BSP_SENSOR";

// --- 引脚定义 ---
#define GPIO_SW_LOW_PRESS  35
#define GPIO_SW_HIGH_PRESS 14
#define GPIO_FLOW_METER    23 // 假设流量计接在 IO23

// --- ADC 通道映射 (基于 ADC1) ---
#define ADC_CHAN_TDS_OUT    ADC_CHANNEL_0 // IO 36 (VP)
#define ADC_CHAN_TDS_BACKUP ADC_CHANNEL_4 // IO 32
#define ADC_CHAN_TDS_IN     ADC_CHANNEL_5 // IO 33
#define ADC_CHAN_TEMP_NTC   ADC_CHANNEL_6 // IO 34

static adc_oneshot_unit_handle_t s_adc1_handle;
static pcnt_unit_handle_t s_pcnt_unit;

// --- 状态缓存 ---
static bool s_last_low_press = true;  // 假设初始正常
static bool s_last_high_press = false; // 假设初始未满

// ==========================================
// 1. 高低压开关消抖任务 (取代 ISR 中断)
// ==========================================
static void switch_debounce_task(void *arg) {
    int low_debounce_cnt = 0;
    int high_debounce_cnt = 0;
    const int DEBOUNCE_THRESHOLD = 3; // 连续 3 次 (150ms) 电平一致才确认为有效动作

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(50)); // 50ms 轮询周期

        bool curr_low = (gpio_get_level(GPIO_SW_LOW_PRESS) == 0);   // 假设低电平=缺水
        bool curr_high = (gpio_get_level(GPIO_SW_HIGH_PRESS) == 1); // 假设高电平=水满

        // --- 低压开关消抖 ---
        if (curr_low != s_last_low_press) {
            low_debounce_cnt++;
            if (low_debounce_cnt >= DEBOUNCE_THRESHOLD) {
                s_last_low_press = curr_low;
                low_debounce_cnt = 0;
                ESP_LOGW(TAG, "低压开关触发: %s", curr_low ? "缺水报警" : "水压恢复");
                esp_event_post(APP_EVENTS, 
                    curr_low ? APP_EVENT_HW_LOW_PRESSURE_ALARM : APP_EVENT_HW_LOW_PRESSURE_RECOVER, 
                    NULL, 0, portMAX_DELAY);
            }
        } else {
            low_debounce_cnt = 0;
        }

        // --- 高压开关消抖 ---
        if (curr_high != s_last_high_press) {
            high_debounce_cnt++;
            if (high_debounce_cnt >= DEBOUNCE_THRESHOLD) {
                s_last_high_press = curr_high;
                high_debounce_cnt = 0;
                ESP_LOGI(TAG, "高压开关触发: %s", curr_high ? "储水桶满" : "请求制水");
                esp_event_post(APP_EVENTS, 
                    curr_high ? APP_EVENT_HW_HIGH_PRESSURE_ALARM : APP_EVENT_HW_HIGH_PRESSURE_RECOVER, 
                    NULL, 0, portMAX_DELAY);
            }
        } else {
            high_debounce_cnt = 0;
        }
    }
}

// ==========================================
// 2. 传感器初始化
// ==========================================
void bsp_sensor_init(void) {
    // 1. 初始化 GPIO (高低压开关)
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE, // 使用轮询，关闭中断
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL<<GPIO_SW_LOW_PRESS) | (1ULL<<GPIO_SW_HIGH_PRESS),
        .pull_up_en = 0,
         .pull_down_en = 0
    };
    gpio_config(&io_conf);
    xTaskCreate(switch_debounce_task, "sw_deb_tsk", 2048, NULL, 10, NULL);

    // 2. 初始化 ADC1 (TDS & NTC)
    adc_oneshot_unit_init_cfg_t init_config = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &s_adc1_handle));

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_11 // 支持测量 0 ~ 3.3V
    };
    adc_oneshot_config_channel(s_adc1_handle, ADC_CHAN_TDS_OUT, &config);
    adc_oneshot_config_channel(s_adc1_handle, ADC_CHAN_TDS_BACKUP, &config);
    adc_oneshot_config_channel(s_adc1_handle, ADC_CHAN_TDS_IN, &config);
    adc_oneshot_config_channel(s_adc1_handle, ADC_CHAN_TEMP_NTC, &config);

    // 3. 初始化 PCNT 流量计脉冲计数
    pcnt_unit_config_t unit_config = {
        .high_limit = 30000,
        .low_limit = -1,
    };
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_config, &s_pcnt_unit));
    
    pcnt_chan_config_t chan_config = {
        .edge_gpio_num = GPIO_FLOW_METER,
        .level_gpio_num = -1, // 不受电平控制
    };
    pcnt_channel_handle_t pcnt_chan = NULL;
    ESP_ERROR_CHECK(pcnt_new_channel(s_pcnt_unit, &chan_config, &pcnt_chan));
    // 仅在上升沿计数值增加
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(pcnt_chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD));
    
    ESP_ERROR_CHECK(pcnt_unit_enable(s_pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(s_pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_start(s_pcnt_unit));

    ESP_LOGI(TAG, "传感器子系统初始化完成");
}

/ ==========================================
// 3. 算法计算逻辑 (NTC 独立，TDS 批处理防浪涌)
// ==========================================

// 缓存 TDS 读取的值，防止频繁唤醒硬件
static int s_raw_tds_in = 0;
static int s_raw_tds_out = 0;
static int s_raw_tds_backup = 0;
static uint32_t s_last_read_ticks = 0;

// --- NTC 温度独立读取 (随时可用，不受 IO15 限制) ---
float bsp_sensor_get_temperature(void) {
    int raw_temp;
    // 直接进行瞬间采样，无需控制电源
    adc_oneshot_read(s_adc1_handle, ADC_CHAN_TEMP_NTC, &raw_temp);
    
    // TODO: 根据 raw_temp 和您的 10k NTC 电路分压比例、B值(如3950)计算真实温度
    // 这里做个示例占位：假设返回 25.0 摄氏度
    return 25.0f; 
}

// --- TDS 统一批处理读取：一次通电，读取全部 3 个 TDS ---
static void bsp_sensor_read_tds_batch(void) {
    // 限制读取频率（1 秒内重复调用直接返回，防止密集轰炸硬件 LDO）
    if (xTaskGetTickCount() - s_last_read_ticks < pdMS_TO_TICKS(1000) && s_last_read_ticks != 0) {
        return; 
    }

    // 1. 打开 TDS 传感器专供电源 (IO15 = High)
    bsp_set_sensor_power(true);          
    
    // 2. 延时 20ms，等待运放稳定并给 LDO 后级电容充饱电
    vTaskDelay(pdMS_TO_TICKS(20));       
    
    // 3. 瞬间抓取 3 个 TDS 通道的数据 (去除了 NTC)
    adc_oneshot_read(s_adc1_handle, ADC_CHAN_TDS_IN, &s_raw_tds_in);
    adc_oneshot_read(s_adc1_handle, ADC_CHAN_TDS_OUT, &s_raw_tds_out);
    adc_oneshot_read(s_adc1_handle, ADC_CHAN_TDS_BACKUP, &s_raw_tds_backup);
    
    // 4. 立刻断电防极化腐蚀 (IO15 = Low)
    bsp_set_sensor_power(false);         
    
    s_last_read_ticks = xTaskGetTickCount();
}

// --- 核心计算 ---
static int calculate_tds(int raw_adc) {
    float voltage = (float)raw_adc * 3.3f / 4095.0f;
    
    // 获取实时温度（现在它非常快，不会引发 IO15 动作）
    float temp = bsp_sensor_get_temperature(); 
    
    // 核心：温度补偿公式 (每升高1度，电导率增加约 2%)
    float comp_voltage = voltage / (1.0f + 0.02f * (temp - 25.0f));
    
    // TODO: 根据您的探头 K 值进行计算
    int tds_value = (int)(comp_voltage * 100.0f); // 占位系数
    return (tds_value > 0) ? tds_value : 0;
}

int bsp_sensor_get_tds_in(void) { 
    bsp_sensor_read_tds_batch();
    return calculate_tds(s_raw_tds_in); 
}

int bsp_sensor_get_tds_out(void) { 
    bsp_sensor_read_tds_batch();
    return calculate_tds(s_raw_tds_out); 
}

int bsp_sensor_get_tds_backup(void) { 
    bsp_sensor_read_tds_batch();
    return calculate_tds(s_raw_tds_backup); 
}

uint32_t bsp_sensor_get_flow_pulses(void) {
    int count = 0;
    pcnt_unit_get_count(s_pcnt_unit, &count);
    return (uint32_t)count;
}

void bsp_sensor_clear_flow_pulses(void) {
    pcnt_unit_clear_count(s_pcnt_unit);
}