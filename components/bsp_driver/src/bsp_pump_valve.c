#include "bsp_pump_valve.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "BSP_CTRL";

static bool s_inlet_state = false;
static bool s_flush_state = false;
static bool s_pump_state = false;

// --- IO 映射 (根据工作表1) ---
#define GPIO_INLET_VALVE   16  // 进水阀
#define GPIO_FLUSH_VALVE   12  // 废水阀 (注意: 启动需低)
#define GPIO_BACKWASH      4   // 反冲洗阀
#define GPIO_PUMP          13  // 水泵 (4.7k上拉，上电默认停机)
#define GPIO_TDS_PWR       15  // TDS总电源 (PMOS控制，低电平导通)

void bsp_pump_valve_init(void) {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL<<GPIO_INLET_VALVE) | (1ULL<<GPIO_FLUSH_VALVE) | 
                        (1ULL<<GPIO_BACKWASH) | (1ULL<<GPIO_PUMP) | (1ULL<<GPIO_TDS_PWR),
        .pull_down_en = 0,
        .pull_up_en = 0
    };
    gpio_config(&io_conf);

    // [重要] 初始化时立刻赋予安全电平
    gpio_set_level(GPIO_INLET_VALVE, 0); 
    gpio_set_level(GPIO_FLUSH_VALVE, 0); 
    gpio_set_level(GPIO_BACKWASH, 0);    
    // 水泵有上拉且上电停机，说明高电平停机，低电平运行 (若实际为高电平运行，请改为0)
    gpio_set_level(GPIO_PUMP, 1);        
    // TDS传感器电源：PMOS低电平导通，尽快拉高断电，用到时再开
    gpio_set_level(GPIO_TDS_PWR, 1);     
    
    ESP_LOGI(TAG, "外设引脚初始化完成，已全部置于安全关断状态");
}

void bsp_set_inlet_valve(bool enable) {
    s_inlet_state = enable; // 记录状态
    ESP_LOGI(TAG, "进水阀 -> %s", enable ? "ON" : "OFF");
    gpio_set_level(GPIO_INLET_VALVE, enable ? 1 : 0);
}

void bsp_set_flush_valve(bool enable) {
    s_flush_state = enable; // 记录状态
    ESP_LOGI(TAG, "废水阀 -> %s", enable ? "ON" : "OFF");
    gpio_set_level(GPIO_FLUSH_VALVE, enable ? 1 : 0);
}

void bsp_set_pump(bool enable) {
    s_pump_state = enable; // 记录状态
    ESP_LOGI(TAG, "增压泵 -> %s", enable ? "ON" : "OFF");
    gpio_set_level(GPIO_PUMP, enable ? 0 : 1); // 注意您之前定义的泵是低电平开启
}

// 控制传感器电源 (false: 关闭, true: 开启)
void bsp_set_sensor_power(bool enable) {
    // 根据 IO 表：PMOS 控制，低电平导通 (enable 为 true 时输出 0)
    gpio_set_level(15, enable ? 0 : 1); 
}
bool bsp_get_inlet_valve_state(void) { return s_inlet_state; }
bool bsp_get_flush_valve_state(void) { return s_flush_state; }
bool bsp_get_pump_state(void)        { return s_pump_state; }