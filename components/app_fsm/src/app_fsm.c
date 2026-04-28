#include "app_fsm.h"
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "app_events.h"
#include "app_storage.h"
#include "net_manager.h"
#include "mqtt_manager.h"
#include "protocol.h"
#include "bsp_pump_valve.h"
#include "bsp_sensor.h"
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
    WATER_EV_WASH_DONE,      // 冲洗定时器结束
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

static float s_accumulated_liters = 0.0f;

static TimerHandle_t s_wash_timer = NULL;   // 18秒冲洗倒计时

// --- 新增：水泵自适应与保护变量 ---
static float s_pump_limit_over = 3.0f; // 默认给个大值，防止启动误判
static float s_pump_limit_dry  = 0.5f; // 空转阈值通常通用
static bool  s_pump_recognized = false;// 是否已完成硬件识别
static uint8_t s_pump_overcurrent_cnt = 0;
static uint8_t s_pump_dryrun_cnt = 0;
static uint32_t s_pump_run_seconds = 0;

static uint8_t s_pump_spec_cached = 0;

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

// static const char *net_type_name(int type) {
//     switch (type) {
//         case APP_NET_TYPE_WIFI: return "WIFI";
//         case APP_NET_TYPE_PPP: return "PPP";
//         default: return "UNKNOWN";
//     }
// }

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

static void calibrate_pump_zero_if_safe(void) {
    if (bsp_get_pump_state()) return;
    bsp_sensor_pump_current_calibrate_zero();
}

// --- 鉴权拦截逻辑 ---
static bool check_quota_allow_water(void) {
    device_status_t status;
    app_storage_load_status(&status);

    if (status.switch_state == 0) return false; // 关机
    // 1. 检查主套餐 (流量或时间)
    if (status.pay_mode == 0 && status.days <= 0) return false; // 计时到期
    if (status.pay_mode == 1 && status.capacity <= 0) return false; // 流量用尽
    // 2. 检查 9 级滤芯 (双轨记录，单轨鉴权)
    for (int i = 0; i < 9; i++) {
        if (!status.filter_valid[i]) {
            continue; // 未启用/未安装的滤芯直接跳过
        }
        
        if (status.filter_type[i] == 0) {
            // 计时型滤芯，只看天数
            if (status.filter_days[i] <= 0) {
                ESP_LOGW(TAG, "第 %d 级滤芯 (计时型) 已到期，拒绝制水!", i + 1);
                return false;
            }
        } else if (status.filter_type[i] == 1) {
            // 计量型滤芯，只看水量
            if (status.filter_capacity[i] <= 0) {
                ESP_LOGW(TAG, "第 %d 级滤芯 (计量型) 已耗尽，拒绝制水!", i + 1);
                return false;
            }
        }
    }
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
        // if (s_accumulated_liters > 0.001f) { //白嫖漏洞
        //     device_status_t status;
        //     app_storage_load_status(&status);
        //     status.total_flow += (int)(s_accumulated_liters * 1000.0f);
        //     app_storage_save_status(&status);
        //     s_accumulated_liters = 0.0f; // 存完清零
        // }
        //
    }
    if (prev == WATER_STATE_WASHING) {
        xTimerStop(s_wash_timer, 0);
    }

    // 进入新状态时的硬件执行
    switch (next) {
        case WATER_STATE_WASHING:
            calibrate_pump_zero_if_safe();
            s_pump_run_seconds = 0;
            s_pump_overcurrent_cnt = 0;
            s_pump_dryrun_cnt = 0;
            s_time_since_last_wash = 0; // 重置6小时计时器
            s_need_pre_wash = false;
            bsp_set_pump(true);
            bsp_set_inlet_valve(true);
            bsp_set_flush_valve(true);
            xTimerStart(s_wash_timer, 0); // 18秒倒计时
            break;

        case WATER_STATE_MAKING:
            s_making_water_seconds = 0;
            calibrate_pump_zero_if_safe();
            s_pump_run_seconds = 0;
            s_pump_overcurrent_cnt = 0;
            s_pump_dryrun_cnt = 0;
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
            break;
            
        default:
            break;
    }
}

// --- 核心评估逻辑：每次硬件状态改变/冲洗结束时调用 ---
static void evaluate_water_state_ex(bool allow_exit_washing) {
    if (s_water_state == WATER_STATE_FAULT) return; // 故障时只能等待30分钟倒计时

    // 规则：缺水保护拥有最高优先级，强制打断所有动作 (包括冲洗)
    if (s_hw_low_pressure) {
        transition_water_state(WATER_STATE_SHORTAGE, "评估: 原水缺水");
        return;
    }

    if (s_water_state == WATER_STATE_WASHING && !allow_exit_washing) return; // 冲洗中不被打断(除非缺水)

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

static void evaluate_water_state(void) {
    evaluate_water_state_ex(false);
}

// --- 冲洗结束定时器回调 ---
static void wash_timer_cb(TimerHandle_t xTimer) {
    ESP_LOGI(TAG, "18秒冲洗结束，请求重新评估状态...");
    esp_event_post(WATER_INTERNAL_EVENTS, WATER_EV_WASH_DONE, NULL, 0, 0);
}

// ============================================================================
// [模块三] 事件分发处理器
// ============================================================================

// 1. 处理水机内部的流转事件
static void on_water_internal_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_id == WATER_EV_EVALUATE) {
        evaluate_water_state();
    } else if (event_id == WATER_EV_TRIGGER_WASH) {
        s_need_pre_wash = false;
        transition_water_state(WATER_STATE_WASHING, "内部触发强制冲洗");
    } else if (event_id == WATER_EV_WASH_DONE) {
        if (s_water_state != WATER_STATE_WASHING) return;
        s_need_pre_wash = false;
        evaluate_water_state_ex(true);
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

        case APP_EVENT_CMD_EVALUATE:
            ESP_LOGI(TAG, "收到云端参数更新，触发状态机重新评估...");
            esp_event_post(WATER_INTERNAL_EVENTS, WATER_EV_EVALUATE, NULL, 0, 0);
            break;

        default:
            break;
    }
}

// ============================================================================
// [模块四] 制水看门狗：处理超时保护与【流量精准计费结算】
// ============================================================================
static void water_monitor_task(void *pvParameters) {
    // 假设流量计规格：450 个脉冲 = 1 升水 (请根据您的流量计实际规格修改)
    const float PULSES_PER_LITER = 450.0f; 
    
  

    while(1) {
        vTaskDelay(pdMS_TO_TICKS(1000)); // 1秒周期

        // 1. 故障恢复规则：连续制水 6 小时无水满，报故障停机 30 分钟后恢复
        if (s_water_state == WATER_STATE_FAULT) {
            s_fault_timer_seconds++;
            if (s_fault_timer_seconds >= 30 * 60) {
                ESP_LOGI(TAG, "故障停机30分钟结束，尝试恢复制水");
                s_fault_timer_seconds = 0;
                s_need_pre_wash = true;
                esp_event_post(WATER_INTERNAL_EVENTS, WATER_EV_EVALUATE, NULL, 0, 0);
            }
        }

        // 2. 待机冲洗规则：每 6 小时自动冲洗一次
        if (s_water_state != WATER_STATE_FAULT && s_water_state != WATER_STATE_WASHING) {
            s_time_since_last_wash++;
            if (s_time_since_last_wash >= 6 * 3600) {
                ESP_LOGI(TAG, "已待机/运行满6小时，触发自动冲洗");
                esp_event_post(WATER_INTERNAL_EVENTS, WATER_EV_TRIGGER_WASH, NULL, 0, 0);
            }
        }

        
        if (s_water_state == WATER_STATE_MAKING || s_water_state == WATER_STATE_WASHING) {
            
            // ==========================================
            // [A] 水泵电流读取
            // ==========================================
            float pump_current = bsp_sensor_get_pump_current();
            s_pump_run_seconds++;
            
            // ==========================================
            // [B] 硬件自适应识别 (开机后的首次运行)
            // ==========================================
            // 避开前4秒的启动浪涌，在第5秒进行平稳期采样识别
            if (s_pump_run_seconds == 5) {
                uint8_t spec_new = 0;
                if (pump_current < 1.8f) {
                    s_pump_limit_over = 2.0f; // 100G 泵最大 2A
                    spec_new = 1;
                    ESP_LOGW(TAG, "🤖 硬件自适应: 识别为【100G水泵】(当前 %.2fA), 锁定过流阈值: %.2fA", pump_current, s_pump_limit_over);
                } else {
                    s_pump_limit_over = 3.0f; // 400G 泵最大 3A
                    spec_new = 2;
                    ESP_LOGW(TAG, "🤖 硬件自适应: 识别为【400G水泵】(当前 %.2fA), 锁定过流阈值: %.2fA", pump_current, s_pump_limit_over);
                }
                s_pump_recognized = true;

                if (s_pump_spec_cached == 0) {
                    uint8_t prev = 0;
                    if (app_storage_get_pump_spec(&prev) == ESP_OK) {
                        s_pump_spec_cached = prev;
                    }
                }
                if (s_pump_spec_cached != 0 && spec_new != 0 && s_pump_spec_cached != spec_new) {
                    alert_report_t alert = {
                        .timestamp = 0,
                        .alert_code = 5,
                        .status = "triggered",
                    };
                    mqtt_manager_publish_alert(&alert);
                }
                if (spec_new != 0) {
                    app_storage_set_pump_spec(spec_new);
                    s_pump_spec_cached = spec_new;
                }
            }

            // ==========================================
            // [C] 异常状态监测与保护
            // ==========================================
            if (s_pump_run_seconds >= 5) {
                
                // 1) 过流/堵转保护检测
                if (pump_current > s_pump_limit_over) {
                    s_pump_overcurrent_cnt++;
                    if (s_pump_overcurrent_cnt >= 2) { // 连续 2 秒超标
                        ESP_LOGE(TAG, "🚨 致命异常：水泵过流/堵转！当前电流: %.2f A，阈值: %.2f A", pump_current, s_pump_limit_over);
                        alert_report_t alert = {
                            .timestamp = 0,
                            .alert_code = 5,
                            .status = "triggered",
                        };
                        mqtt_manager_publish_alert(&alert);
                        transition_water_state(WATER_STATE_FAULT, "水泵过流保护");
                    }
                } else {
                    s_pump_overcurrent_cnt = 0;
                }

                // 2) 空转/缺水保护检测
                if (pump_current < s_pump_limit_dry) {
                    s_pump_dryrun_cnt++;
                    if (s_pump_dryrun_cnt >= 3) { // 连续 3 秒过低
                        ESP_LOGE(TAG, "🚨 致命异常：水泵空转/无负载！当前电流: %.2f A", pump_current);
                        alert_report_t alert = {
                            .timestamp = 0,
                            .alert_code = 5,
                            .status = "triggered",
                        };
                        mqtt_manager_publish_alert(&alert);
                        transition_water_state(WATER_STATE_FAULT, "水泵空转保护");
                    }
                } else {
                    s_pump_dryrun_cnt = 0;
                }
            }
            if (s_water_state == WATER_STATE_MAKING) {
                s_making_water_seconds++;
                s_total_making_time++;
            }

            // --- 【新增】读取流量计并精准扣费 ---
            uint32_t pulses = bsp_sensor_get_flow_pulses();
            bsp_sensor_clear_flow_pulses(); // 读取后立刻清零，等待下一秒累加

            if (pulses > 0) {
                // 计算这 1 秒内的真实制水量 (小数)
                float current_liters = (float)pulses / PULSES_PER_LITER;
                s_accumulated_liters += current_liters;

                // 当累积水量达到 1 升时，才进行 NVS 扣减 (保护 Flash 寿命，防精度丢失)
                if (s_accumulated_liters >= 1.0f) {
                    int deduct_liters = (int)s_accumulated_liters; // 提取整数部分
                    s_accumulated_liters -= deduct_liters;         // 留下零头下次算

                    device_status_t status;
                    app_storage_load_status(&status);

                    status.total_flow += (deduct_liters * 1000); // NVS里总水量单位是毫升
                    bool need_intercept = false; // 是否需要强制停机拦截标志

                    // 核心拦截：如果是计量套餐，执行扣费
                    if (status.pay_mode == 1) { 
                        status.capacity -= deduct_liters;
                        ESP_LOGI(TAG, "扣除套餐水量 %d L, 剩余 %d L", deduct_liters, status.capacity);

                        if (status.capacity <= 0) {
                            ESP_LOGE(TAG, "🚨 套餐水量已彻底用尽，强制停机拦截！");
                            status.capacity = 0;
                            // 抛出评估事件，状态机会因为鉴权不通过自动关闭水泵和进水阀
                            need_intercept = true;
                        }
                    }
                    // 2. 扣除 9 级滤芯的容量 (所有有效滤芯都要扣)
                    for (int i = 0; i < 9; i++) {
                        if (status.filter_valid[i]) {
                            // 无论类型，水量都要双轨递减 (允许减到负数，不兜底归0)
                            status.filter_capacity[i] -= deduct_liters; 
                            
                            // 单轨鉴权：仅当它是计量滤芯，且恰好 <= 0 时，触发拦截
                            if (status.filter_type[i] == 1 && status.filter_capacity[i] <= 0) {
                                ESP_LOGE(TAG, "🚨 第 %d 级计量滤芯水量已用尽！", i + 1);
                                need_intercept = true;
                            }
                        }
                    }
                    // 3. 执行强制拦截
                    if (need_intercept) {
                        ESP_LOGE(TAG, "🚨 套餐水量或滤芯已耗尽，强制停机拦截！");
                        // 抛出评估事件，状态机会因为 check_quota_allow_water 不通过而关闭水泵
                        esp_event_post(WATER_INTERNAL_EVENTS, WATER_EV_EVALUATE, NULL, 0, 0);
                    }
                    app_storage_save_status(&status); // 存入 Flash
                }
            }
            
            // 4. 制水超时保护：连续制水超过 6 小时
            if (s_making_water_seconds >= 6 * 3600) {
                ESP_LOGE(TAG, "严重：连续制水超过6小时，触发保护停机！");
                transition_water_state(WATER_STATE_FAULT, "制水超时");
            }

            // 5. 制水期间维护：累计制水满 2 小时强制冲洗
            if (s_total_making_time >= 2 * 3600) {
                ESP_LOGI(TAG, "累计制水达2小时，触发维护冲洗！");
                s_total_making_time = 0; 
                esp_event_post(WATER_INTERNAL_EVENTS, WATER_EV_TRIGGER_WASH, NULL, 0, 0);
            }
        } else {
            // 如果不在制水状态，清空残留的水量零头和脉冲，防止误算
            bsp_sensor_clear_flow_pulses();
            // s_accumulated_liters = 0.0f; //白嫖漏洞
        }

    }
}

// --- 辅助函数：获取制水业务状态的字符串名称 ---
static const char *water_state_name(water_state_t state) {
    switch (state) {
        case WATER_STATE_INIT: return "INIT (初始化)";
        case WATER_STATE_WASHING: return "WASHING (冲洗中)";
        case WATER_STATE_MAKING: return "MAKING_WATER (制水中)";
        case WATER_STATE_FULL: return "WATER_FULL (水满待机)";
        case WATER_STATE_SHORTAGE: return "SHORTAGE (缺水报警)";
        case WATER_STATE_FAULT: return "FAULT (超时故障)";
        default: return "UNKNOWN";
    }
}

// ============================================================================
// [模块五] 系统实时诊断面板任务 (Dashboard)
// ============================================================================
static void system_dashboard_task(void *pvParameters) {
    (void)pvParameters;
    while (1) {
        // 每 5 秒刷新一次面板
        vTaskDelay(pdMS_TO_TICKS(5000));

        // 获取最新 NVS 存储数据
        device_status_t status;
        app_storage_load_status(&status);

        net_config_t net_cfg = {0};
        app_storage_load_net_config(&net_cfg);

        char device_id[64] = {0};
        char uid[64] = {0};
        protocol_get_device_id(device_id, sizeof(device_id));
        protocol_get_uid(uid, sizeof(uid));

        uint32_t heap_free = esp_get_free_heap_size();
        uint32_t min_heap_free = esp_get_minimum_free_heap_size();
        uint64_t uptime_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);

        float pump_current = bsp_sensor_get_pump_current();
        bool quota_allow = check_quota_allow_water();

        const char *pump_spec_str = "UNKNOWN";
        if (s_pump_spec_cached == 1) pump_spec_str = "100G";
        else if (s_pump_spec_cached == 2) pump_spec_str = "400G";

        uint32_t fault_elapsed_s = (s_water_state == WATER_STATE_FAULT) ? s_fault_timer_seconds : 0;
        uint32_t fault_remain_s = 0;
        if (s_water_state == WATER_STATE_FAULT && s_fault_timer_seconds < 30 * 60) {
            fault_remain_s = (30 * 60) - s_fault_timer_seconds;
        }

        printf("\n");
        printf("========================================================\n");
        printf("              [ 智能净水器系统实时诊断面板 ]            \n");
        printf("========================================================\n");
        
        // 1. 核心状态机
        printf(" [1] 核心状态 (看门狗监控)\n");
        printf("  ├─ DeviceID/UID : %s / %s\n", device_id, uid);
        printf("  ├─ Uptime/Heap  : %llu ms | free %lu B (min %lu B)\n",
               (unsigned long long)uptime_ms, (unsigned long)heap_free, (unsigned long)min_heap_free);
        printf("  ├─ Net/MQTT FSM  : %s (net_ready=%d)\n", state_name(s_state), s_net_ready ? 1 : 0);
        printf("  ├─ Water FSM     : %s (quota=%s, need_pre_wash=%d)\n",
               water_state_name(s_water_state), quota_allow ? "OK" : "BLOCK", s_need_pre_wash ? 1 : 0);
        printf("  ├─ 单次连续制水 : %lu 秒 (限 6h 超时保护)\n", (unsigned long)s_making_water_seconds);
        printf("  ├─ 累计制水时长 : %lu 秒 (满 2h 触发维护冲洗)\n", (unsigned long)s_total_making_time);
        printf("  ├─ 距上次冲洗   : %lu 秒 (满 6h 触发定期冲洗)\n", (unsigned long)s_time_since_last_wash);
        printf("  └─ 故障恢复(30min): 已停机 %lu s, 剩余 %lu s\n",
               (unsigned long)fault_elapsed_s, (unsigned long)fault_remain_s);
        printf("\n");

        // 2. 硬件传感器实时读数
        printf(" [2] 传感器实时数据\n");
        printf("  ├─ 原水水压 (低压): %s\n", s_hw_low_pressure ? "【缺水/断开】" : "正常/闭合");
        printf("  ├─ 储水压力 (高压): %s\n", s_hw_high_pressure ? "【水满/闭合】" : "未满/断开");
        printf("  ├─ 实时水温 (NTC) : %.1f °C\n", bsp_sensor_get_temperature());
        printf("  ├─ 水质 TDS (ppm) : 进水 %d | 纯水 %d | 备用 %d\n", 
                bsp_sensor_get_tds_in(), bsp_sensor_get_tds_out(), bsp_sensor_get_tds_backup());
        printf("  ├─ 流量计累计脉冲 : %lu (读后清零前)\n", (unsigned long)bsp_sensor_get_flow_pulses());
        printf("  └─ 水泵电流        : %.2f A (spec=%s, recognized=%d, run=%lu s, over=%u/%0.2fA, dry=%u/%0.2fA)\n",
               (double)pump_current,
               pump_spec_str,
               s_pump_recognized ? 1 : 0,
               (unsigned long)s_pump_run_seconds,
               (unsigned)s_pump_overcurrent_cnt,
               (double)s_pump_limit_over,
               (unsigned)s_pump_dryrun_cnt,
               (double)s_pump_limit_dry);
        printf("\n");

        // 3. 继电器与外设执行器
        printf(" [3] 外设执行器状态\n");
        printf("  ├─ 进水电磁阀     : %s\n", bsp_get_inlet_valve_state() ? "■ 开启" : "□ 关闭");
        printf("  ├─ 废水电磁阀     : %s\n", bsp_get_flush_valve_state() ? "■ 开启" : "□ 关闭");
        printf("  └─ 增压水泵       : %s\n", bsp_get_pump_state() ? "■ 运行" : "□ 停止");
        printf("\n");

        // 4. 网络配置 (NVS)
        printf(" [4] 网络与 MQTT 配置 (NVS)\n");
        printf("  ├─ Net mode      : %s\n", net_cfg.mode == 0 ? "WiFi" : "4G");
        if (net_cfg.mode == 0) {
            printf("  ├─ WiFi SSID     : %s\n", net_cfg.ssid);
        }
        printf("  ├─ MQTT URL      : %s\n", net_cfg.full_url);
        if (net_cfg.username[0] != '\0') {
            char user_mask[80] = {0};
            size_t ulen = strlen(net_cfg.username);
            if (ulen <= 2) {
                snprintf(user_mask, sizeof(user_mask), "**");
            } else if (ulen <= 6) {
                snprintf(user_mask, sizeof(user_mask), "%c**%c", net_cfg.username[0], net_cfg.username[ulen - 1]);
            } else {
                snprintf(user_mask, sizeof(user_mask), "%c%c**%c%c",
                         net_cfg.username[0], net_cfg.username[1],
                         net_cfg.username[ulen - 2], net_cfg.username[ulen - 1]);
            }
            printf("  └─ MQTT Username : %s\n", user_mask);
        } else {
            printf("  └─ MQTT Username : (empty)\n");
        }
        printf("\n");

        // 5. 用户套餐与计费参数
        printf(" [5] 套餐/滤芯/计费 (NVS)\n");
        printf("  ├─ 设备软开关机   : %s\n", status.switch_state ? "开机" : "关机拦截");
        printf("  ├─ 当前计费模式   : %s\n", status.pay_mode == 0 ? "计时模式" : "计量模式");

        float display_capacity = (float)status.capacity;
        if (status.pay_mode == 1) {
            display_capacity -= s_accumulated_liters; // 实时扣减零头显示
        }
        uint32_t display_total_flow = (uint32_t)status.total_flow + (uint32_t)(s_accumulated_liters * 1000.0f);
        
        printf("  ├─ 剩余天数/水量  : %d 天 / %.2f L\n", status.days, display_capacity);
        printf("  ├─ 历史总制水量   : %lu mL (delta %.2f L)\n",
               (unsigned long)display_total_flow, (double)s_accumulated_liters);
        
        printf("  └─ 滤芯剩余       : ");
        bool any_filter = false;
        for (int i = 0; i < 9; i++) {
            if (!status.filter_valid[i]) continue;
            any_filter = true;
            printf("L%d(%s) D:%d C:%d  ",
                   i + 1,
                   status.filter_type[i] == 1 ? "V" : "T",
                   status.filter_days[i],
                   status.filter_capacity[i]);
        }
        if (!any_filter) {
            printf("none");
        }
        printf("\n");
        printf("========================================================\n\n");
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
    // 【新增】启动诊断面板任务 (堆栈稍微给大一点点保证 printf 不溢出)
    // xTaskCreate(system_dashboard_task, "sys_dash", 4096, NULL, 4, NULL);


    // 状态机初始状态
    s_state = FSM_STATE_WAIT_NET;
    s_water_state = WATER_STATE_INIT;
    transition_to(FSM_STATE_WAIT_NET, "fsm init");

    uint8_t pump_spec = 0;
    if (app_storage_get_pump_spec(&pump_spec) == ESP_OK) {
        s_pump_spec_cached = pump_spec;
        if (pump_spec == 1) {
            s_pump_limit_over = 2.0f;
        } else if (pump_spec == 2) {
            s_pump_limit_over = 3.0f;
        }
    }

    // 规则：开机流程 -> 执行一次自动冲洗
    esp_event_post(WATER_INTERNAL_EVENTS, WATER_EV_TRIGGER_WASH, NULL, 0, pdMS_TO_TICKS(100));
}
