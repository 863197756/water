#include "app_fsm.h"
#include <stdbool.h>
#include <string.h>
#include "esp_log.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "app_events.h"
#include "app_storage.h"
#include "net_manager.h"
#include "mqtt_manager.h"
#include "bsp_pump_valve.h"



// ============================================================================
// 状态枚举与内部事件定义
// ============================================================================
typedef enum {
    FSM_STATE_IDLE = 0,
    FSM_STATE_WAIT_NET,
    FSM_STATE_MQTT_CONNECTING,
    FSM_STATE_MQTT_READY,
    FSM_STATE_RUNNING,
} fsm_state_t;

typedef enum {
    WATER_STATE_INIT = 0,
    WATER_STATE_WASHING,     // 冲洗状态
    WATER_STATE_MAKING,      // 制水状态
    WATER_STATE_FULL,        // 水满状态
    WATER_STATE_SHORTAGE,    // 缺水状态
    WATER_STATE_FAULT        // 故障状态
} water_state_t;

// 定义专属于水机流转的内部事件基，用于解耦硬件中断与状态机评估
ESP_EVENT_DEFINE_BASE(WATER_INTERNAL_EVENTS);
enum {
    WATER_EV_EVALUATE = 0,   // 重新评估水机状态
    WATER_EV_TRIGGER_WASH,   // 强制触发一次冲洗
};

// ============================================================================
// 全局状态与计时器变量
// ============================================================================
static const char *TAG = "APP_FSM";

static fsm_state_t s_state = FSM_STATE_IDLE;             // 网络/MQTT状态
static water_state_t s_water_state = WATER_STATE_INIT;   // 制水业务状态
static bool s_net_ready = false;

// 硬件传感器真实状态缓存
static bool s_hw_low_pressure = false;  // true = 缺水
static bool s_hw_high_pressure = false; // true = 水满

// 业务流转标志与计时器
static bool s_need_pre_wash = false;        // 是否需要制水前置冲洗
static uint32_t s_making_water_seconds = 0; // 单次连续制水时长(秒)
static uint32_t s_total_making_time = 0;    // 累计制水时长(秒) - 用于2小时冲洗
static uint32_t s_time_since_last_wash = 0; // 距离上次冲洗时长(秒) - 用于6小时冲洗
static uint32_t s_fault_timer_seconds = 0;  // 故障恢复倒计时(秒) - 用于30分钟恢复

static TimerHandle_t s_wash_timer = NULL;   // 18秒冲洗倒计时

// ============================================================================
// [模块一] 原有的网络与 MQTT 管理逻辑 (原封不动保留)
// ============================================================================
static const char *state_name(fsm_state_t state) {
    switch (state) {
        case FSM_STATE_IDLE: return "IDLE";
        case FSM_STATE_WAIT_NET: return "WAIT_NET";
        case FSM_STATE_MQTT_CONNECTING: return "MQTT_CONNECTING";
        case FSM_STATE_MQTT_READY: return "MQTT_READY";
        case FSM_STATE_RUNNING: return "RUNNING";
        default: return "UNKNOWN";
    }
}

static const char *net_type_name(int type) {
    switch (type) {
        case APP_NET_TYPE_WIFI: return "WIFI";
        case APP_NET_TYPE_PPP: return "PPP";
        default: return "UNKNOWN";
    }
}

static void transition_to(fsm_state_t next, const char *reason) {
    if (s_state == next) return;
    ESP_LOGI(TAG, "Net State: %s -> %s (%s)", state_name(s_state), state_name(next), reason);
    s_state = next;
}

static bool has_valid_mqtt_config(void) {
    net_config_t cfg = {0};
    if (app_storage_load_net_config(&cfg) != ESP_OK) return false;
    return strlen(cfg.full_url) > 5;
}

static void try_start_mqtt(const char *reason) {
    if (!s_net_ready) {
        ESP_LOGI(TAG, "Skip MQTT start: network not ready (%s)", reason);
        return;
    }
    if (!has_valid_mqtt_config()) {
        ESP_LOGW(TAG, "Skip MQTT start: invalid MQTT config (%s)", reason);
        return;
    }
    ESP_LOGI(TAG, "Starting MQTT (%s)", reason);
    mqtt_manager_start();
    transition_to(FSM_STATE_MQTT_CONNECTING, reason);
}

// ============================================================================
// [模块二] 制水状态机核心逻辑 (补全了所有边界规则)
// ============================================================================

// --- 鉴权拦截逻辑 ---
static bool check_quota_allow_water(void) {
    device_status_t status;
    app_storage_load_status(&status);

    if (status.switch_state == 0) return false; // 关机
    if (status.pay_mode == 0 && status.days <= 0) return false; // 计时到期
    if (status.pay_mode == 1 && status.capacity <= 0) return false; // 流量用尽
    return true;
}

// --- 硬件动作转移引擎 ---
static void transition_water_state(water_state_t next, const char *reason) {
    if (s_water_state == next) return;
    ESP_LOGI(TAG, "水机状态转移: [%d] -> [%d] (原因: %s)", s_water_state, next, reason);
    
    water_state_t prev = s_water_state;
    s_water_state = next;

    // 退出上一个状态时的清理工作
    if (prev == WATER_STATE_MAKING) {
        // 规则：制水至水满后（需满足本次累积制水60秒以上）触发冲洗
        if (next == WATER_STATE_FULL && s_making_water_seconds > 60) {
            ESP_LOGI(TAG, "制水满且大于60秒，触发后置冲洗");
            esp_event_post(WATER_INTERNAL_EVENTS, WATER_EV_TRIGGER_WASH, NULL, 0, 0);
        }
        s_making_water_seconds = 0;
        // TODO: 可在此处将制水时长换算为流量并保存 NVS
    }
    if (prev == WATER_STATE_WASHING) {
        xTimerStop(s_wash_timer, 0);
    }

    // 进入新状态时的硬件执行
    switch (next) {
        case WATER_STATE_WASHING:
            s_time_since_last_wash = 0; // 重置6小时计时器
            bsp_set_pump(true);
            bsp_set_inlet_valve(true);
            bsp_set_flush_valve(true);
            xTimerStart(s_wash_timer, 0); // 18秒倒计时
            break;

        case WATER_STATE_MAKING:
            s_making_water_seconds = 0;
            bsp_set_inlet_valve(true);
            bsp_set_flush_valve(false); // 关废水，建立背压
            bsp_set_pump(true);
            break;

        case WATER_STATE_FULL:
        case WATER_STATE_SHORTAGE:
            bsp_set_pump(false);
            bsp_set_inlet_valve(false);
            bsp_set_flush_valve(false);
            break;

        case WATER_STATE_FAULT:
            s_fault_timer_seconds = 0;  // 启动 30 分钟恢复倒计时
            bsp_set_pump(false);
            bsp_set_inlet_valve(false);
            bsp_set_flush_valve(false);
            // TODO: 发送 alert 警报 MQTT
            break;
            
        default:
            break;
    }
}

// --- 核心评估逻辑：每次硬件状态改变/冲洗结束时调用 ---
static void evaluate_water_state(void) {
    if (s_water_state == WATER_STATE_FAULT) return; // 故障时只能等待30分钟倒计时

    // 规则：缺水保护拥有最高优先级，强制打断所有动作 (包括冲洗)
    if (s_hw_low_pressure) {
        transition_water_state(WATER_STATE_SHORTAGE, "评估: 原水缺水");
        return;
    }

    if (s_water_state == WATER_STATE_WASHING) return; // 冲洗中不被打断(除非缺水)

    if (s_hw_high_pressure) {
        transition_water_state(WATER_STATE_FULL, "评估: 储水桶满");
        return;
    }

    // 既不缺水，也不水满 -> 需要制水
    if (check_quota_allow_water()) {
        if (s_need_pre_wash) {
            // 规则：开始制水时冲洗 / 缺水转制水状态冲洗
            s_need_pre_wash = false;
            transition_water_state(WATER_STATE_WASHING, "制水前置冲洗");
        } else {
            transition_water_state(WATER_STATE_MAKING, "开始制水");
        }
    } else {
        transition_water_state(WATER_STATE_FULL, "评估: 鉴权拦截待机");
    }
}

// --- 冲洗结束定时器回调 ---
static void wash_timer_cb(TimerHandle_t xTimer) {
    ESP_LOGI(TAG, "18秒冲洗结束，请求重新评估状态...");
    esp_event_post(WATER_INTERNAL_EVENTS, WATER_EV_EVALUATE, NULL, 0, 0);
}

// ============================================================================
// [模块三] 事件分发处理器
// ============================================================================

// 1. 处理水机内部的流转事件
static void on_water_internal_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_id == WATER_EV_EVALUATE) {
        evaluate_water_state();
    } else if (event_id == WATER_EV_TRIGGER_WASH) {
        transition_water_state(WATER_STATE_WASHING, "内部触发强制冲洗");
    }
}

// 2. 处理外部网络与硬件传感器事件
static void on_app_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base != APP_EVENTS) return;

    switch (event_id) {
        // --- 原有的网络逻辑 (完全保留) ---
        case APP_EVENT_NET_MODE_REQUEST: {
            if (!event_data) break;
            const app_event_net_mode_t *req = (const app_event_net_mode_t *)event_data;
            s_net_ready = false;
            mqtt_manager_stop();
            net_manager_set_mode(req->mode);
            transition_to(FSM_STATE_WAIT_NET, "net mode request");
            break;
        }
        case APP_EVENT_MQTT_CONFIG_UPDATED:
            try_start_mqtt("mqtt config updated");
            break;
        case APP_EVENT_NET_READY:
            if (s_net_ready && (s_state == FSM_STATE_MQTT_CONNECTING || s_state == FSM_STATE_MQTT_READY || s_state == FSM_STATE_RUNNING)) break;
            s_net_ready = true;
            transition_to(FSM_STATE_WAIT_NET, "network ready");
            try_start_mqtt("network ready");
            break;
        case APP_EVENT_NET_LOST:
            s_net_ready = false;
            mqtt_manager_stop();
            transition_to(FSM_STATE_WAIT_NET, "network lost");
            break;
        case APP_EVENT_MQTT_CONNECTED:
            transition_to(FSM_STATE_MQTT_READY, "mqtt connected");
            break;
        case APP_EVENT_MQTT_PLAN_RECEIVED:
            transition_to(FSM_STATE_RUNNING, "plan received");
            break;
        case APP_EVENT_MQTT_DISCONNECTED:
            if (s_net_ready) transition_to(FSM_STATE_MQTT_CONNECTING, "mqtt disconnected");
            else transition_to(FSM_STATE_WAIT_NET, "mqtt disconnected no network");
            break;
        
        // --- 硬件与指令流转逻辑 ---
        case APP_EVENT_HW_LOW_PRESSURE_ALARM:
            s_hw_low_pressure = true;
            esp_event_post(WATER_INTERNAL_EVENTS, WATER_EV_EVALUATE, NULL, 0, 0);
            break;

        case APP_EVENT_HW_LOW_PRESSURE_RECOVER:
            s_hw_low_pressure = false;
            s_need_pre_wash = true; // 规则：缺水转制水状态冲洗
            esp_event_post(WATER_INTERNAL_EVENTS, WATER_EV_EVALUATE, NULL, 0, 0);
            break;

        case APP_EVENT_HW_HIGH_PRESSURE_ALARM:
            s_hw_high_pressure = true;
            esp_event_post(WATER_INTERNAL_EVENTS, WATER_EV_EVALUATE, NULL, 0, 0);
            break;

        case APP_EVENT_HW_HIGH_PRESSURE_RECOVER:
            s_hw_high_pressure = false;
            s_need_pre_wash = true; // 规则：开始制水时冲洗
            esp_event_post(WATER_INTERNAL_EVENTS, WATER_EV_EVALUATE, NULL, 0, 0);
            break;

        case APP_EVENT_CMD_START_WASH:
            // 规则：收到云端或开机冲洗指令
            esp_event_post(WATER_INTERNAL_EVENTS, WATER_EV_TRIGGER_WASH, NULL, 0, 0);
            break;

        default:
            break;
    }
}

// ============================================================================
// [模块四] 制水看门狗：处理所有的超时规则
// ============================================================================
static void water_monitor_task(void *pvParameters) {
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(1000)); // 1秒周期

        // 1. 规则：连续制水 6 小时无水满，报故障停机 30 分钟后恢复
        if (s_water_state == WATER_STATE_FAULT) {
            s_fault_timer_seconds++;
            if (s_fault_timer_seconds >= 30 * 60) {
                ESP_LOGI(TAG, "故障停机30分钟结束，尝试恢复制水");
                s_fault_timer_seconds = 0;
                s_need_pre_wash = true;
                esp_event_post(WATER_INTERNAL_EVENTS, WATER_EV_EVALUATE, NULL, 0, 0);
            }
        }

        // 2. 规则：每 6 小时自动冲洗一次
        if (s_water_state != WATER_STATE_FAULT && s_water_state != WATER_STATE_WASHING) {
            s_time_since_last_wash++;
            if (s_time_since_last_wash >= 6 * 3600) {
                ESP_LOGI(TAG, "已待机/运行满6小时，触发自动冲洗");
                esp_event_post(WATER_INTERNAL_EVENTS, WATER_EV_TRIGGER_WASH, NULL, 0, 0);
            }
        }

        // 3. 运行期的超时保护与累计统计
        if (s_water_state == WATER_STATE_MAKING) {
            s_making_water_seconds++;
            s_total_making_time++; 
            
            // 故障触发判断
            if (s_making_water_seconds >= 6 * 3600) {
                ESP_LOGE(TAG, "严重：连续制水超过6小时，触发保护停机！");
                transition_water_state(WATER_STATE_FAULT, "制水超时");
            }

            // 规则：累计制水满 2 小时，强制插入一次冲洗
            if (s_total_making_time >= 2 * 3600) {
                ESP_LOGI(TAG, "累计制水达2小时，触发维护冲洗！");
                s_total_making_time = 0; 
                esp_event_post(WATER_INTERNAL_EVENTS, WATER_EV_TRIGGER_WASH, NULL, 0, 0);
            }
        }
    }
}

// ============================================================================
// 初始化入口
// ============================================================================
void app_fsm_init(void) {
    // 注册网络与硬件事件
    ESP_ERROR_CHECK(esp_event_handler_register(APP_EVENTS, ESP_EVENT_ANY_ID, &on_app_event, NULL));
    // 注册内部水机流转事件
    ESP_ERROR_CHECK(esp_event_handler_register(WATER_INTERNAL_EVENTS, ESP_EVENT_ANY_ID, &on_water_internal_event, NULL));
    
    // 初始化定时器与任务
    s_wash_timer = xTimerCreate("wash_tmr", pdMS_TO_TICKS(18000), pdFALSE, NULL, wash_timer_cb);
    xTaskCreate(water_monitor_task, "water_dog", 3072, NULL, 5, NULL);
    
    // 状态机初始状态
    s_state = FSM_STATE_WAIT_NET;
    s_water_state = WATER_STATE_INIT;
    transition_to(FSM_STATE_WAIT_NET, "fsm init");

    // 规则：开机流程 -> 执行一次自动冲洗
    esp_event_post(WATER_INTERNAL_EVENTS, WATER_EV_TRIGGER_WASH, NULL, 0, pdMS_TO_TICKS(100));
}