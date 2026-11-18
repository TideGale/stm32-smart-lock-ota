#ifndef __ESP8266_H
#define __ESP8266_H

#include "main.h"

#define ESP_ST_OFF      0   // 没连通
#define ESP_ST_AT       1   // AT 通了，还没上 WiFi
#define ESP_ST_WIFI     2   // WiFi 连上
#define ESP_ST_MQTT     3   // MQTT 连上

extern volatile uint8_t g_esp_state;

void esp_start(void);          // 初始化 USART3（PB10=TX, PB11=RX），建任务
void esp_publish_state(const char *evt);   // 往 lock/state 发一条状态

#endif

