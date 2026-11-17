#ifndef __APP_KEY_H
#define __APP_KEY_H

#include "main.h"
#include "FreeRTOS.h"
#include "queue.h"

extern QueueHandle_t xKeyQueue;   // 键盘任务往这里塞键值，密码任务取

void app_key_start(void);

#endif
