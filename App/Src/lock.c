#include "lock.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <stdio.h>

/* ======================= 硬件层 =======================
 * 现在接的是 LY03A 12V 电磁锁（通电开锁、断电落锁），
 * 经 5V 光耦继电器驱动：PC0 高 -> 继电器吸合 -> 锁通电 -> 锁舌收回。
 *
 * 换锁只改下面这三个宏 + 一个驱动函数，业务层一行不用动：
 *   - 换成"断电开锁"的锁（fail-secure）：两个电平宏对调
 *   - 换成电机锁（要正反转）：把 lock_hw() 换成控制 H 桥
 */
#define LOCK_GPIO        GPIOC
#define LOCK_PIN         GPIO_PIN_0
#define LOCK_ON_LEVEL    GPIO_PIN_SET       /* 开锁时 PC0 的电平 */
#define LOCK_OFF_LEVEL   GPIO_PIN_RESET     /* 落锁时 PC0 的电平 */

/* ======================= 时间参数 =====================
 * LOCK_OPEN_MS：一次开锁保持多久（3 秒够人推门）
 * LOCK_OPEN_MAX_MS：硬上限。电磁锁手册写"不可长时间通电（<10s）"，
 *   万一上层逻辑卡住或者有人手动 lock_open 传了大值，
 *   到这里无条件断电，防止线圈烧毁、也防止继电器粘连。
 */
#define LOCK_OPEN_MS      3000
#define LOCK_OPEN_MAX_MS  5000

/* 锁体状态机
 * CLOSED --lock_request--> OPEN --LOCK_OPEN_MS--> CLOSED
 * OPEN 期间再来请求：不重复计数，只把截止时间往后推（延迟关锁）
 */
static uint8_t  s_state;            // LOCK_STATE_CLOSED / LOCK_STATE_OPEN
static uint32_t s_deadline;         // 自动关锁时刻
static uint32_t s_on_tick;          // 本次通电开始的时刻，用来算硬上限
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

/* ---------- 硬件层：换锁只动这里 ---------- */
static void lock_hw(uint8_t on)
{
	HAL_GPIO_WritePin(LOCK_GPIO, LOCK_PIN, on ? LOCK_ON_LEVEL : LOCK_OFF_LEVEL);
}

static void lock_hw_on(void)
{
	lock_hw(1);
	printf("[LOCK] hw=ON  (relay closed, 12V to lock)\r\n");
}

static void lock_hw_off(void)
{
	lock_hw(0);
	printf("[LOCK] hw=OFF (relay open, lock powered down)\r\n");
}

void lock_open(uint32_t ms)
{
	uint32_t now = xTaskGetTickCount();

	if (ms > LOCK_OPEN_MAX_MS)                       // 超过硬上限的请求直接钳住
	{
		ms = LOCK_OPEN_MAX_MS;
	}

	if (s_state == LOCK_STATE_OPEN)
	{
		/* 已经开着：只延长，不重复计数；延长也不能突破硬上限 */
		uint32_t cap = s_on_tick + pdMS_TO_TICKS(LOCK_OPEN_MAX_MS);

		s_deadline = now + pdMS_TO_TICKS(ms);

		if ((int32_t)(s_deadline - cap) > 0)
		{
			s_deadline = cap;
		}

		printf("[LOCK] extend %lu ms\r\n", (unsigned long)ms);
		return;
	}

	lock_hw_on();
	s_state    = LOCK_STATE_OPEN;
	s_on_tick  = now;
	s_deadline = now + pdMS_TO_TICKS(ms);

	if (s_last_src != 0)
	{
		s_open_count++;
		printf("[LOCK] state=OPEN count=%lu src=%s\r\n",
		       (unsigned long)s_open_count, src_name(s_last_src));
	}
}

void lock_close(void)
{
	if (s_state == LOCK_STATE_CLOSED)
	{
		return;
	}

	lock_hw_off();
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
		lock_open(LOCK_OPEN_MS);
		return;
	}

	if (xSemaphoreTake(s_lock_mutex, pdMS_TO_TICKS(200)) == pdTRUE)
	{
		s_last_src = src;
		printf("[LOCK] req src=%s\r\n", src_name(src));
		lock_open(LOCK_OPEN_MS);
		xSemaphoreGive(s_lock_mutex);
	}
	else
	{
		printf("[LOCK] busy\r\n");
	}
}

/* 接线自检用：不经过密码/指纹，直接吸合一次，方便用万用表量继电器 */
void lock_selftest(void)
{
	printf("[LOCK] selftest: relay ON for 1s\r\n");
	lock_hw_on();
	vTaskDelay(pdMS_TO_TICKS(1000));
	lock_hw_off();
	printf("[LOCK] selftest done\r\n");
}

static void vLockTask(void *pvParameters)
{
	(void)pvParameters;

	for (;;)
	{
		if (s_state == LOCK_STATE_OPEN)
		{
			uint32_t now = xTaskGetTickCount();

			/* 第一道：到点自动关 */
			if ((int32_t)(now - s_deadline) >= 0)
			{
				lock_close();
			}
			/* 第二道：硬上限。正常情况下永远走不到这里，
			 * 走到了说明上面的截止时间没生效（或有人绕过状态机直接开了锁） */
			else if ((int32_t)(now - s_on_tick) > pdMS_TO_TICKS(LOCK_OPEN_MAX_MS))
			{
				printf("[LOCK] force off, over %u ms\r\n", (unsigned)LOCK_OPEN_MAX_MS);
				lock_close();
			}
		}

		vTaskDelay(pdMS_TO_TICKS(50));
	}
}

void lock_start(void)
{
	GPIO_InitTypeDef gpio = {0};

	__HAL_RCC_GPIOC_CLK_ENABLE();                // PC0 没在 CubeMX 配，这里手动初始化

	/* 先写电平再配成输出：避免配成输出的那一瞬间电平不定，继电器"嗒"一下 */
	HAL_GPIO_WritePin(LOCK_GPIO, LOCK_PIN, LOCK_OFF_LEVEL);

	gpio.Pin = LOCK_PIN;
	gpio.Mode = GPIO_MODE_OUTPUT_PP;
	gpio.Pull = GPIO_NOPULL;
	gpio.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(LOCK_GPIO, &gpio);

	lock_hw(0);                                  // 上电先锁
	s_state = LOCK_STATE_CLOSED;
	s_on_tick = 0;
	s_last_src = 0;
	s_open_count = 0;
	s_lock_mutex = xSemaphoreCreateMutex();

	xTaskCreate(vLockTask, "LOCK", 128, NULL, 3, NULL);
	printf("[LOCK] start, state=CLOSED, open=%u ms, max=%u ms\r\n",
	       (unsigned)LOCK_OPEN_MS, (unsigned)LOCK_OPEN_MAX_MS);
}
