#include "app_oled.h"
#include "ssd1306.h"
#include "ota_meta.h"
#include "lock.h"
#include "app_pass.h"
#include "app_fp.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

#define LINE_NUM    5

static const char *src_name(uint8_t src)
{
	switch (src)
	{
		case SRC_PASS: return "PASS";
		case SRC_FP:   return "FP";
		case SRC_NET:  return "NET";
		default:       return "-";
	}
}

static void oled_fp_line(char *line, size_t n)
{
	switch (g_fp_state)
	{
		case FP_ST_MATCH:
			snprintf(line, n, "FP:OK id%u s%u", g_fp_page, g_fp_score);
			break;

		case FP_ST_LOWSCORE:
			snprintf(line, n, "FP:low s%u", g_fp_score);
			break;

		case FP_ST_NOMATCH:
			snprintf(line, n, "FP:no match");
			break;

		case FP_ST_OFFLINE:
			snprintf(line, n, "FP:offline");
			break;

		default:
			snprintf(line, n, "FP:ready");
			break;
	}
}

static void vOledTask(void *pvParameters)
{
	(void)pvParameters;
	char cur[LINE_NUM][24];
	char prev[LINE_NUM][24];
	uint8_t i;
	uint32_t err_last = 0;
	uint32_t err_now;

	for (i = 0; i < LINE_NUM; i++)
	{
		prev[i][0] = '\0';
	}

	for (;;)
	{
		/* 1. 先把这一帧要显示的内容全部生成好 */
		if (SCB->VTOR == OTA_SLOT_A_ADDR)
		{
			snprintf(cur[0], sizeof(cur[0]), "SmartLock  A");
		}
		else
		{
			snprintf(cur[0], sizeof(cur[0]), "SmartLock  B");
		}

		if (lock_get_state() == LOCK_STATE_OPEN)
		{
			snprintf(cur[1], sizeof(cur[1]), "OPEN %us %s",
			         lock_get_remain_s(), src_name(lock_get_last_src()));
		}
		else
		{
			snprintf(cur[1], sizeof(cur[1]), "CLOSED cnt:%lu",
			         (unsigned long)lock_get_open_count());
		}

		if (g_pass_locked)
		{
			snprintf(cur[2], sizeof(cur[2]), "LOCKED %us", g_pass_remain_s);
		}
		else
		{
			snprintf(cur[2], sizeof(cur[2]), "P:%s", (const char *)g_pass_disp);
		}

		oled_fp_line(cur[3], sizeof(cur[3]));

		snprintf(cur[4], sizeof(cur[4]), "1-4 pass 1234#");

		/* 2. 只重画和刷新内容变了的那一页，I2C 流量大幅降低 */
		for (i = 0; i < LINE_NUM; i++)
		{
			if (strcmp(cur[i], prev[i]) == 0)
			{
				continue;
			}

			/* 整页清空再画，避免上一次的长文本残留 */
			ssd1306_fill_page(i, 0);

			if (cur[i][0] != '\0')
			{
				ssd1306_draw_string(0, i, cur[i]);
			}

			ssd1306_refresh_page(i);
			strcpy(prev[i], cur[i]);
		}

		/* 3. I2C 出错就打印并尝试救回总线，避免屏永久卡住 */
		err_now = ssd1306_err_count();

		if (err_now != err_last)
		{
			printf("[OLED] i2c err total=%lu\r\n", (unsigned long)err_now);
			err_last = err_now;
			ssd1306_bus_recover();
			ssd1306_clear();                       // 恢复后全屏重画
			memset(prev, 0, sizeof(prev));
		}

		vTaskDelay(pdMS_TO_TICKS(300));
	}
}

void app_oled_start(I2C_HandleTypeDef *hi2c)
{
	ssd1306_init(hi2c);

	if (HAL_I2C_IsDeviceReady(hi2c, SSD1306_I2C_ADDR, 3, 100) != HAL_OK)
	{
		printf("[OLED] not found, check 0x78 / PB6 / PB7\r\n");
	}

	xTaskCreate(vOledTask, "OLED", 384, NULL, 1, NULL);
}
