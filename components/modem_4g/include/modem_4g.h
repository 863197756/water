#pragma once
#include "esp_err.h"

// 初始化 4G 模组 (配置 GPIO、UART 等)
void modem_4g_init(void);

// 启动拨号 (开始 PPPoS 协商)
void modem_4g_start(void);

// 停止拨号 (断开网络，低功耗)
void modem_4g_stop(void);