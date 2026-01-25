#pragma once
#include "protocol.h"

// 初始化业务逻辑
void app_logic_init(void);

// 处理来自服务器的指令 (MQTT Manager 会调用此函数)
void app_logic_handle_cmd(server_cmd_t *cmd);