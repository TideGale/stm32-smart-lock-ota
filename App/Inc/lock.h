#ifndef __LOCK_H
#define __LOCK_H

#include "main.h"

void lock_start(void);
void lock_open(uint32_t ms);     // 开锁，ms 后自动关
void lock_close(void);
uint8_t lock_is_open(void);

#endif
