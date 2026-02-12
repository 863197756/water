// net_manager.h
#pragma once
#include "esp_err.h"


// 初始化网络 (根据 NVS 配置自动选择启动 WiFi 还是 4G)
void net_manager_init(void);

void net_manager_set_mode(int mode); // 0: WiFi, 1: 4G
