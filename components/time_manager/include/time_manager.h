#pragma once
#include "esp_err.h"
#include <time.h>
#include <sys/time.h>
#include <stdbool.h>

/**
 * @brief 初始化时间管理器 (设置时区、配置 SNTP 服务器)
 * 建议在 app_main 中调用
 */
void time_manager_init(void);

/**
 * @brief 检查时间是否已经同步
 * @return true: 已同步, false: 未同步 (当前可能是 1970 年)
 */
bool time_manager_is_synced(void);

/**
 * @brief 获取当前 Unix 时间戳 (秒)
 */
time_t time_manager_get_timestamp(void);

/**
 * @brief 获取格式化的时间字符串
 * @param buf 输出缓冲区
 * @param max_len 缓冲区大小
 * @return char* 指向 buf 的指针
 * @example "2026-01-26 12:00:00"
 */
char* time_manager_get_time_str(char *buf, size_t max_len);