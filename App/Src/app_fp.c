#include "app_fp.h"
#include "as608.h"
#include "lock.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

/* 录指纹期间保持 1；录完把这里改成 0，重新编译烧录 */
#define AS608_ENROLL_ON_BOOT   0

static void vFpTask(void *pvParameters)
{
	(void)pvParameters;
	uint16_t page, score, cnt;
	uint8_t st;
	uint8_t fail = 0;

	for (;;)
	{
		/* 1. 先确保模块通，不通就每 2 秒重试，给模块断电重上的机会 */
		as608_selftest();                        // 回环自测，接线排查用
		st = as608_init();

		if (st != AS608_OK)
		{
			printf("[FP] no module, retry in 2s\r\n");
			vTaskDelay(pdMS_TO_TICKS(2000));
			continue;
		}

		if (as608_count(&cnt) == AS608_OK)
		{
			printf("[FP] lib count=%u\r\n", cnt);
		}

		/* 2. 需要时录入一次 */
#if AS608_ENROLL_ON_BOOT
		printf("[FP] enroll page1\r\n");
		st = as608_enroll(1);
		printf("[FP] enroll=%u\r\n", st);

		if (as608_count(&cnt) == AS608_OK)
		{
			printf("[FP] lib count=%u\r\n", cnt);
		}
#endif

		/* 3. 搜索循环 */
		fail = 0;

		for (;;)
		{
			st = as608_search(&page, &score);

			if (st == AS608_OK)
			{
				printf("[FP] match page=%u score=%u\r\n", page, score);
				lock_open(3000);
				fail = 0;
			}
			else if (st == AS608_NO_MATCH || st == AS608_NOT_FOUND)
			{
				printf("[FP] no match\r\n");
				fail = 0;
			}
			else if (st == 0xFF)
			{
				if (++fail >= 5)             // 连续超时，回去重新初始化
				{
					printf("[FP] lost module\r\n");
					break;
				}
			}
			else
			{
				fail = 0;                    // 0x02 没手指等，正常
			}

			vTaskDelay(pdMS_TO_TICKS(200));
		}
	}
}

void app_fp_start(void)
{
	xTaskCreate(vFpTask, "FP", 320, NULL, 3, NULL);
}
