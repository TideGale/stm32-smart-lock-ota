#include "app_fp.h"
#include "as608.h"
#include "lock.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

/* 录指纹期间保持 1；录完把这里改成 0，重新编译烧录 */
#define AS608_ENROLL_ON_BOOT   0

volatile uint8_t  g_fp_state;
volatile uint16_t g_fp_score;
volatile uint16_t g_fp_page;

static void vFpTask(void *pvParameters)
{
	(void)pvParameters;
	uint16_t page, score, cnt;
	uint8_t st;
	uint8_t fail = 0;

	for (;;)
	{
		/* 1. 先确保模块通，不通就每 2 秒重试，给模块断电重上的机会 */
		st = as608_init();

		if (st != AS608_OK)
		{
			as608_selftest();                    /* only when init fails: real module does not echo */
			printf("[FP] no module, retry in 2s\r\n");
			g_fp_state = FP_ST_OFFLINE;
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
		g_fp_state = FP_ST_IDLE;

		for (;;)
		{
			st = as608_search(&page, &score);

			if (st == AS608_OK)
			{
				g_fp_score = score;
				g_fp_page = page;
				fail = 0;

				if (score < FP_MIN_SCORE)
				{
					/* 匹配到但分数太低，多半是别人手指或按偏了，不开 */
					g_fp_state = FP_ST_LOWSCORE;
					printf("[FP] low score=%u (<%u), reject\r\n", score, FP_MIN_SCORE);
				}
				else
				{
					g_fp_state = FP_ST_MATCH;
					printf("[FP] match page=%u score=%u\r\n", page, score);
					lock_request(SRC_FP);
				}
			}
			else if (st == AS608_NO_MATCH || st == AS608_NOT_FOUND)
			{
				printf("[FP] no match\r\n");
				g_fp_state = FP_ST_NOMATCH;
				fail = 0;
			}
			else if (st == 0xFF)
			{
				if (++fail >= 5)             // 连续超时，回去重新初始化
				{
					printf("[FP] lost module\r\n");
					g_fp_state = FP_ST_OFFLINE;
					break;
				}
			}
			else
			{
				fail = 0;                    // 0x02 没手指等，正常
				g_fp_state = FP_ST_IDLE;
			}

			vTaskDelay(pdMS_TO_TICKS(200));
		}
	}
}

void app_fp_start(void)
{
	g_fp_state = FP_ST_IDLE;
	g_fp_score = 0;
	g_fp_page = 0;

	xTaskCreate(vFpTask, "FP", 320, NULL, 3, NULL);
}
