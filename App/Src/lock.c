#include "lock.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <stdio.h>

#define LOCK_GPIO   GPIOC
#define LOCK_PIN    GPIO_PIN_0

/* 锁体状态机
 * CLOSED --lock_request/open--> OPEN --3s--> CLOSED
 * OPEN 期间再来请求：不重复计数，只把截止时间往后推（相当于延迟关锁）
 */
static uint8_t  s_state;            // LOCK_STATE_CLOSED / LOCK_STATE_OPEN
static uint32_t s_deadline;         // 自动关锁时刻
static uint8_t  s_last_src;         // 最近一次开锁来源
static uint32_t s_open_count;       // 累计开锁次数
static SemaphoreHandle_t s_lock_mutex;

static const char *src_name(uint8_t src)
{
	switch (src)
	{
		case SRC_PASS: return "PASS";
		case SRC_FP:   return "FP";
		case SRC_NET:  return "NET";
		default:       return "?";
	}
}

static void lock_hw(uint8_t on)
{
	HAL_GPIO_WritePin(LOCK_GPIO, LOCK_PIN, on ? GPIO_PIN_SET : GPIO_PIN_RESET);  // 高电平吸合
}

void lock_open(uint32_t ms)
{
	uint32_t now = xTaskGetTickCount();

	if (s_state == LOCK_STATE_OPEN)
	{
		/* 已经开着：只延长，不重复计数 */
		s_deadline = now + pdMS_TO_TICKS(ms);
		printf("[LOCK] extend %lu ms\r\n", (unsigned long)ms);
		return;
	}

	lock_hw(1);
	s_state = LOCK_STATE_OPEN;
	s_deadline = now + pdMS_TO_TICKS(ms);

	if (s_last_src != 0)
	{
		s_open_count++;
		printf("[LOCK] state=OPEN count=%lu\r\n", (unsigned long)s_open_count);
	}
}

void lock_close(void)
{
	if (s_state == LOCK_STATE_CLOSED)
	{
		return;
	}

	lock_hw(0);
	s_state = LOCK_STATE_CLOSED;
	printf("[LOCK] state=CLOSED\r\n");
}

uint8_t lock_is_open(void)
{
	return (s_state == LOCK_STATE_OPEN) ? 1 : 0;
}

uint8_t lock_get_state(void)
{
	return s_state;
}

uint8_t lock_get_last_src(void)
{
	return s_last_src;
}

uint32_t lock_get_open_count(void)
{
	return s_open_count;
}

uint16_t lock_get_remain_s(void)
{
	uint32_t left;

	if (s_state != LOCK_STATE_OPEN)
	{
		return 0;
	}

	left = (s_deadline - xTaskGetTickCount()) * portTICK_PERIOD_MS;
	return (uint16_t)(left / 1000);
}

/* 指纹、密码、远程三路都调这个，互斥串行，不会同时开 */
void lock_request(uint8_t src)
{
	if (s_lock_mutex == NULL)                    // 调度器还没起来（理论上不会）
	{
		s_last_src = src;
		lock_open(3000);
		return;
	}

	if (xSemaphoreTake(s_lock_mutex, pdMS_TO_TICKS(200)) == pdTRUE)
	{
		s_last_src = src;
		printf("[LOCK] req src=%s\r\n", src_name(src));
		lock_open(3000);
		xSemaphoreGive(s_lock_mutex);
	}
	else
	{
		printf("[LOCK] busy\r\n");
	}
}

static void vLockTask(void *pvParameters)
{
	(void)pvParameters;

	for (;;)
	{
		if (s_state == LOCK_STATE_OPEN &&
		    (int32_t)(xTaskGetTickCount() - s_deadline) >= 0)
		{
			lock_close();                        // 到点自动关
		}

		vTaskDelay(pdMS_TO_TICKS(50));
	}
}

void lock_start(void)
{
	GPIO_InitTypeDef gpio = {0};

	__HAL_RCC_GPIOC_CLK_ENABLE();                // PC0 没在 CubeMX 配，这里手动初始化
	gpio.Pin = LOCK_PIN;
	gpio.Mode = GPIO_MODE_OUTPUT_PP;
	gpio.Pull = GPIO_NOPULL;
	gpio.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(LOCK_GPIO, &gpio);

	lock_hw(0);                                  // 上电先锁
	s_state = LOCK_STATE_CLOSED;
	s_last_src = 0;
	s_open_count = 0;
	s_lock_mutex = xSemaphoreCreateMutex();

	xTaskCreate(vLockTask, "LOCK", 128, NULL, 3, NULL);
	printf("[LOCK] start, state=CLOSED\r\n");
}
