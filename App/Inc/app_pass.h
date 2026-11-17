#ifndef __APP_PASS_H
#define __APP_PASS_H

#include "main.h"

#define PASS_LEN        4
#define PASS_BUF_MAX    PASS_LEN        /* 最多就能输这么多位，多了不再收 */
#define PASS_CODE       "1234"
#define PASS_MAX_FAIL   3

/* 测试时改成 10000（10 秒），验完改回 5 分钟 */
#define PASS_LOCK_MS    (5 * 60 * 1000)

/* 给 OLED 读的状态 */
extern volatile uint8_t  g_pass_len;        // 已输入位数
extern volatile uint8_t  g_pass_locked;     // 是否处于锁定期
extern volatile uint16_t g_pass_remain_s;   // 锁定剩余秒数

void app_pass_start(void);

#endif
