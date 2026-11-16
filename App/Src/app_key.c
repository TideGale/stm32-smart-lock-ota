#include "app_key.h"
#include "keypad.h"
#include "lock.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

static void vKeyTask(void *pvParameters)
{
	(void)pvParameters;
	char key;

	for (;;)
	{
		key = keypad_scan();

		if (key != 0)
		{
			printf("[KEY] %c\r\n", key);        // 第 15 步改成 xQueueSend

			if (key == '#')                     // 临时：按 # 开锁 3 秒
			{
				lock_open(3000);
			}
		}

		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

void app_key_start(void)
{
	keypad_init();
	xTaskCreate(vKeyTask, "KEY", 192, NULL, 2, NULL);
}
