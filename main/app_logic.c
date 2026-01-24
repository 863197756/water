#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "app_storage.h"
#include "esp_system.h" 
#include "protocol.h"

// 假设这是你处理指令的地方
void handle_server_cmd(server_cmd_t *cmd) {
    if (strcmp(cmd->cmd, "reset") == 0) {
        if (cmd->param == 1) {
            ESP_LOGW("CMD", "收到指令：重置网络");
            app_storage_erase(RESET_LEVEL_NET);
            
            // 动作：通常重置后需要重启进入配网模式
            esp_restart(); 
            
        } else if (cmd->param == 9) {
            ESP_LOGW("CMD", "收到指令：恢复出厂设置");
            app_storage_erase(RESET_LEVEL_FACTORY);
            esp_restart();
        }
    }
    // ... 其他指令 (wash 等)
}
// TODO 待完善