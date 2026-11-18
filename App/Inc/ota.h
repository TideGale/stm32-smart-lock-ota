#ifndef __OTA_H
#define __OTA_H

#include "main.h"
#include "ota_meta.h"

#define OTA_STAGE_ADDR   0x000000U          /* W25Q128 升级包暂存区起始地址 */
#define OTA_FW_SIZE      OTA_SLOT_SIZE      /* 整槽复制，96KB */

/* 把当前正在运行的镜像整槽复制到 W25Q 暂存区（相当于"下载完成"） */
int  ota_prepare_from_running(void);

/* 暂存区 -> 非活动槽，CRC 校验通过后切槽、置试启动，返回 0 成功（不自动复位） */
int  ota_apply(void);

/* 串口发 'P' 准备升级包，发 'A' 执行升级并复位；另建一个任务收这两个键 */
void ota_task_start(void);

#endif
