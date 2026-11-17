#include "app_pass.h"
#include "app_key.h"
#include "lock.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include <stdio.h>
#include <string.h>

volatile uint8_t  g_pass_len;
volatile char     g_pass_disp[8];
volatile uint8_t  g_pass_locked;
volatile uint16_t g_pass_remain_s;

static char     s_buf[PASS_BUF_MAX + 1];    // 多留一位给 '\0'
static uint8_t  s_len;
static uint8_t  s_fail;
static uint8_t  s_locked;
static uint32_t s_unlock_tick;

static void pass_clear(void)
{
	s_len = 0;
	s_buf[0] = '\0';
	g_pass_len = 0;
	g_pass_disp[0] = '\0';
}

static void pass_lock(uint32_t ms)
{
	s_locked = 1;
	s_fail = 0;
	s_unlock_tick = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
	g_pass_locked = 1;
	printf("[PASS] fail %u/%u, locked\r\n", PASS_MAX_FAIL, PASS_MAX_FAIL);
}

/* 返回 1 = 密码正确 */
static uint8_t pass_submit(void)
{
	if (s_len == 0)
	{
		printf("[PASS] empty\r\n");
		return 0;
	}

	if (s_len == PASS_LEN && strcmp(s_buf, PASS_CODE) == 0)
	{
		printf("[PASS] ok\r\n");
		pass_clear();
		s_fail = 0;
		lock_request(SRC_PASS);
		return 1;
	}

	pass_clear();
	s_fail++;
	printf("[PASS] fail %u/%u\r\n", s_fail, PASS_MAX_FAIL);

	if (s_fail >= PASS_MAX_FAIL)
	{
		pass_lock(PASS_LOCK_MS);
	}

	return 0;
}

static void pass_handle_key(char key)
{
	if (key >= '0' && key <= '9')
	{
		if (s_len >= PASS_BUF_MAX)
		{
			printf("[PASS] max %u digits, press #\r\n", (unsigned)PASS_BUF_MAX);
			return;
		}

		s_buf[s_len++] = key;
		s_buf[s_len] = '\0';
		g_pass_len = s_len;
		strncpy((char *)g_pass_disp, s_buf, PASS_BUF_MAX);   // 同步一份给 OLED
		g_pass_disp[PASS_BUF_MAX] = '\0';
		printf("[PASS] key=%c  len=%u/%u  disp=%s\r\n",
		       key, s_len, (unsigned)PASS_BUF_MAX, (const char *)g_pass_disp);
		return;
	}

	if (key == '*')                          // 退格
	{
		if (s_len > 0)
		{
			s_len--;
			s_buf[s_len] = '\0';
			g_pass_len = s_len;
			strncpy((char *)g_pass_disp, s_buf, PASS_BUF_MAX);
			g_pass_disp[PASS_BUF_MAX] = '\0';
		}

		printf("[PASS] back  len=%u\r\n", s_len);
		return;
	}

	if (key == '#')                          // 提交
	{
		pass_submit();
		return;
	}

	/* A~D 这一步先不用，留给菜单 */
}

static void vPassTask(void *pvParameters)
{
	(void)pvParameters;
	char key;
	uint32_t left;

	for (;;)
	{
		/* 锁定期内：检查是否到期 */
		if (s_locked)
		{
			if ((int32_t)(xTaskGetTickCount() - s_unlock_tick) >= 0)
			{
				s_locked = 0;
				g_pass_locked = 0;
				g_pass_remain_s = 0;
				printf("[PASS] unlocked\r\n");
			}
			else
			{
				left = (s_unlock_tick - xTaskGetTickCount()) * portTICK_PERIOD_MS;
				g_pass_remain_s = (uint16_t)(left / 1000);
			}
		}

		/* 取键，200ms 超时，方便上面那段到期检查 */
		if (xQueueReceive(xKeyQueue, &key, pdMS_TO_TICKS(200)) != pdTRUE)
		{
			continue;
		}

		if (s_locked)
		{
			printf("[PASS] locked, %us left\r\n", g_pass_remain_s);
			continue;                        // 锁定期间连正确密码也不开
		}

		pass_handle_key(key);
	}
}

void app_pass_start(void)
{
	s_len = 0;
	s_fail = 0;
	s_locked = 0;
	s_buf[0] = '\0';
	g_pass_len = 0;
	g_pass_locked = 0;
	g_pass_remain_s = 0;

	xTaskCreate(vPassTask, "PASS", 256, NULL, 2, NULL);
	printf("[PASS] start, code len=%u\r\n", (unsigned)PASS_LEN);
}
