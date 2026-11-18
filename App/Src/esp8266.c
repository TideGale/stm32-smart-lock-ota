#include "esp8266.h"
#include "lock.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

/* 连哪个 WiFi：手机热点，必须是 2.4GHz */
#define WIFI_SSID       "Redmi K70E"
#define WIFI_PASS       "88888888"

/* 公共 MQTT 服务器，连不上就换下一个。hivemq 放第一个：emqx 公共服实测会拒连 */
static const char *const s_brokers[] =
{
	"broker.hivemq.com",
	"test.mosquitto.org",
	"broker.emqx.io",
};

#define BROKER_NUM   (sizeof(s_brokers) / sizeof(s_brokers[0]))
#define MQTT_PORT    1883

#define TOPIC_CMD       "lock/cmd"      /* 手机发开锁指令到这里 */
#define TOPIC_STATE     "lock/state"    /* 门锁上报状态到这里 */

static UART_HandleTypeDef s_uart;

/* 中断收 + 环形缓冲：轮询收会被 FreeRTOS 的 1ms tick 打断，
 * 115200 下 1 字节只要 87us，中断一来就丢字节（AT version 被截断就是这原因） */
#define ESP_RX_SIZE   512

static volatile uint8_t  s_rx[ESP_RX_SIZE];
static volatile uint16_t s_rx_head;
static volatile uint16_t s_rx_tail;
static uint8_t s_rx_byte;

void USART3_IRQHandler(void)
{
	HAL_UART_IRQHandler(&s_uart);
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
	if (huart->Instance != USART3)
	{
		return;
	}

	uint16_t next = (uint16_t)((s_rx_head + 1) % ESP_RX_SIZE);

	if (next != s_rx_tail)                  // 满了就丢最新的，不覆盖旧数据
	{
		s_rx[s_rx_head] = s_rx_byte;
		s_rx_head = next;
	}

	HAL_UART_Receive_IT(&s_uart, &s_rx_byte, 1);
}

volatile uint8_t g_esp_state;

/* USART3 自己初始化：PB10 = TX，PB11 = RX
 * 不走 CubeMX，避免重新生成代码时把 SVC/PendSV、SysTick 那几处再冲一遍 */
static void esp_uart_config(void)
{
	GPIO_InitTypeDef gpio = {0};

	__HAL_RCC_GPIOB_CLK_ENABLE();
	__HAL_RCC_USART3_CLK_ENABLE();

	gpio.Pin   = GPIO_PIN_10;                    // PB10 = USART3_TX
	gpio.Mode  = GPIO_MODE_AF_PP;
	gpio.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(GPIOB, &gpio);

	gpio.Pin  = GPIO_PIN_11;                     // PB11 = USART3_RX
	gpio.Mode = GPIO_MODE_INPUT;
	gpio.Pull = GPIO_PULLUP;
	HAL_GPIO_Init(GPIOB, &gpio);

	s_uart.Instance          = USART3;
	s_uart.Init.BaudRate     = 115200;
	s_uart.Init.WordLength   = UART_WORDLENGTH_8B;
	s_uart.Init.StopBits     = UART_STOPBITS_1;
	s_uart.Init.Parity       = UART_PARITY_NONE;
	s_uart.Init.Mode         = UART_MODE_TX_RX;
	s_uart.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
	s_uart.Init.OverSampling = UART_OVERSAMPLING_16;

	HAL_UART_Init(&s_uart);

	HAL_NVIC_SetPriority(USART3_IRQn, 5, 0);
	HAL_NVIC_EnableIRQ(USART3_IRQn);
	HAL_UART_Receive_IT(&s_uart, &s_rx_byte, 1);
}

static void esp_flush(void)
{
	s_rx_tail = s_rx_head;
}

static void esp_send(const char *s)
{
	HAL_UART_Transmit(&s_uart, (uint8_t *)s, (uint16_t)strlen(s), 500);
}

/* 把窗口内收到的字节整包清空到 buf，不做行切分，避免回显和应答粘连 */
static uint16_t esp_drain(char *buf, uint16_t max, uint32_t ms)
{
	uint16_t n = 0;
	uint32_t start = HAL_GetTick();

	while ((HAL_GetTick() - start) < ms && n < max - 1)
	{
		if (s_rx_tail != s_rx_head)
		{
			buf[n++] = (char)s_rx[s_rx_tail];
			s_rx_tail = (uint16_t)((s_rx_tail + 1) % ESP_RX_SIZE);
		}
	}

	buf[n] = '\0';
	return n;
}

/* 打出来时把不可见字符换成点，方便看 */
static void esp_print_raw(const char *buf, uint16_t n)
{
	printf("[ESP] << ");

	for (uint16_t i = 0; i < n; i++)
	{
		char c = buf[i];
		putchar((c == '\r') ? ' ' : ((c == '\n') ? '|' : ((c >= ' ' && c <= '~') ? c : '.')));
	}

	printf("\r\n");
}

/* 发一条 AT 指令，整包收 800ms，然后在整包里搜 OK / ERROR */
static uint8_t esp_at(const char *cmd, uint32_t timeout_ms)
{
	char buf[192];
	uint16_t n;
	uint16_t total = 0;
	uint32_t start;

	esp_flush();
	printf("[ESP] >> %s", cmd);
	esp_send(cmd);

	start = HAL_GetTick();
	buf[0] = '\0';

	/* 分小片收，一看到 OK/ERROR 就返回，不能死等满超时——
	 * broker 等不到 MQTT CONNECT 会主动断 TCP，等太久连接就没了 */
	while (((HAL_GetTick() - start) < timeout_ms) && (total < (sizeof(buf) - 1)))
	{
		n = esp_drain(&buf[total], (uint16_t)(sizeof(buf) - 1 - total), 50);
		total += n;
		buf[total] = '\0';

		if ((strstr(buf, "OK") != NULL) || (strstr(buf, "ERROR") != NULL))
		{
			break;
		}
	}

	if (total == 0)
	{
		printf("[ESP] << (nothing)\r\n");
		return 0;
	}

	esp_print_raw(buf, total);
	return (strstr(buf, "OK") != NULL) ? 1 : 0;
}

/* 只收不印发，把收到的字节按十六进制打出来，用来判断线上到底有没有数据 */
static uint16_t esp_sniff(const char *tag, uint32_t ms)
{
	char buf[64];
	uint16_t n = esp_drain(buf, sizeof(buf), ms);

	printf("[ESP] %s raw %u:", tag, n);

	for (uint16_t i = 0; i < n; i++)
	{
		printf(" %02X", (uint8_t)buf[i]);
	}

	printf("\r\n");
	return n;
}

/* 判别"模块真回显"还是"PB10/PB11 自己短接"：
 * 发一条模块不认识的指令，正常模块会多回 ERROR，短接只会原样回来一样长 */
static void esp_probe(void)
{
	const char *cmd = "AT+XQZ\r\n";
	char buf[128];
	uint16_t n;

	esp_flush();
	printf("[ESP] probe >> AT+XQZ (11 bytes)\r\n");
	esp_send(cmd);

	n = esp_drain(buf, sizeof(buf), 1500);

	printf("[ESP] probe raw %u:", n);

	for (uint16_t i = 0; i < n; i++)
	{
		printf(" %02X", (uint8_t)buf[i]);
	}

	printf("\r\n");

	if (n == 8)
	{
		printf("[ESP] echo only -> PB10/PB11 shorted or loopback\r\n");
	}
	else if (n > 8)
	{
		printf("[ESP] module replied -> real device\r\n");
	}
	else
	{
		printf("[ESP] nothing back\r\n");
	}
}

/* ================= 下行解析 =================
 * TCP 透传时模块报 +IPD,<len>:<数据>，数据就是一个完整的 MQTT 报文，
 * 可能是 CONNACK / SUBACK / PUBLISH。这里按报文头拆，不再靠搜字符串。
 */
static char s_ipd[256];
static uint16_t s_ipd_len;

/* 解析一个 MQTT 报文，收到开锁指令返回 1 */
static uint8_t mqtt_parse(const uint8_t *p, uint16_t n)
{
	uint8_t type;

	if (n < 2)
	{
		return 0;
	}

	type = (uint8_t)(p[0] & 0xF0);

	if (type == 0x20)                                   // CONNACK
	{
		printf("[MQTT] connack rc=%u\r\n", (n >= 4) ? p[3] : 0xFF);
		return 0;
	}

	if (type == 0x90)                                   // SUBACK
	{
		printf("[MQTT] suback\r\n");
		return 0;
	}

	if (type != 0x30)                                   // 只处理 PUBLISH
	{
		printf("[MQTT] pkt type 0x%02X\r\n", p[0]);
		return 0;
	}

	/* 跳固定头：1 字节类型 + 变长剩余长度 */
	uint16_t i = 1;

	while (i < n && (p[i] & 0x80))
	{
		i++;
	}

	i++;                                                // 跳过最后一个长度字节

	if (i + 2 > n)
	{
		return 0;
	}

	uint16_t tlen = (uint16_t)((p[i] << 8) | p[i + 1]);
	i = (uint16_t)(i + 2 + tlen);                       // 跳过主题

	if (i > n)
	{
		return 0;
	}

	uint16_t plen = (uint16_t)(n - i);

	printf("[MQTT] payload %u: ", plen);

	for (uint16_t k = 0; k < plen; k++)
	{
		printf("%c", (p[i + k] >= ' ' && p[i + k] <= '~') ? p[i + k] : '.');
	}

	printf("\r\n");

	if ((plen >= 4) && (memcmp(&p[i], "open", 4) == 0))
	{
		return 1;
	}

	if ((plen >= 5) && (memcmp(&p[i], "close", 5) == 0))
	{
		return 2;
	}

	return 0;
}

/* 把新收到的字节接到流缓冲后面，再从里面切出完整的 +IPD 帧 */
static uint8_t mqtt_feed(const char *data, uint16_t n)
{
	uint8_t result = 0;

	if ((uint16_t)(s_ipd_len + n) > sizeof(s_ipd))
	{
		s_ipd_len = 0;                                  // 溢出就丢掉重来
	}

	memcpy(&s_ipd[s_ipd_len], data, n);
	s_ipd_len = (uint16_t)(s_ipd_len + n);
	s_ipd[s_ipd_len] = '\0';                            // strstr 需要结束符

	for (;;)
	{
		char *hdr = strstr(s_ipd, "+IPD,");

		if (hdr == NULL)
		{
			/* 没有帧头，只保留最后 8 字节，防止半截 "+IPD," 被丢 */
			if (s_ipd_len > 8)
			{
				memmove(s_ipd, &s_ipd[s_ipd_len - 8], 8);
				s_ipd_len = 8;
			}

			break;
		}

		/* 帧头前的垃圾丢掉 */
		uint16_t skip = (uint16_t)(hdr - s_ipd);

		if (skip > 0)
		{
			memmove(s_ipd, hdr, (uint16_t)(s_ipd_len - skip));
			s_ipd_len = (uint16_t)(s_ipd_len - skip);
			s_ipd[s_ipd_len] = '\0';
		}

		/* 解析 "+IPD,<len>:" */
		uint16_t pos = 5;
		uint32_t len = 0;

		while ((pos < s_ipd_len) && (s_ipd[pos] >= '0') && (s_ipd[pos] <= '9'))
		{
			len = len * 10 + (uint32_t)(s_ipd[pos] - '0');
			pos++;
		}

		if ((pos >= s_ipd_len) || (s_ipd[pos] != ':'))
		{
			break;                                      // 还没收全
		}

		pos++;                                          // 跳过冒号

		if ((uint32_t)s_ipd_len < (uint32_t)pos + len)
		{
			break;                                      // 数据还没到齐
		}

		uint8_t r = mqtt_parse((const uint8_t *)&s_ipd[pos], (uint16_t)len);

		if (r != 0)
		{
			result = r;
		}

		/* 这一帧处理完，剩下的往前挪 */
		uint16_t used = (uint16_t)(pos + len);
		memmove(s_ipd, &s_ipd[used], (uint16_t)(s_ipd_len - used));
		s_ipd_len = (uint16_t)(s_ipd_len - used);
		s_ipd[s_ipd_len] = '\0';
	}

	return result;
}

/* 前置声明 */
static uint8_t mqtt_publish(const char *topic, const char *payload);
static uint8_t mqtt_ping(void);
static uint8_t net_connect(void);

/* 上报状态到 lock/state */
void esp_publish_state(const char *evt)
{
	mqtt_publish(TOPIC_STATE, evt);
}

/* ================= 裸 MQTT 3.1.1 =================
 * 这个固件没有 AT+MQTTCONN，只能自己拼报文走 TCP。
 * CONNECT 0x10 / SUBSCRIBE 0x82 / PUBLISH 0x30 / PINGREQ 0xC0
 */

static char s_mqtt_buf[192];

/* 剩余长度用变长编码：每字节 7 位，最高位表示后面还有 */
static uint8_t mqtt_varint(uint8_t *out, uint32_t len)
{
	uint8_t n = 0;

	do
	{
		uint8_t b = (uint8_t)(len % 128);
		len /= 128;

		if (len > 0)
		{
			b |= 0x80;
		}

		out[n++] = b;
	}
	while (len > 0);

	return n;
}

/* 通过 AT+CIPSEND 发一段二进制，成功返回 1 */
static uint8_t mqtt_send_raw(const uint8_t *data, uint16_t len)
{
	char cmd[32];
	char buf[128];
	uint32_t start;
	uint8_t got_prompt = 0;
	uint16_t n;

	snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%u\r\n", len);
	esp_flush();
	printf("[MQTT] >> CIPSEND %u\r\n", len);
	esp_send(cmd);

	start = HAL_GetTick();

	while ((HAL_GetTick() - start) < 3000)
	{
		n = esp_drain(buf, sizeof(buf), 100);

		for (uint16_t i = 0; i < n; i++)
		{
			if (buf[i] == '>')
			{
				got_prompt = 1;
			}
		}

		if (got_prompt)
		{
			break;
		}
	}

	if (!got_prompt)
	{
		printf("[MQTT] no '>' prompt\r\n");
		return 0;
	}

	HAL_UART_Transmit(&s_uart, (uint8_t *)data, len, 1000);

	printf("[MQTT] tx:");

	for (uint16_t k = 0; k < len; k++)
	{
		printf(" %02X", data[k]);
	}

	printf("\r\n");

	start = HAL_GetTick();

	while ((HAL_GetTick() - start) < 5000)
	{
		n = esp_drain(buf, sizeof(buf), 300);

		if (n == 0)
		{
			continue;
		}

		esp_print_raw(buf, n);

		if (strstr(buf, "SEND OK") != NULL)
		{
			printf("[MQTT] sent ok\r\n");
			return 1;
		}

		if (strstr(buf, "ERROR") != NULL || strstr(buf, "SEND FAIL") != NULL || strstr(buf, "CLOSED") != NULL)
		{
			printf("[MQTT] send fail\r\n");
			return 0;
		}
	}

	printf("[MQTT] send timeout, try escape\r\n");

	/* 模块还在等那 len 个字节，后面的 AT 会被当数据吃掉。
	 * 先发 +++ 退出透传，再硬件复位模块，让它回到干净状态 */
	esp_send("+++");
	vTaskDelay(pdMS_TO_TICKS(1200));
	esp_flush();
	esp_send("AT+RST\r\n");
	vTaskDelay(pdMS_TO_TICKS(3000));
	esp_flush();

	return 0;
}

/* CONNECT：协议名 MQTT / 级别 4 / 清会话 / 保活 60s / 客户端 ID */
static uint8_t mqtt_connect(const char *cid)
{
	uint16_t cid_len = (uint16_t)strlen(cid);
	uint16_t i = 0;
	uint8_t vh[10];
	uint8_t vl;

	vh[0] = 0x00; vh[1] = 0x04;                 // 协议名长度
	vh[2] = 'M'; vh[3] = 'Q'; vh[4] = 'T'; vh[5] = 'T';
	vh[6] = 0x04;                               // 协议级别 3.1.1
	vh[7] = 0x02;                               // 标志：清会话
	vh[8] = 0x00; vh[9] = 0x3C;                 // 保活 60 秒

	uint32_t remain = 10 + 2 + cid_len;

	s_mqtt_buf[i++] = 0x10;
	vl = mqtt_varint((uint8_t *)&s_mqtt_buf[i], remain);
	i += vl;
	memcpy(&s_mqtt_buf[i], vh, 10);
	i += 10;
	s_mqtt_buf[i++] = (uint8_t)(cid_len >> 8);
	s_mqtt_buf[i++] = (uint8_t)(cid_len & 0xFF);
	memcpy(&s_mqtt_buf[i], cid, cid_len);
	i += cid_len;

	return mqtt_send_raw((const uint8_t *)s_mqtt_buf, i);
}

/* SUBSCRIBE：报文号 1，主题 + QoS0 */
static uint8_t mqtt_subscribe(const char *topic)
{
	uint16_t tlen = (uint16_t)strlen(topic);
	uint16_t i = 0;
	uint32_t remain = 2 + 2 + tlen + 1;
	uint8_t vl;

	s_mqtt_buf[i++] = 0x82;
	vl = mqtt_varint((uint8_t *)&s_mqtt_buf[i], remain);
	i += vl;
	s_mqtt_buf[i++] = 0x00;                     // 报文号高字节
	s_mqtt_buf[i++] = 0x01;                     // 报文号低字节
	s_mqtt_buf[i++] = (uint8_t)(tlen >> 8);
	s_mqtt_buf[i++] = (uint8_t)(tlen & 0xFF);
	memcpy(&s_mqtt_buf[i], topic, tlen);
	i += tlen;
	s_mqtt_buf[i++] = 0x00;                     // 请求 QoS0

	return mqtt_send_raw((const uint8_t *)s_mqtt_buf, i);
}

/* PUBLISH QoS0：主题 + 纯文本载荷 */
static uint8_t mqtt_publish(const char *topic, const char *payload)
{
	uint16_t tlen = (uint16_t)strlen(topic);
	uint16_t plen = (uint16_t)strlen(payload);
	uint16_t i = 0;
	uint32_t remain = 2 + tlen + plen;
	uint8_t vl;

	if (g_esp_state != ESP_ST_MQTT)
	{
		return 0;
	}

	s_mqtt_buf[i++] = 0x30;
	vl = mqtt_varint((uint8_t *)&s_mqtt_buf[i], remain);
	i += vl;
	s_mqtt_buf[i++] = (uint8_t)(tlen >> 8);
	s_mqtt_buf[i++] = (uint8_t)(tlen & 0xFF);
	memcpy(&s_mqtt_buf[i], topic, tlen);
	i += tlen;
	memcpy(&s_mqtt_buf[i], payload, plen);
	i += plen;

	return mqtt_send_raw((const uint8_t *)s_mqtt_buf, i);
}

/* 保活：PINGREQ 两个字节 */
static uint8_t mqtt_ping(void)
{
	static const uint8_t ping[2] = {0xC0, 0x00};
	return mqtt_send_raw(ping, 2);
}

/* 连 WiFi + 建 TCP + MQTT CONNECT + SUBSCRIBE */
static uint8_t net_connect(void)
{
	char cmd[192];

	/* 1. 连 WiFi */
	snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"\r\n", WIFI_SSID, WIFI_PASS);
	printf("[ESP] join %s ...\r\n", WIFI_SSID);

	if (!esp_at(cmd, 20000))
	{
		printf("[ESP] wifi fail, check hotspot on / 2.4G / password\r\n");
		g_esp_state = ESP_ST_AT;
		return 0;
	}

	g_esp_state = ESP_ST_WIFI;
	printf("[ESP] wifi ok\r\n");

	/* 3. 逐个 broker 试：建 TCP -> 发 CONNECT -> 订阅 */
	for (uint8_t i = 0; i < BROKER_NUM; i++)
	{
		snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",%d\r\n", s_brokers[i], MQTT_PORT);

		if (!esp_at(cmd, 8000))
		{
			printf("[ESP] %s tcp fail\r\n", s_brokers[i]);
			esp_at("AT+CIPCLOSE\r\n", 1000);
			continue;
		}

		if (mqtt_connect("stm32lock01") && mqtt_subscribe(TOPIC_CMD))
		{
			printf("[ESP] mqtt ready @ %s\r\n", s_brokers[i]);
			g_esp_state = ESP_ST_MQTT;
			return 1;
		}

		printf("[ESP] %s rejected, try next\r\n", s_brokers[i]);
		esp_at("AT+CIPCLOSE\r\n", 1000);
		vTaskDelay(pdMS_TO_TICKS(500));
	}

	printf("[ESP] all brokers failed\r\n");
	g_esp_state = ESP_ST_WIFI;
	return 0;
}

static void vEspTask(void *pvParameters)
{
	(void)pvParameters;
	uint8_t tries;
	char buf[256];
	uint16_t n;
	uint32_t last_ping = 0;

	esp_uart_config();                           // 先在这初始化，避免和别的任务抢
	vTaskDelay(pdMS_TO_TICKS(1000));             // 等模块上电启动完

	printf("[ESP] start, UART3 115200\r\n");

	/* 先看线上有没有字节：ESP 上电会吐启动信息，哪怕波特率不对也该看到乱码 */
	if (esp_sniff("boot", 800) == 0)
	{
		printf("[ESP] no bytes: check 3V3 / EN / IO0 / TX-RX\r\n");
	}

	esp_probe();                                 // 先判别是真模块还是自环

	for (tries = 0; tries < 3; tries++)          // AT 多发几次，模块可能还在启动
	{
		if (esp_at("AT\r\n", 1500))
		{
			g_esp_state = ESP_ST_AT;
			break;
		}

		vTaskDelay(pdMS_TO_TICKS(300));
	}

	if (g_esp_state != ESP_ST_AT)
	{
		printf("[ESP] AT no reply at 115200\r\n");
		g_esp_state = ESP_ST_OFF;
		goto idle;
	}

	printf("[ESP] at ok\r\n");
	esp_at("ATE0\r\n", 1000);                    // 关回显，避免 1 字节缓冲被回显冲爆
	esp_at("AT+GMR\r\n", 2000);                  // 固件版本

	/* 热点可能没开，连不上就每 10 秒重试，不用重启板子 */
	while (!net_connect())
	{
		printf("[ESP] retry in 10s\r\n");
		vTaskDelay(pdMS_TO_TICKS(10000));
	}

	for (;;)
	{
		/* 4. 收 MQTT 下行：交给 mqtt_feed 按 +IPD 帧和 MQTT 报文头解析 */
		n = esp_drain(buf, sizeof(buf), 300);

		if (n > 0)
		{
			uint8_t cmd_r = mqtt_feed(buf, n);

			if (cmd_r == 1)
			{
				printf("[ESP] remote open\r\n");
				lock_request(SRC_NET);
				esp_publish_state("open");
			}
			else if (cmd_r == 2)
			{
				printf("[ESP] remote close\r\n");
				lock_close();
				esp_publish_state("close");
			}
		}

		/* 5. 每 30 秒发一次 MQTT 保活 */
		if ((HAL_GetTick() - last_ping) > 30000)
		{
			last_ping = HAL_GetTick();

			if (!mqtt_ping())
			{
				printf("[ESP] ping fail, reconnect\r\n");
				g_esp_state = ESP_ST_AT;
				esp_at("AT+CIPCLOSE\r\n", 2000);

				while (!net_connect())
				{
					printf("[ESP] retry in 10s\r\n");
					vTaskDelay(pdMS_TO_TICKS(10000));
				}
			}
		}

		vTaskDelay(pdMS_TO_TICKS(200));
	}

idle:
	for (;;)
	{
		vTaskDelay(pdMS_TO_TICKS(5000));
	}
}

void esp_start(void)
{
	g_esp_state = ESP_ST_OFF;
	xTaskCreate(vEspTask, "ESP", 384, NULL, 2, NULL);
}
