#include "app_oled.h"
#include "ssd1306.h"
#include "ota_meta.h"
#include "lock.h"
#include "app_pass.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

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

static void vOledTask(void *pvParameters)
{
	(void)pvParameters;
	char line[24];

	for (;;)
	{
		ssd1306_clear();
		ssd1306_draw_string(0, 0, "SmartLock");

		if (SCB->VTOR == OTA_SLOT_A_ADDR)
		{
			ssd1306_draw_string(0, 2, "OTA slot: A");
		}
		else
		{
			ssd1306_draw_string(0, 2, "OTA slot: B");
		}

		/* 锁体状态：CLOSED / OPEN 3s / 最近一次来源 / 累计次数 */
		if (lock_get_state() == LOCK_STATE_OPEN)
		{
			snprintf(line, sizeof(line), "OPEN %us by %s",
			         lock_get_remain_s(), src_name(lock_get_last_src()));
			ssd1306_draw_string(0, 4, line);
		}
		else
		{
			snprintf(line, sizeof(line), "CLOSED  cnt:%lu",
			         (unsigned long)lock_get_open_count());
			ssd1306_draw_string(0, 4, line);
		}

		/* 密码输入状态：星号回显 / 剩余输入位数 / 锁定倒计时 */
		if (g_pass_locked)
		{
			snprintf(line, sizeof(line), "LOCKED %us left", g_pass_remain_s);
		}
		else
		{
			uint8_t i;
			line[0] = 'P';
			line[1] = ':';

			for (i = 0; i < g_pass_len && i < 6; i++)
			{
				line[2 + i] = '*';
			}

			line[2 + i] = '\0';
		}
		ssd1306_draw_string(0, 6, line);

		ssd1306_refresh();
		vTaskDelay(pdMS_TO_TICKS(500));
	}
}

void app_oled_start(I2C_HandleTypeDef *hi2c)
{
	ssd1306_init(hi2c);

	if (HAL_I2C_IsDeviceReady(hi2c, SSD1306_I2C_ADDR, 3, 100) != HAL_OK)
	{
		printf("[OLED] not found, check 0x78 / PB6 / PB7\r\n");
	}

	xTaskCreate(vOledTask, "OLED", 256, NULL, 1, NULL);
}
