
#ifndef lcd_st75161_H
#define lcd_st75161_H

#include "freertos/FreeRTOS.h"

/* LCD规格 */
#define LCD_WIDTH				160
#define LCD_HEIGHT				160
#define LINE_LENGTH				(LCD_WIDTH / 8)

#define GAMMA_MIN					200
#define GAMMA_MAX					360
#define DEFAULT_GAMMA				320

/* 液晶对比度等级 */
enum
{
	LCD_GAMMA_LEVEL_0		=	0,
	LCD_GAMMA_LEVEL_1,
	LCD_GAMMA_LEVEL_2,
	LCD_GAMMA_LEVEL_3,
	LCD_GAMMA_LEVEL_4,
	LCD_GAMMA_LEVEL_5,
	LCD_GAMMA_LEVEL_6,
	LCD_GAMMA_LEVEL_7,
	LCD_GAMMA_LEVEL_8,
	LCD_GAMMA_LEVEL_9,
	LCD_GAMMA_LEVEL_MAX,	
};

#define LCD_INIT_DEFAULT_GAMMA_LEVEL			(LCD_GAMMA_LEVEL_7)

bool lcd_set_gamma(uint32_t contrast);
void lcd_flush(int col_s, int col_e, int page_s, int page_e, uint8_t *buf);
void lcd_clear(uint8_t val);
int lcd_spiiokey_get();
void lcd_init(void);
void lcd_BL(uint8_t state);

// int linuxfb_flush(const uint8_t *data, int xs, int ys, int xe, int ye, size_t data_size);


#endif


