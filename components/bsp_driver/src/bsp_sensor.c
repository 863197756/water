#include "bsp_sensor.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_event.h"
#include "app_events.h" // 确保能拿到 APP_EVENT_HW_... 的宏

static const char *TAG = "BSP_SENSOR";

#define GPIO_SW_LOW_PRESS  35 // 低压开关 (接10k上拉)
#define GPIO_SW_HIGH_PRESS 14 // 高压开关 (接10k上拉)

// 中断服务函数 (ISR)
static void IRAM_ATTR pressure_switch_isr_handler(void* arg) {
    uint32_t gpio_num = (uint32_t) arg;
    int level = gpio_get_level(gpio_num);
    
    // ISR中发送事件，必须使用 esp_event_isr_post
    BaseType_t high_task_wakeup = pdFALSE;
    
    if (gpio_num == GPIO_SW_LOW_PRESS) {
        // 假设：低电平为缺水 (开关断开)，高电平为正常 (开关闭合)
        if (level == 0) {
            esp_event_isr_post(APP_EVENTS, APP_EVENT_HW_LOW_PRESSURE_ALARM, NULL, 0, &high_task_wakeup);
        } else {
            esp_event_isr_post(APP_EVENTS, APP_EVENT_HW_LOW_PRESSURE_RECOVER, NULL, 0, &high_task_wakeup);
        }
    } else if (gpio_num == GPIO_SW_HIGH_PRESS) {
        // 假设：高电平为水满 (开关断开)，低电平为需要制水 (开关闭合)
        if (level == 1) {
            esp_event_isr_post(APP_EVENTS, APP_EVENT_HW_HIGH_PRESSURE_ALARM, NULL, 0, &high_task_wakeup);
        } else {
            esp_event_isr_post(APP_EVENTS, APP_EVENT_HW_HIGH_PRESSURE_RECOVER, NULL, 0, &high_task_wakeup);
        }
    }
    
    if (high_task_wakeup == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

void bsp_sensor_init(void) {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_ANYEDGE, // 双边沿触发
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL<<GPIO_SW_LOW_PRESS) | (1ULL<<GPIO_SW_HIGH_PRESS),
        .pull_up_en = 1, // 内部上拉辅助
        .pull_down_en = 0
    };
    gpio_config(&io_conf);

    // 安装全局 GPIO 中断服务并挂载回调
    gpio_install_isr_service(0);
    gpio_isr_handler_add(GPIO_SW_LOW_PRESS, pressure_switch_isr_handler, (void*) GPIO_SW_LOW_PRESS);
    gpio_isr_handler_add(GPIO_SW_HIGH_PRESS, pressure_switch_isr_handler, (void*) GPIO_SW_HIGH_PRESS);

    ESP_LOGI(TAG, "压力开关中断已注册 (IO14, IO35)");
}