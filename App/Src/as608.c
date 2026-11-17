#include "as608.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdio.h>

static UART_HandleTypeDef s_uart;

/* 调度器起来后用 vTaskDelay，别用 HAL_Delay 死等 */
static void as608_delay(uint32_t ms)
{
	if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
	{
		vTaskDelay(pdMS_TO_TICKS(ms));
	}
	else
	{
		HAL_Delay(ms);
	}
}

/* UART4 自己初始化：CubeMX 的 .ioc 里没有 UART4，这里直接配 */
static void as608_uart_config(uint32_t baud)
{
	GPIO_InitTypeDef gpio = {0};

	__HAL_RCC_GPIOC_CLK_ENABLE();
	__HAL_RCC_UART4_CLK_ENABLE();

	gpio.Pin   = GPIO_PIN_10;                    // PC10 = UART4_TX
	gpio.Mode  = GPIO_MODE_AF_PP;
	gpio.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(GPIOC, &gpio);

	gpio.Pin  = GPIO_PIN_11;                     // PC11 = UART4_RX
	gpio.Mode = GPIO_MODE_INPUT;
	gpio.Pull = GPIO_PULLUP;
	HAL_GPIO_Init(GPIOC, &gpio);

	s_uart.Instance          = UART4;
	s_uart.Init.BaudRate     = baud;
	s_uart.Init.WordLength   = UART_WORDLENGTH_8B;
	s_uart.Init.StopBits     = UART_STOPBITS_1;
	s_uart.Init.Parity       = UART_PARITY_NONE;
	s_uart.Init.Mode         = UART_MODE_TX_RX;
	s_uart.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
	s_uart.Init.OverSampling = UART_OVERSAMPLING_16;

	HAL_UART_Init(&s_uart);
}

/* 清掉 RX 里残留字节，顺带清 ORE */
static void as608_flush(void)
{
	volatile uint32_t dummy;
	uint16_t guard = 256;

	dummy = s_uart.Instance->SR;                 // 读 SR 再读 DR 才能清 ORE
	(void)dummy;

	while (guard--)
	{
		if (s_uart.Instance->SR & USART_SR_RXNE)
		{
			dummy = s_uart.Instance->DR;
			(void)dummy;
		}
		else
		{
			break;
		}
	}
}

static uint8_t as608_send(uint8_t cmd, const uint8_t *params, uint16_t plen)
{
	uint8_t buf[64];
	uint16_t i = 0;
	uint16_t len = 1 + plen + 2;                 // 指令 + 参数 + 校验
	uint16_t sum = 0;

	buf[i++] = 0xEF;
	buf[i++] = 0x01;
	buf[i++] = 0xFF;
	buf[i++] = 0xFF;
	buf[i++] = 0xFF;
	buf[i++] = 0xFF;
	buf[i++] = 0x01;                             // 命令包
	buf[i++] = (uint8_t)(len >> 8);
	buf[i++] = (uint8_t)(len & 0xFF);
	buf[i++] = cmd;

	for (uint16_t k = 0; k < plen; k++)
	{
		buf[i++] = params[k];
	}

	for (uint16_t k = 6; k < i; k++)             // 从包标识开始累加
	{
		sum += buf[k];
	}

	buf[i++] = (uint8_t)(sum >> 8);
	buf[i++] = (uint8_t)(sum & 0xFF);

	as608_flush();                               // 发之前先清干净

	return (HAL_UART_Transmit(&s_uart, buf, i, 200) == HAL_OK) ? 0 : 1;
}

/* 收一个字节，超时按真实毫秒算 */
static uint8_t as608_getc(uint8_t *b, uint32_t timeout_ms)
{
	uint32_t start = HAL_GetTick();

	while ((HAL_GetTick() - start) < timeout_ms)
	{
		if (s_uart.Instance->SR & USART_SR_RXNE)
		{
			*b = (uint8_t)(s_uart.Instance->DR & 0xFF);
			return 0;
		}
	}

	return 1;
}

/* 收一个完整应答包；*alen = 包长度（含确认码和校验） */
static uint8_t as608_recv(uint8_t *ack, uint16_t *alen, uint32_t timeout_ms)
{
	uint8_t b;
	uint8_t h;
	uint8_t idx = 0;
	uint16_t len = 0;
	uint32_t start = HAL_GetTick();

	/* 1. 找包头 EF 01 */
	for (;;)
	{
		if ((HAL_GetTick() - start) >= timeout_ms)
		{
			return 1;
		}

		if (as608_getc(&b, 20))
		{
			continue;
		}

		if (b != 0xEF)
		{
			continue;
		}

		if (as608_getc(&h, 20) == 0 && h == 0x01)
		{
			break;
		}
	}

	/* 2. 地址 4 字节 + 包标识 1 字节 */
	for (uint8_t k = 0; k < 5; k++)
	{
		if (as608_getc(&b, 100))
		{
			return 1;
		}
	}

	/* 3. 长度 2 字节 */
	if (as608_getc(&b, 100))
	{
		return 1;
	}
	len = (uint16_t)b << 8;

	if (as608_getc(&b, 100))
	{
		return 1;
	}
	len |= b;

	if (len == 0 || len > 32)
	{
		return 1;
	}

	/* 4. 包内容：确认码 + 参数 + 校验 */
	for (idx = 0; idx < len; idx++)
	{
		if (as608_getc(ack + idx, 100))
		{
			return 1;
		}
	}

	*alen = len;
	return 0;
}

/* 发命令并收应答，返回确认码（ack[0]），通信失败返回 0xFF */
static uint8_t as608_cmd(uint8_t cmd, const uint8_t *params, uint16_t plen, uint32_t timeout_ms)
{
	uint8_t ack[32];
	uint16_t alen;

	if (as608_send(cmd, params, plen))
	{
		return 0xFF;
	}

	if (as608_recv(ack, &alen, timeout_ms))
	{
		return 0xFF;
	}

	return ack[0];
}

/* 单字节回环自测：PC10 和 PC11 短接时应该收到自己发的 0x55 */
uint8_t as608_selftest(void)
{
	uint8_t tx = 0x55;
	uint8_t got = 0;
	uint32_t sr = 0;
	uint8_t ok = 0;

	as608_uart_config(9600);
	as608_delay(50);
	as608_flush();

	HAL_UART_Transmit(&s_uart, &tx, 1, 100);
	as608_delay(20);

	sr = s_uart.Instance->SR;

	if (sr & USART_SR_RXNE)
	{
		got = (uint8_t)(s_uart.Instance->DR & 0xFF);
		ok = (got == 0x55) ? 1 : 0;
	}

	printf("[FP] selftest sr=0x%04lX got=0x%02X %s\r\n",
	       (unsigned long)sr, got, ok ? "OK" : "FAIL");

	return ok;
}

static uint8_t as608_ping(uint32_t baud)
{
	uint8_t pwd[4] = {0x00, 0x00, 0x00, 0x00};
	uint8_t st;

	as608_uart_config(baud);
	as608_delay(100);
	printf("[FP] try %lu\r\n", (unsigned long)baud);

	st = as608_cmd(0x13, pwd, 4, 1000);          // 验证口令

	if (st == 0xFF)
	{
		printf("[FP] no ack @%lu\r\n", (unsigned long)baud);
	}

	return st;
}

uint8_t as608_init(void)
{
	uint8_t st;
	uint8_t round;

	for (round = 0; round < 3; round++)
	{
		st = as608_ping(57600);                  // 这块模块实测是 57600
		if (st != 0xFF)
		{
			printf("[FP] init=%u @57600\r\n", st);
			return st;
		}

		st = as608_ping(9600);
		if (st != 0xFF)
		{
			printf("[FP] init=%u @9600\r\n", st);
			return st;
		}
	}

	printf("[FP] init=255\r\n");
	return 0xFF;
}

uint8_t as608_count(uint16_t *cnt)
{
	uint8_t ack[32];
	uint16_t alen;

	if (as608_send(0x1D, NULL, 0))               // 读模板总数
	{
		return 0xFF;
	}
	if (as608_recv(ack, &alen, 1000))
	{
		return 0xFF;
	}
	if (ack[0] != AS608_OK)
	{
		return ack[0];
	}

	*cnt = ((uint16_t)ack[1] << 8) | ack[2];
	return AS608_OK;
}

/* 取一次图，code 返回确认码 */
static uint8_t as608_genimg(uint8_t *code)
{
	uint8_t st = as608_cmd(0x01, NULL, 0, 3000);

	*code = st;
	return (st == 0xFF) ? 1 : 0;
}

/* 一直等到真的按上手指：0=取到图 */
static uint8_t as608_wait_press(uint32_t timeout_ms)
{
	uint32_t start = HAL_GetTick();
	uint8_t code;
	uint32_t shown = 0;

	for (;;)
	{
		if ((HAL_GetTick() - start) >= timeout_ms)
		{
			return 0xFF;
		}

		if (as608_genimg(&code) == 0)
		{
			if (code == AS608_OK)
			{
				return AS608_OK;
			}

			if (code != AS608_NO_FINGER)
			{
				if (shown == 0)                 // 错误码只打一次
				{
					printf("[FP] genimg err=%u\r\n", code);
					shown = 1;
				}
			}
		}

		as608_delay(100);
	}
}

/* 一直等到手指抬起 */
static uint8_t as608_wait_release(uint32_t timeout_ms)
{
	uint32_t start = HAL_GetTick();
	uint8_t code;

	for (;;)
	{
		if ((HAL_GetTick() - start) >= timeout_ms)
		{
			return 0xFF;
		}

		if (as608_genimg(&code) == 0 && code == AS608_NO_FINGER)
		{
			return AS608_OK;
		}

		as608_delay(100);
	}
}

uint8_t as608_enroll(uint16_t page)
{
	uint8_t store[3] = {0x01, (uint8_t)(page >> 8), (uint8_t)(page & 0xFF)};
	uint8_t st;

	printf("[FP] put finger\r\n");
	st = as608_wait_press(20000);
	if (st != AS608_OK)
	{
		printf("[FP] press fail=%u\r\n", st);
		return st;
	}
	printf("[FP] img1 ok\r\n");

	st = as608_cmd(0x02, &(uint8_t){0x01}, 1, 3000);   // 图 -> Buffer1
	printf("[FP] buf1=%u\r\n", st);
	if (st != AS608_OK)
	{
		return st;
	}

	printf("[FP] lift finger\r\n");
	as608_wait_release(10000);

	printf("[FP] put same finger again\r\n");
	st = as608_wait_press(20000);
	if (st != AS608_OK)
	{
		printf("[FP] press2 fail=%u\r\n", st);
		return st;
	}
	printf("[FP] img2 ok\r\n");

	st = as608_cmd(0x02, &(uint8_t){0x02}, 1, 3000);   // 图 -> Buffer2
	printf("[FP] buf2=%u\r\n", st);
	if (st != AS608_OK)
	{
		return st;
	}

	st = as608_cmd(0x05, NULL, 0, 3000);               // 合成模板
	printf("[FP] model=%u\r\n", st);
	if (st != AS608_OK)
	{
		return st;                                     // 0x0A = 两次不匹配
	}

	st = as608_cmd(0x06, store, 3, 3000);              // 存模板
	printf("[FP] store=%u\r\n", st);

	return st;
}

uint8_t as608_search(uint16_t *page, uint16_t *score)
{
	uint8_t ack[32];
	uint16_t alen;
	uint8_t buf_id = 0x01;
	uint8_t search[5] = {0x01, 0x00, 0x00, 0x03, 0xE7};   // 缓冲区1，0 开始，搜 999 个
	uint8_t st;

	st = as608_cmd(0x01, NULL, 0, 3000);               // 拍照
	if (st != AS608_OK)
	{
		return st;
	}

	st = as608_cmd(0x02, &buf_id, 1, 3000);            // 图像转特征
	if (st != AS608_OK)
	{
		return st;
	}

	if (as608_send(0x04, search, 5))                   // 搜索
	{
		return 0xFF;
	}
	if (as608_recv(ack, &alen, 3000))
	{
		return 0xFF;
	}
	if (ack[0] != AS608_OK)
	{
		return ack[0];
	}

	*page  = ((uint16_t)ack[1] << 8) | ack[2];
	*score = ((uint16_t)ack[3] << 8) | ack[4];
	return AS608_OK;
}

uint8_t as608_delete(uint16_t page, uint16_t count)
{
	uint8_t d[4] = {(uint8_t)(page >> 8), (uint8_t)(page & 0xFF),
	                (uint8_t)(count >> 8), (uint8_t)(count & 0xFF)};

	return as608_cmd(0x0C, d, 4, 1000);
}
