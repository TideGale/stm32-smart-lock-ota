#ifndef __AS608_H
#define __AS608_H

#include "main.h"

#define AS608_OK         0x00
#define AS608_NO_FINGER  0x02
#define AS608_NO_MATCH   0x08
#define AS608_NOT_FOUND  0x09

uint8_t as608_init(void);            // 自己配 UART4（PC10=TX，PC11=RX），返回确认码
uint8_t as608_selftest(void);        // 单字节回环自测：PC10 与 PC11 短接时应通过
uint8_t as608_count(uint16_t *cnt);  // 读指纹库模板数量
uint8_t as608_search(uint16_t *page, uint16_t *score);
uint8_t as608_enroll(uint16_t page);
uint8_t as608_delete(uint16_t page, uint16_t count);

#endif
