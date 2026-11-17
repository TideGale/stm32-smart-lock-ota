#include "app_key.h"
#include "keypad.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include <stdio.h>

QueueHandle_t xKeyQueue = NULL;

static void vKeyTask(void *pvParameters)
{
	(void)pvParameters;
	char key;

	for (;;)
	{
		key = keypad_scan();

		if (key != 0)
		{
			uint8_t r, c;
			keypad_last_rc(&r, &c);
			printf("[KEY] %c  r=%u c=%u\r\n", key, r, c);   // 接线排查用
			xQueueSend(xKeyQueue, &key, 0);        // 队列满就丢，不阻塞扫描
		}

		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

void app_key_start(void)
{
	keypad_init();
	xKeyQueue = xQueueCreate(8, sizeof(char));
	xTaskCreate(vKeyTask, "KEY", 192, NULL, 2, NULL);
}
