#include "bsp_led.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "BSP_LED";

// I2C 引脚定义
#define I2C_MASTER_SDA_IO           21
#define I2C_MASTER_SCL_IO           22
#define I2C_MASTER_NUM              I2C_NUM_0
#define I2C_MASTER_FREQ_HZ          100000

// TM1650 I2C 7-bit 地址映射
#define TM1650_CMD_SYS_CTRL         (0x48 >> 1) 
#define TM1650_CMD_DIG1             (0x68 >> 1) 
#define TM1650_CMD_DIG2             (0x6A >> 1) 

// --- 状态缓存 ---
static led_alarm_state_t s_alarm_states[5] = {LED_ALARM_OFF}; // index 1~4 对应 LED1~4
static led_work_mode_t s_work_mode = LED_MODE_STANDBY;
static bool s_i2c_ready = false;
static bool s_anim_task_started = false;

// I2C 写入函数
static esp_err_t write_tm1650(uint8_t addr_7bit, uint8_t data) {
    if (!s_i2c_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_write_to_device(I2C_MASTER_NUM, addr_7bit, &data, 1, pdMS_TO_TICKS(10));
}

// =========================================================
// 后台动画刷新任务 (核心逻辑)
// =========================================================
static void led_animation_task(void *arg) {
    uint32_t tick = 0;
    
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(150)); // 150ms 刷新一帧
        tick++;
        
        uint8_t dig1 = 0x00;
        uint8_t dig2 = 0x00;
        
        bool blink_flag = (tick % 4) < 2; // 周期 600ms 的闪烁标志 (亮300ms, 灭300ms)

        // --------------------------------------------------
        // 1. 处理独立状态灯 (LED1 - LED4) 都在 DIG1 的低4位
        // --------------------------------------------------
        for (int i = 1; i <= 4; i++) {
            bool is_on = false;
            if (s_alarm_states[i] == LED_ALARM_ON) is_on = true;
            else if (s_alarm_states[i] == LED_ALARM_BLINK && blink_flag) is_on = true;
            
            if (is_on) {
                dig1 |= (1 << (i - 1)); // LED1=bit0, LED2=bit1, LED3=bit2, LED4=bit3
            }
        }

        // --------------------------------------------------
        // 2. 处理业务模式 (流水灯与制水灯双色交替)
        // --------------------------------------------------
        switch (s_work_mode) {
            case LED_MODE_MAKING_WATER: {
                // 制水双色灯交替 (LED13-1: DIG1.6, LED13-2: DIG1.7)
                if (blink_flag) dig1 |= (1 << 6);
                else            dig1 |= (1 << 7);
                
                // 流水灯动画 (1->2->3->4 循环)
                uint8_t step = tick % 4;
                // 左流水 (LED8->7->6->5 对应 DIG2.3 -> DIG2.2 -> DIG2.1 -> DIG2.0)
                dig2 |= (1 << (3 - step));
                
                // 右流水 (LED11->12->9->10)
                // 对应: R1(DIG2.4), R2(DIG2.5), R3(DIG1.4), R4(DIG1.5)
                if (step == 0) dig2 |= (1 << 4);
                else if (step == 1) dig2 |= (1 << 5);
                else if (step == 2) dig1 |= (1 << 4);
                else if (step == 3) dig1 |= (1 << 5);
                break;
            }
            case LED_MODE_NETWORKING: {
                // 配网/联网：所有流水灯全亮并闪烁
                if (blink_flag) {
                    dig2 |= 0x3F; // DIG2 bit0~5 全亮 (L4,L3,L2,L1, R1,R2)
                    dig1 |= 0x30; // DIG1 bit4~5 全亮 (R3,R4)
                }
                break;
            }
            case LED_MODE_LOW_BALANCE: {
                // 左侧流水灯全亮 (LED5~LED8 -> DIG2 bit0~3)
                dig2 |= 0x0F;
                break;
            }
            case LED_MODE_FILTER_EXPIRE: {
                // 右侧流水灯全亮 (LED9,10 -> DIG1 bit4,5 | LED11,12 -> DIG2 bit4,5)
                dig1 |= 0x30;
                dig2 |= 0x30;
                break;
            }
            case LED_MODE_STANDBY:
            default:
                // 流水灯灭，制水灯灭，保持不变
                break;
        }

        // --------------------------------------------------
        // 3. 将计算好的两组数据刷入硬件
        // --------------------------------------------------
        (void)write_tm1650(TM1650_CMD_DIG1, dig1);
        (void)write_tm1650(TM1650_CMD_DIG2, dig2);
    }
}

// =========================================================
// 外部 API 实现
// =========================================================
static void led_i2c_init_task(void *arg) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };

    esp_err_t err = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_param_config failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    err = i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "i2c_driver_install failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    s_i2c_ready = true;

    (void)write_tm1650(TM1650_CMD_SYS_CTRL, 0x31);
    (void)write_tm1650(TM1650_CMD_DIG1, 0x00);
    (void)write_tm1650(TM1650_CMD_DIG2, 0x00);

    if (!s_anim_task_started) {
        s_anim_task_started = true;
        xTaskCreatePinnedToCore(led_animation_task, "led_anim", 2048, NULL, 5, NULL, 1);
    }

    ESP_LOGI(TAG, "TM1650 LED 驱动与动画任务初始化完成");
    vTaskDelete(NULL);
}

void bsp_led_init(void) {
    static bool s_init_started = false;
    if (s_init_started) return;
    s_init_started = true;

    xTaskCreatePinnedToCore(led_i2c_init_task, "led_i2c_init", 2048, NULL, 5, NULL, 1);
}

void bsp_led_set_alarm(uint8_t led_idx, led_alarm_state_t state) {
    if (led_idx >= 1 && led_idx <= 4) {
        s_alarm_states[led_idx] = state;
    }
}

void bsp_led_set_mode(led_work_mode_t mode) {
    s_work_mode = mode;
}
