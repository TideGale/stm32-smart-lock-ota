#include "lock.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

#define LOCK_GPIO   GPIOC
#define LOCK_PIN    GPIO_PIN_0

static uint8_t  s_open;
static uint32_t s_deadline;

static void lock_hw(uint8_t on)
{
	HAL_GPIO_WritePin(LOCK_GPIO, LOCK_PIN, on ? GPIO_PIN_SET : GPIO_PIN_RESET);  // 高电平吸合
}

void lock_open(uint32_t ms)
{
	lock_hw(1);
	s_open = 1;
	s_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
	printf("[LOCK] open %lu ms\r\n", (unsigned long)ms);
}

void lock_close(void)
{
	lock_hw(0);
	s_open = 0;
	printf("[LOCK] close\r\n");
}

uint8_t lock_is_open(void)
{
	return s_open;
}

static void vLockTask(void *pvParameters)
{
	(void)pvParameters;

	for (;;)
	{
		if (s_open && (int32_t)(xTaskGetTickCount() - s_deadline) >= 0)
		{
			lock_close();                                   // 到点自动关
		}

		vTaskDelay(pdMS_TO_TICKS(50));
	}
}

void lock_start(void)
{
	GPIO_InitTypeDef gpio = {0};

	__HAL_RCC_GPIOC_CLK_ENABLE();                           // PC0 没在 CubeMX 配，这里手动初始化
	gpio.Pin = LOCK_PIN;
	gpio.Mode = GPIO_MODE_OUTPUT_PP;
	gpio.Pull = GPIO_NOPULL;
	gpio.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(LOCK_GPIO, &gpio);

	lock_hw(0);                                             // 上电先锁
	s_open = 0;
	xTaskCreate(vLockTask, "LOCK", 128, NULL, 3, NULL);
	printf("[LOCK] start\r\n");
}
