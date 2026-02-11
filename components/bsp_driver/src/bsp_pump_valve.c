#include "bsp_pump_valve.h"
#include "driver/gpio.h"

// 根据实际引脚修改
#define PIN_VALVE_IN    21
#define PIN_VALVE_FLUSH 22
#define PIN_PUMP        23

void bsp_pump_valve_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL<<PIN_VALVE_IN) | (1ULL<<PIN_VALVE_FLUSH) | (1ULL<<PIN_PUMP),
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = 0,
        .pull_up_en = 0,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    
    // 默认关闭
    bsp_set_valve(false);
    bsp_set_pump(false);
    bsp_set_flush_valve(false);
}

void bsp_set_valve(bool open) {
    gpio_set_level(PIN_VALVE_IN, open ? 1 : 0);
}

void bsp_set_pump(bool open) {
    gpio_set_level(PIN_PUMP, open ? 1 : 0);
}

void bsp_set_flush_valve(bool open) {
    gpio_set_level(PIN_VALVE_FLUSH, open ? 1 : 0);
}