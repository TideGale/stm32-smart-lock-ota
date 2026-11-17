#ifndef __APP_FP_H
#define __APP_FP_H

#include "main.h"

/* 匹配分数下限：低于这个值视为不可靠，拒绝开锁
 * AS608 的 score 范围 0~1999，同一手指正常 200 以上，勉强匹配 50~100 */
#define FP_MIN_SCORE    100

/* 给 OLED 显示的状态 */
#define FP_ST_IDLE      0   // 待机
#define FP_ST_MATCH     1   // 匹配成功
#define FP_ST_NOMATCH   2   // 库中无匹配
#define FP_ST_LOWSCORE  3   // 匹配到但分数过低
#define FP_ST_OFFLINE   4   // 模块没应答

extern volatile uint8_t  g_fp_state;
extern volatile uint16_t g_fp_score;
extern volatile uint16_t g_fp_page;

void app_fp_start(void);

#endif
