#include "time_manager.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include <string.h>


static const char *TAG = "TIME_MGR";

// 时间同步回调函数 (当 SNTP 同步成功时被调用)
static void time_sync_notification_cb(struct timeval *tv) {
    ESP_LOGI(TAG, "Notification of a time synchronization event");
    
    // 打印一下同步后的时间
    char strftime_buf[64];
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "Current Time: %s", strftime_buf);
}

void time_manager_init(void) {
    ESP_LOGI(TAG, "Initializing SNTP");
    
    // 1. 设置 SNTP 工作模式为轮询
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    
    // 2. 设置 NTP 服务器
    // 优先级 0: 阿里云 NTP (国内最快)
    sntp_setservername(0, "ntp.aliyun.com"); 
    // 优先级 1: 腾讯云 NTP
    sntp_setservername(1, "ntp.tencent.com");
    // 优先级 2: 全球 NTP 池
    sntp_setservername(2, "pool.ntp.org");

    // 3. 设置同步回调
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    
    // 4. 初始化 SNTP 模块
    sntp_init();

    // 5. 设置时区为中国标准时间 (UTC+8)
    // CST-8: CST 是时区名，-8 表示比 UTC 快 8 小时 (POSIX 格式比较反直觉，东八区写负数)
    setenv("TZ", "CST-8", 1);
    tzset();
    
    ESP_LOGI(TAG, "Timezone set to CST-8");
}

bool time_manager_is_synced(void) {
    // 检查同步状态
    return (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED);
}

time_t time_manager_get_timestamp(void) {
    time_t now;
    time(&now);
    return now;
}

char* time_manager_get_time_str(char *buf, size_t max_len) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    
    // 格式化为: 2026-01-26 14:30:05
    strftime(buf, max_len, "%Y-%m-%d %H:%M:%S", &timeinfo);
    return buf;
}