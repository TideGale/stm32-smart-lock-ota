#include "ota.h"
#include "w25q.h"
#include "crc32.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

#define OTA_CHUNK        256                        /* 一次搬运 256 字节 */
#define OTA_PAGE_SIZE    2048                       /* F1 大容量型号一页 2KB */
#define OTA_SECTOR_SIZE  4096                       /* W25Q128 一个扇区 4KB */

static uint8_t s_buf[OTA_CHUNK];

/* 当前运行的槽号 */
static uint8_t ota_running_slot(void)
{
	return (SCB->VTOR == OTA_SLOT_A_ADDR) ? OTA_SLOT_A : OTA_SLOT_B;
}

/* 把当前镜像整槽复制到 W25Q 暂存区，模拟"升级包已下载到外部 Flash" */
int ota_prepare_from_running(void)
{
	uint32_t src = (SCB->VTOR == OTA_SLOT_A_ADDR) ? OTA_SLOT_A_ADDR : OTA_SLOT_B_ADDR;
	uint32_t off;

	printf("[OTA] prepare: slot=%s -> W25Q, %u KB\r\n",
	       (src == OTA_SLOT_A_ADDR) ? "A" : "B", (unsigned)(OTA_FW_SIZE / 1024));

	for (off = 0; off < OTA_FW_SIZE; off += OTA_SECTOR_SIZE)
	{
		w25q_erase_sector(OTA_STAGE_ADDR + off);
	}

	for (off = 0; off < OTA_FW_SIZE; off += OTA_CHUNK)
	{
		memcpy(s_buf, (const void *)(src + off), OTA_CHUNK);   /* 内部 Flash 可直接当内存读 */
		w25q_write_blk(OTA_STAGE_ADDR + off, s_buf, OTA_CHUNK);
	}

	/* 回读比对，确认搬运无误；偶尔读错就重读，三次都对不上才算真失败 */
	for (off = 0; off < OTA_FW_SIZE; off += OTA_CHUNK)
	{
		uint8_t retry;

		for (retry = 0; retry < 3; retry++)
		{
			w25q_read_blk(OTA_STAGE_ADDR + off, s_buf, OTA_CHUNK);

			if (memcmp(s_buf, (const void *)(src + off), OTA_CHUNK) == 0)
			{
				break;
			}
		}

		if (retry == 3)                                          /* 三次都错，说明这一块真的没写进去 */
		{
			uint16_t k;

			for (k = 0; k < OTA_CHUNK; k++)                      /* 找第一个不一样的字节 */
			{
				if (s_buf[k] != ((const uint8_t *)(src + off))[k])
				{
					break;
				}
			}

			printf("[OTA] prepare verify fail at %lu+%u: w25q=0x%02X flash=0x%02X\r\n",
			       (unsigned long)off, k, s_buf[k], ((const uint8_t *)(src + off))[k]);
			return -1;
		}
	}

	printf("[OTA] prepare ok, crc=0x%08X\r\n", crc32_calc((const uint8_t *)src, OTA_FW_SIZE));
	return 0;
}

/* 暂存区 -> 非活动槽；成功后改元数据，下次上电 Bootloader 会启动新槽 */
int ota_apply(void)
{
	ota_meta_t meta;
	uint8_t target;
	uint32_t addr;
	uint32_t off;
	uint32_t crc;
	FLASH_EraseInitTypeDef e;
	uint32_t err = 0;

	ota_meta_load(&meta);

	if (!ota_meta_is_valid(&meta))
	{
		printf("[OTA] meta invalid, refuse\r\n");
		return -1;
	}

	target = (ota_running_slot() == OTA_SLOT_A) ? OTA_SLOT_B : OTA_SLOT_A;
	addr   = ota_slot_addr(target);

	printf("[OTA] apply: slot=%s -> slot=%s, %u KB\r\n",
	       (ota_running_slot() == OTA_SLOT_A) ? "A" : "B",
	       (target == OTA_SLOT_A) ? "A" : "B",
	       (unsigned)(OTA_FW_SIZE / 1024));

	/* 1. 擦目标槽 */
	HAL_FLASH_Unlock();
	__HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPERR);

	e.TypeErase   = FLASH_TYPEERASE_PAGES;
	e.PageAddress = addr;
	e.NbPages     = OTA_FW_SIZE / OTA_PAGE_SIZE;

	if (HAL_FLASHEx_Erase(&e, &err) != HAL_OK)
	{
		HAL_FLASH_Lock();
		printf("[OTA] erase fail, err page=%lu\r\n", (unsigned long)err);
		return -2;
	}

	/* 2. 从 W25Q 搬过去，按字写 */
	for (off = 0; off < OTA_FW_SIZE; off += OTA_CHUNK)
	{
		w25q_read_blk(OTA_STAGE_ADDR + off, s_buf, OTA_CHUNK);

		for (uint16_t i = 0; i < OTA_CHUNK; i += 4)
		{
			uint32_t w = (uint32_t)s_buf[i]
			           | ((uint32_t)s_buf[i + 1] << 8)
			           | ((uint32_t)s_buf[i + 2] << 16)
			           | ((uint32_t)s_buf[i + 3] << 24);

			if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + off + i, w) != HAL_OK)
			{
				HAL_FLASH_Lock();
				printf("[OTA] write fail at %lu\r\n", (unsigned long)(off + i));
				return -3;
			}
		}
	}

	HAL_FLASH_Lock();

	/* 3. CRC 校验：新写入的槽和暂存区必须一致，否则不切槽 */
	{
		uint32_t src_addr  = (SCB->VTOR == OTA_SLOT_A_ADDR) ? OTA_SLOT_A_ADDR : OTA_SLOT_B_ADDR;
		uint32_t stage_crc = crc32_calc((const uint8_t *)src_addr, OTA_FW_SIZE);

		crc = crc32_flash(addr, OTA_FW_SIZE);
		printf("[OTA] crc slot=0x%08X stage=0x%08X\r\n", crc, stage_crc);

		if (crc != stage_crc)
		{
			uint32_t k, bad = 0;

			for (k = 0; k < OTA_FW_SIZE; k++)                    /* 找出第一个不一样的字节，判断是零星坏还是一片坏 */
			{
				if (*(const uint8_t *)(addr + k) != *(const uint8_t *)(src_addr + k))
				{
					if (bad < 8)                                 /* 最多打 8 个，够判断了 */
					{
						printf("[OTA] diff @%lu new=0x%02X old=0x%02X\r\n",
						       (unsigned long)k,
						       *(const uint8_t *)(addr + k),
						       *(const uint8_t *)(src_addr + k));
						bad++;
					}
				}
			}

			printf("[OTA] crc mismatch, %lu bytes differ, abort\r\n", (unsigned long)bad);
			return -4;
		}
	}

	/* 4. 更新元数据：切槽 + 试启动 + 计数清零 */
	if (target == OTA_SLOT_A)
	{
		meta.crc_a = crc;
	}
	else
	{
		meta.crc_b = crc;
	}

	meta.active_slot = target;
	meta.trial       = 1;                       /* 新固件要试启动，跑不到 main 会被回滚 */
	meta.boot_count  = 0;
	meta.ota_req     = 0;
	ota_meta_save(&meta);

	printf("[OTA] meta saved: slot=%u trial=1 crc=0x%08X\r\n", meta.active_slot, crc);
	printf("[OTA] done, reboot to take effect\r\n");
	return 0;
}

static void vOtaTask(void *pvParameters)
{
	(void)pvParameters;
	uint8_t ch;

	for (;;)
	{
		/* 轮询调试串口 USART1 的接收，不用中断，简单可靠 */
		if (USART1->SR & USART_SR_RXNE)
		{
			ch = (uint8_t)(USART1->DR & 0xFF);

			if (ch == 'P' || ch == 'p')
			{
				ota_prepare_from_running();
			}
			else if (ch == 'R' || ch == 'r')                 /* 调试用：把活动槽强制设回 A，方便反复做 A->B->A */
			{
				ota_meta_t m;

				ota_meta_load(&m);

				if (!ota_meta_is_valid(&m))
				{
					ota_meta_set_default(&m);
				}

				m.crc_a       = crc32_flash(OTA_SLOT_A_ADDR, OTA_FW_SIZE);
				m.crc_b       = crc32_flash(OTA_SLOT_B_ADDR, OTA_FW_SIZE);
				m.active_slot = OTA_SLOT_A;
				m.trial       = 0;
				m.boot_count  = 0;
				m.ota_req     = 0;
				ota_meta_save(&m);

				printf("[OTA] meta reset: slot=A crc_a=0x%08X crc_b=0x%08X\r\n", m.crc_a, m.crc_b);
				vTaskDelay(pdMS_TO_TICKS(200));
				NVIC_SystemReset();
			}
			else if (ch == 'B' || ch == 'b')                 /* 调试用：活动槽切到 B，验证 B 槽镜像能独立运行 */
			{
				ota_meta_t m;

				ota_meta_load(&m);

				if (!ota_meta_is_valid(&m))
				{
					ota_meta_set_default(&m);
				}

				m.crc_a       = crc32_flash(OTA_SLOT_A_ADDR, OTA_FW_SIZE);
				m.crc_b       = crc32_flash(OTA_SLOT_B_ADDR, OTA_FW_SIZE);
				m.active_slot = OTA_SLOT_B;
				m.trial       = 1;
				m.boot_count  = 0;
				m.ota_req     = 0;
				ota_meta_save(&m);

				printf("[OTA] meta set: slot=B crc_a=0x%08X crc_b=0x%08X\r\n", m.crc_a, m.crc_b);
				vTaskDelay(pdMS_TO_TICKS(200));
				NVIC_SystemReset();
			}
			else if (ch == 'A' || ch == 'a')
			{
				if (ota_apply() == 0)
				{
					vTaskDelay(pdMS_TO_TICKS(300));   /* 让最后几行日志发完 */
					NVIC_SystemReset();
				}
			}
		}

		vTaskDelay(pdMS_TO_TICKS(20));
	}
}

void ota_task_start(void)
{
	if (xTaskCreate(vOtaTask, "OTA", 512, NULL, 2, NULL) != pdPASS)
	{
		printf("[OTA] task create fail, heap not enough\r\n");
		return;
	}

	printf("[OTA] cmd: 'P' prepare, 'A' apply+reboot, 'R' slotA, 'B' slotB\r\n");
}
