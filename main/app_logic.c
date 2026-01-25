#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "app_storage.h"
#include "esp_system.h" 
#include "protocol.h"
#include "app_logic.h"





#include "app_logic.h"
#include "protocol.h"
#include "esp_log.h"
#include "app_storage.h" // 需要用到 reset

static const char *TAG = "LOGIC";

void app_logic_init(void) {
    // 可以在这里创建制水任务
    ESP_LOGI(TAG, "App Logic Initialized");
}


void app_logic_handle_cmd(server_cmd_t *cmd) {
    ESP_LOGI(TAG, "Handling CMD: %s, Param: %d", cmd->cmd, cmd->param);

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
    else if (strcmp(cmd->cmd, "wash") == 0) {
        ESP_LOGI(TAG, "Start Washing for %d seconds", cmd->param);
        // TODO: 调用 bsp_pump_valve 进行冲洗
    }
}

// TODO 待完善