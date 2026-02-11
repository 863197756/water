// net_manager.h
#pragma once
#include "esp_err.h"


// 初始化网络 (根据 NVS 配置自动选择启动 WiFi 还是 4G)
void net_manager_init(void);

void net_manager_set_mode(int mode); // 0: WiFi, 1: 4G

// 发送数据 (内部自动判断走哪条路)
// payload: 要发送的 JSON 字符串
esp_err_t net_manager_send_data(const char *payload);