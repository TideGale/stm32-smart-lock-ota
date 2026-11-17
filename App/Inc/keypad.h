#ifndef __KEYPAD_H
#define __KEYPAD_H

#include "main.h"

void keypad_init(void);
char keypad_scan(void);          // 返回 0 表示没有新按键
void keypad_last_rc(uint8_t *row, uint8_t *col);   // 最近一次按键的实际行列号

#endif

