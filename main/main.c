#include <stdio.h>
#include <string.h> // 记得加这个，因为用了 strcmp 或 memset
#include "esp_wifi.h"
#include "esp_log.h"   // 加日志头文件
#include "blufi_custom.h"

static const char *TAG = "MAIN";

void app_main(void)
{
   
 


    blufi_custom_init(); 
}