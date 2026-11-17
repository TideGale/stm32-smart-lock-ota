#include "keypad.h"

#define KEYPAD_ROW_NUM  4
#define KEYPAD_COL_NUM  4

/* 实测接线（HW-834 排针从下往上 C4..C1，你的接法让行列都是反的）
 * 代码行 0 = 物理最上面一行 R1 = PA1
 * 代码列 0 = 物理最左一列 = PB4（原 PB10，让给 USART3 给 ESP8266 用）
 */
static GPIO_TypeDef *const s_row_port[KEYPAD_ROW_NUM] =
{
	GPIOA, GPIOB, GPIOB, GPIOB
};

static const uint16_t s_row_pins[KEYPAD_ROW_NUM] =
{
	GPIO_PIN_1, GPIO_PIN_3, GPIO_PIN_1, GPIO_PIN_0
};

static GPIO_TypeDef *const s_col_port[KEYPAD_COL_NUM] =
{
	GPIOB, GPIOB, GPIOA, GPIOB
};

static const uint16_t s_col_pins[KEYPAD_COL_NUM] =
{
	GPIO_PIN_4, GPIO_PIN_9, GPIO_PIN_2, GPIO_PIN_5
};

static const char s_keymap[KEYPAD_ROW_NUM][KEYPAD_COL_NUM] =
{
	{'1', '2', '3', 'A'},
	{'4', '5', '6', 'B'},
	{'7', '8', '9', 'C'},
	{'*', '0', '#', 'D'}
};

static char s_last_raw;          // 上一次原始扫描
static char s_stable;            // 已确认的键
static uint8_t s_count;          // 连续相同次数
static uint8_t s_last_r;         // 最近一次按键的行号
static uint8_t s_last_c;         // 最近一次按键的列号

void keypad_last_rc(uint8_t *row, uint8_t *col)
{
	*row = s_last_r;
	*col = s_last_c;
}

void keypad_init(void)
{
	for (uint8_t i = 0; i < KEYPAD_ROW_NUM; i++)
	{
		HAL_GPIO_WritePin(s_row_port[i], s_row_pins[i], GPIO_PIN_SET);   // 行默认高
	}
}

static char keypad_read_raw(void)
{
	for (uint8_t r = 0; r < KEYPAD_ROW_NUM; r++)
	{
		HAL_GPIO_WritePin(s_row_port[r], s_row_pins[r], GPIO_PIN_RESET); // 当前行拉低
		HAL_Delay(1);                                                    // 等电平稳定

		for (uint8_t c = 0; c < KEYPAD_COL_NUM; c++)
		{
			if (HAL_GPIO_ReadPin(s_col_port[c], s_col_pins[c]) == GPIO_PIN_RESET)
			{
				HAL_GPIO_WritePin(s_row_port[r], s_row_pins[r], GPIO_PIN_SET);
				s_last_r = r;                                            // 记下实际行列
				s_last_c = c;
				return s_keymap[r][c];                                   // 找到键
			}
		}

		HAL_GPIO_WritePin(s_row_port[r], s_row_pins[r], GPIO_PIN_SET);   // 恢复高
	}

	return 0;                                                            // 没有键
}

char keypad_scan(void)
{
	char raw = keypad_read_raw();

	if (raw != s_last_raw)                  // 变了，重新计数
	{
		s_last_raw = raw;
		s_count = 0;
		return 0;
	}

	if (s_count < 2)                        // 连续 3 次（30ms）
	{
		s_count++;
	}

	if (s_count == 2 && raw != s_stable)    // 稳定后只报一次
	{
		s_stable = raw;

		if (raw != 0)
		{
			return raw;                     // 按下沿
		}
	}

	return 0;
}

