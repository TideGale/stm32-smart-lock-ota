#ifndef __LOCK_H
#define __LOCK_H

#include "main.h"

#define SRC_PASS    1
#define SRC_FP      2
#define SRC_NET     3

#define LOCK_STATE_CLOSED   0
#define LOCK_STATE_OPEN     1

void lock_start(void);
void lock_open(uint32_t ms);     // 开锁，ms 后自动关
void lock_close(void);
uint8_t lock_is_open(void);
void lock_request(uint8_t src);  // 带互斥的开锁入口，指纹/密码/远程都走这里
void lock_selftest(void);        // 接线自检：继电器吸合 1 秒，不经过密码/指纹

uint8_t  lock_get_state(void);      // LOCK_STATE_CLOSED / OPEN
uint8_t  lock_get_last_src(void);   // 最近一次开锁来源
uint32_t lock_get_open_count(void); // 累计开锁次数
uint16_t lock_get_remain_s(void);   // 开锁剩余秒数，0 表示已关

#endif
