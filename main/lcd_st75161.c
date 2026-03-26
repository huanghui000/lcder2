#include "lcd_st75161.h"

#include <string.h>

#include "esp8266/gpio_struct.h"
#include "esp8266/spi_struct.h"
#include "esp8266/pin_mux_register.h"

#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/spi.h"

/* LCD GPIO */
#define LCD_PIN_RESET			4
#define LCD_PIN_CD				16		// L-cmd, H-data
#define LCD_PIN_CSN				2
#define LCD_PIN_BL				5
#define LCD_PIN_PIN_SEL			((1ULL<<LCD_PIN_RESET) | (1ULL<<LCD_PIN_CD) | (1ULL<<LCD_PIN_CSN) | (1ULL<<LCD_PIN_BL))
#define LCD_SPIDEV				HSPI_HOST

#define LCD_RST(lvl)			gpio_set_level(LCD_PIN_RESET, (lvl))
#define LCD_CD(lvl)				gpio_set_level(LCD_PIN_CD, (lvl))
#define LCD_CSN(lvl)			gpio_set_level(LCD_PIN_CSN, (lvl))
#define LCD_BL(state)			gpio_set_level(LCD_PIN_BL, (state))

/* LCD Buffer */
static uint8_t lcd_buf[LCD_HEIGHT * LCD_WIDTH / 8];

/* LCD delay */
#define LCD_DELAY_US(us)		os_delay_us(us)
#define LCD_DELAY_MS(ms)		os_delay_us((ms)*1000)

/*
 * gamma is used to control contrast in this driver, actually this will change
 * the VOP register, based on the requirements this value is between 55H and
 * 168H.
 */

#define LINES_PER_PAGE						8

#if 1
#define ST75161_CMD(cmd)					(cmd)
#define ST75161_DATA(data)					(data)

#define ST75161_EXTENSION_CMD1				ST75161_CMD(0x30)
#define ST75161_EXTENSION_CMD2				ST75161_CMD(0x31)
#define ST75161_EXTENSION_CMD3				ST75161_CMD(0x80)
#define ST75161_EXTENSION_CMD4				ST75161_CMD(0x81)

/* EXTENSION COMMAND 1 */
#define ST75161_DISPLAY_ON					ST75161_CMD(0xAF)
#define ST75161_DISPLAY_OFF					ST75161_CMD(0xAE)
#define ST75161_NORMAL_DISPLAY				ST75161_CMD(0xA6)
#define ST75161_INVERSE_DISPLAY				ST75161_CMD(0xA7)
#define ST75161_ALL_PIXEL_ON				ST75161_CMD(0x23)
#define ST75161_ALL_PIXEL_OFF				ST75161_CMD(0x22)
#define ST75161_DISPLAY_CONTROL(cld, dt, lf, fi)	\
	ST75161_CMD(0xCA),						\
	ST75161_DATA(((cld) & 0x1) << 2),		\
	ST75161_DATA((dt) & 0xFF),				\
	ST75161_DATA(((lf) & 0x2F) | (((fi) & 0x1) << 4))
#define ST75161_SLEEP_IN					ST75161_CMD(0x95)
#define ST75161_SLEEP_OUT					ST75161_CMD(0x94)
#define ST75161_SET_PAGE_ADDRESS(start, end)	\
	ST75161_CMD(0x75),						\
	ST75161_DATA((start) & 0xFF),			\
	ST75161_DATA((end) & 0xFF)
#define ST75161_SET_COL_ADDRESS(start, end)	\
	ST75161_CMD(0x15),						\
	ST75161_DATA((start) & 0xFF),			\
	ST75161_DATA((end) & 0xFF)
#define ST75161_DATA_SCAN_DIR(mv, mx)		\
	ST75161_CMD(0xBC),						\
	ST75161_DATA((((mv) & 0x1) << 2) | (((mx) & 0x1) << 1))
#define ST75161_WRITE_DATA					ST75161_CMD(0x5C)
#define ST75161_PARTIAL_IN(start, end)		\
	ST75161_CMD(0xA8),						\
	ST75161_DATA((start) & 0xFF),			\
	ST75161_DATA((end) & 0xFF)
#define ST75161_PARTIAL_OUT					ST75161_CMD(0xA9)
#define ST75161_SCROLL_AREA(tl, bl, nsl, scm)	\
	ST75161_CMD(0xAA),						\
	ST75161_DATA((tl)  & 0xFF),				\
	ST75161_DATA((bl)  & 0xFF),				\
	ST75161_DATA((nsl) & 0xFF),				\
	ST75161_DATA((scm) & 0x03)	
#define ST75161_SET_SCROLL_START_LINE(sl)	\
	ST75161_CMD(0xAB),						\
	ST75161_DATA((sl) & 0xFF)
#define ST75161_OSC_ON						ST75161_CMD(0xD1)
#define ST75161_OSC_OFF						ST75161_CMD(0xD2)
#define ST75161_POWER_CONTROL(vb, vf, vr)	\
	ST75161_CMD(0x20),						\
	ST75161_DATA((((vb) & 0x1) << 3) |		\
				 (((vf) & 0x1) << 1) |		\
				 (((vr) & 0x1) << 0))
#define ST75161_SET_VOP(vop)				\
	ST75161_CMD(0x81),						\
	ST75161_DATA((vop) & 0x3F),				\
	ST75161_DATA(((vop) >> 6) & 0x7)
#define ST75161_VOP_INCREASE				ST75161_CMD(0xD6)
#define ST75161_VOP_DECREASE				ST75161_CMD(0xD7)
#define ST75161_DATA_LSB_ON_BOTTOM			ST75161_CMD(0x08)
#define ST75161_DATA_LSB_ON_TOP				ST75161_CMD(0x0C)
#define ST75161_DISPLAY_MODE(dm)			\
	ST75161_CMD(0xF0),						\
	ST75161_DATA(0x10 | ((dm) & 0x1))
#define ST75161_ICON_ENABLE					ST75161_CMD(0x77)
#define ST75161_ICON_DISABLE				ST75161_CMD(0x76)

/* EXTENSION COMMAND 2 */
#define ST75161_SET_GRAY_LEVEL(light, dark)	\
	ST75161_CMD(0x20),						\
	ST75161_DATA(0x00),						\
	ST75161_DATA(0x00),						\
	ST75161_DATA(0x00),						\
	ST75161_DATA((light) & 0x1F),			\
	ST75161_DATA((light) & 0x1F),			\
	ST75161_DATA((light) & 0x1F),			\
	ST75161_DATA(0x00),						\
	ST75161_DATA(0x00),						\
	ST75161_DATA((dark) & 0x1F),			\
	ST75161_DATA(0x00),						\
	ST75161_DATA(0x00),						\
	ST75161_DATA((dark) & 0x1F),			\
	ST75161_DATA((dark) & 0x1F),			\
	ST75161_DATA((dark) & 0x1F),			\
	ST75161_DATA(0x00),						\
	ST75161_DATA(0x00)
#define ST75161_ANALOG_CIRCUIT_SET(be, bs)	\
	ST75161_CMD(0x32),						\
	ST75161_DATA(0x00),						\
	ST75161_DATA((be) & 0x3),				\
	ST75161_DATA((bs) & 0x7)
#define ST75161_BOOTSTER_LEVLE(bst)			\
	ST75161_CMD(0x51),						\
	ST75161_DATA(0xFA | ((bst) & 0x1))
#define ST75161_DRIVING_SELECT_INTERNAL		ST75161_CMD(0x40)
#define ST75161_DRIVING_SELECT_EXTERNAL		ST75161_CMD(0x41)
#define ST75161_AUTO_READ_CONTROL(xrad)		\
	ST75161_CMD(0xD7),						\
	ST75161_DATA(0x8F | (((xrad) & 0x1) << 4))
#define ST75161_OTP_WR_RD_CONTROL(wr_rd)	\
	ST75161_CMD(0xE0),						\
	ST75161_DATA(((wr_rd) & 0x1) << 5)
#define ST75161_OTP_CONTROL_OUT				ST75161_CMD(0xE1)
#define ST75161_OTP_WRITE					ST75161_CMD(0xE2)
#define ST75161_OTP_READ					ST75161_CMD(0xE3)
#define ST75161_OTP_SELECTION_CONTROL(ctrl)	\
	ST75161_CMD(0xE4),						\
	ST75161_DATA(0x99 | (((ctrl) & 0x1) << 6))
#define ST75161_OTP_PROGRAMMING_SETTING		\
	ST75161_CMD(0xE5),						\
	ST75161_DATA(0x0F)
#define ST75161_FRAME_RATE(a, b, c, d)		\
	ST75161_CMD(0xF0),						\
	ST75161_DATA((a) & 0x1F),				\
	ST75161_DATA((b) & 0x1F),				\
	ST75161_DATA((c) & 0x1F),				\
	ST75161_DATA((d) & 0x1F)
#define ST75161_TEMP_RANGE(a, b, c)			\
	ST75161_CMD(0xF2),						\
	ST75161_DATA((a) & 0x7F),				\
	ST75161_DATA((b) & 0x7F),				\
	ST75161_DATA((c) & 0x7F)
#define ST75161_TEMP_GRADIENT(a, b, c, d, e, f, g, h)	\
	ST75161_CMD(0xF4),						\
	ST75161_DATA((a) & 0xff),				\
	ST75161_DATA((b) & 0xff),				\
	ST75161_DATA((c) & 0xff),				\
	ST75161_DATA((d) & 0xff),				\
	ST75161_DATA((e) & 0xff),				\
	ST75161_DATA((f) & 0xff),				\
	ST75161_DATA((g) & 0xff),				\
	ST75161_DATA((h) & 0xff)	

/* EXTENSION COMMAND 3 */
#define ST75161_READ_ID_ENABLE				ST75161_CMD(0x7F)
#define ST75161_READ_ID_DISABLE				ST75161_CMD(0x7E)

/* EXTENSION COMMAND 4 */
#define ST75161_ENABLE_OTP(eotp)			\
	ST75161_CMD(0xD6),						\
	ST75161_DATA(((eotp) & 0x1) << 4)
#endif

static const int default_init_sequence[] =
{
	-1, ST75161_EXTENSION_CMD2,
	-1, ST75161_AUTO_READ_CONTROL(1),				// OTP auto-read	: 0-enable, 1-disable
	-1, ST75161_OTP_WR_RD_CONTROL(0),				// OTP w/r control	: 0-read, 1-write
	-1, ST75161_OTP_READ,							// OTP read
	-1, ST75161_OTP_CONTROL_OUT,					// OTP control out

	-1, ST75161_EXTENSION_CMD1,
	-1, ST75161_SLEEP_OUT,							// Exit sleep, 0x94-Sleep in(POR default), 0x95-Sleep out
	-1, ST75161_POWER_CONTROL(1, 1, 1),				// Power control : VB-booster, VR-regulator, VF-follower (1-on, 0-off)
	-1, ST75161_SET_VOP(DEFAULT_GAMMA),				// Setting V0 = 3.6 + VOP[8:0]*0.04 (304 -> 15.76V)

	-1, ST75161_EXTENSION_CMD2,
	-1, ST75161_SET_GRAY_LEVEL(0x17, 0x1D),			// Set gray level - useless in mono mode (0x17, 0x1D - ???)
	-1, ST75161_ANALOG_CIRCUIT_SET(0x01, 0x02),		// Setting analog circuit (boost efficiency & lcd bias ratio) to default
	-1, ST75161_BOOTSTER_LEVLE(0),					// Setting booster level factor : 0-X8, 1-X10(default)

	-1, ST75161_EXTENSION_CMD1,
	-1, ST75161_DISPLAY_MODE(0),					// Setting display mode to : 0-Mono(default), 1-4Gray Scale
	-1, ST75161_DATA_LSB_ON_TOP,					// Setting Data format : 0x08-LSB bottom(default), 0x0C-LSB top
	-1, ST75161_DISPLAY_CONTROL(0, 0x9F, 0, 0),		// Display Control (0x9F - 160 lines)
	-1, ST75161_DATA_SCAN_DIR(1, 0),				// Data scan direction : MV (1-vertical, 0-horizontal) & MX (0-left, 1-right)
	-1, ST75161_NORMAL_DISPLAY,						// Inverse display : 0xA6-Normal, 0xA7-Inverse

	-1, ST75161_EXTENSION_CMD2,
	-1, ST75161_DRIVING_SELECT_INTERNAL,			// Setting power type : 0x40-internal(default), 0x41-external
	-1, ST75161_EXTENSION_CMD2,
	-1, ST75161_FRAME_RATE(0x06, 0x0C, 0x0C, 0x0C),	// Frame rate : 46Hz 69Hz 69Hz 69Hz
	-1, ST75161_TEMP_RANGE(0x05, 0x23, 0x40),		// Temp range TA-(-35deg), TB-(-5deg), TC-24deg
	-1, ST75161_TEMP_GRADIENT(0xFF, 0x0F, 0x00, 0x00, 0x00, 0x00, 0xAF, 0xFF),

	-1, ST75161_EXTENSION_CMD1,
	-1, ST75161_DISPLAY_ON,							// Display ON/OFF : 0xAE-OFF 0xAF-ON

	-3, /* END */
};

// #include <pthread.h>
// extern pthread_mutex_t *pSpi2Lock;
uint32_t spibuf[16];	// SPI trans max bytes - 64 (16 32-bit words)
						// In order to improve the transmission efficiency,
						// it is recommended that the external incoming data
						// is (uint32_t *) type data, do not use other type data.
/* write接口封装 */
static void spi_write_bytes(uint8_t *pDat, int len)
{
	// pthread_mutex_lock(pSpi2Lock);
	int pktLen;
	uint8_t *pByte = pDat;
	spi_trans_t trans = {0};

	// Transmit 64-bytes packet
	while (len > 0)
	{
		pktLen = (len>=64) ? 64 : len;

		for (int i = 0; i < (pktLen+3) / 4; i++)
		{
			spibuf[i] = pByte[3] << 24 | pByte[2] << 16 | pByte[1] << 8 | pByte[0];
			pByte += 4;
		}
		trans.mosi = spibuf;
		trans.bits.mosi = pktLen*8;
		spi_trans(LCD_SPIDEV, &trans);
		len -= pktLen;
	}

	// pthread_mutex_unlock(pSpi2Lock);
}

// len : in word (32 bit)
static void spi_write_words(uint32_t *pDat, int len)
{
	// pthread_mutex_lock(pSpi2Lock);
	int pktLen;
	uint32_t *pWord = pDat;
	spi_trans_t trans = {0};

	// Transmit 64-bytes packet
	while (len > 0)
	{
		pktLen = (len>=16) ? 16 : len;
		trans.bits.mosi = pktLen*32;
		trans.mosi = pWord;
		spi_trans(LCD_SPIDEV, &trans);
		len -= pktLen;
		pWord += pktLen;
	}
	// pthread_mutex_unlock(pSpi2Lock);
}

// len : in byte (8 bit)
static void spi_write_allBytes(uint8_t dat, int len)
{
	// pthread_mutex_lock(pSpi2Lock);
	int pktLen;
	spi_trans_t trans = {0};
	memset(spibuf, dat, sizeof(spibuf));

	// Transmit 64-bytes packet
	trans.mosi = spibuf;
	while (len > 0)
	{
		pktLen = (len>=16) ? 16 : len;
		trans.bits.mosi = pktLen*32;
		spi_trans(LCD_SPIDEV, &trans);
		len -= pktLen;
	}
	// pthread_mutex_unlock(pSpi2Lock);
}

static uint8_t lcd_dc_level;
static void lcd_set_dc(uint8_t lvl)
{
	lcd_dc_level = lvl;
}

/* Please see: 7.3.6 DDRAM Map to LCD Driver Output */
static inline int lcd_row2page(int row)
{
	return row / LINES_PER_PAGE;
}

static void lcd_tx_cmd(uint8_t *cmd, size_t size)
{
	lcd_set_dc(0);
	spi_write_bytes(cmd, 1);
	if (size > 1)       // with parameters
	{
		lcd_set_dc(1);
		spi_write_bytes(cmd + 1, size - 1);
	}
    // else 
        // without parameters
}

/* Define area of DDRAM where MCU can access
	col_s < col_e
	page_s < page_e
	col range  : 0 ~ 159
	page range : 0 ~ 39   (160160 LCD - 0~19)
 */
static void lcd_set_area(int col_s, int col_e, int page_s, int page_e)
{
	uint8_t cmd_set_page[] = {ST75161_SET_PAGE_ADDRESS(0, 0)};
	uint8_t cmd_set_col[] = {ST75161_SET_COL_ADDRESS(0, 0)};

	cmd_set_col[1] = col_s;
	cmd_set_col[2] = col_e;
	cmd_set_page[1] = page_s;
	cmd_set_page[2] = page_e;

	lcd_tx_cmd(cmd_set_col, sizeof(cmd_set_col));
	lcd_tx_cmd(cmd_set_page, sizeof(cmd_set_page));
}

#if 0
/*
 * screen's format(x,y):                   tx_buf's format
 * Row000 00,00 00,01 ... 00,9E 00,9F\
 * Row001 01,00 01,01 ... 01,9E 01,9F \
 * Row002 02,00 02,01 ... 02,9E 02,9F  |
 * Row003 03,00 03,01 ... 03,9E 03,9F  |-> Page00 00,00 00,01 ... 00,9E 00,9F
 * Row004 04,00 04,01 ... 04,9E 04,9F  |
 * Row005 05,00 05,01 ... 05,9E 05,9F  |
 * Row006 06,00 06,01 ... 06,9E 06,9F /
 * Row007 07,00 07,01 ... 07,9E 07,9F/
 *
 *        ...
 *
 * Row152 98,00 98,01 ... 98,9E 98,9F\
 * Row153 99,00 99,01 ... 99,9E 99,9F \
 * Row154 9A,00 9A,01 ... 9A,9E 9A,9F  |
 * Row155 9B,00 9B,01 ... 9B,9E 9B,9F  |-> Page19 13,00 13,01 ... 13,9E 13,9F
 * Row156 9C,00 9C,01 ... 9C,9E 9C,9F  |
 * Row157 9D,00 9D,01 ... 9D,9E 9D,9F  |
 * Row158 9E,00 9E,01 ... 9E,9E 9E,9F /
 * Row159 9F,00 9f,01 ... 9F,9E 9F,9F/
 */
static int screen_to_st75161_mono_page(uint8_t *tx_buf, uint8_t *screen)
{
	#define BIT(n) (1 << (n))
	int screen_idx = 0;
	int i, x, y;

	for (y = LINES_PER_PAGE - 1; y >= 0; y--)
    {
		for (x = 0; x < LCD_WIDTH; )
        {
			uint8_t b8 = screen[screen_idx++];

			for (i = 7; i >= 0; i--)
            {
				if (b8 & BIT(i))
					tx_buf[x] |= BIT(y);
				x++;
			}
		}
	}

	return 0;
}

static int write_vmem(int y_start, int y_end)
{
	uint8_t *screen, *screen_base = lcd_buf, *tx_buf = lcd_buf_bus;
	int line_length_per_page = LINE_LENGTH * LINES_PER_PAGE;
	uint8_t cmd_write_data = ST75161_WRITE_DATA;

	int page_s = lcd_row2page(y_start);
	int page_e = lcd_row2page(y_end);
	int page;

	screen = screen_base + page_s * line_length_per_page;

	for (page = page_s; page <= page_e; page++)
    {
		memset(tx_buf, 0, LCD_WIDTH);
		screen_to_st75161_mono_page(tx_buf, screen);

		screen += line_length_per_page;
		tx_buf += LCD_WIDTH;
	}

	lcd_set_area(0, LCD_WIDTH - 1, page_s, page_e);
	lcd_tx_cmd(&cmd_write_data, 1);

	spi_write_bytes(lcd_buf_bus, (page_e - page_s + 1) * LCD_WIDTH);

	return 0;
}

/* 液晶屏刷新数据 */
int linuxfb_flush(const uint8_t *data, int x_s, int y_s, int x_e, int y_e, size_t data_size)
{
	uint8_t *p = lcd_buf + y_s * LINE_LENGTH;
	const uint8_t *p_data = data;

	if (x_s != 0 || x_e != LCD_WIDTH - 1)
    {
		log_err("%s: unaligned doesn't impl", __func__);
		return -1;
	}
    else if ((int)data_size != (y_e - y_s + 1) * LCD_WIDTH / 8)
    {
		log_err("%s: data size doesn't match, %d ~ %d, %ld",
			__func__, y_s, y_e, (long)data_size);
		return -1;
	}

	memcpy(p, p_data, data_size);
	write_vmem(0, LCD_HEIGHT - 1);

	return 0;
}
#endif

/* 底层调节对比度 */
bool lcd_set_gamma(uint32_t contrast)
{
	uint8_t cmd_set_vop[] = {ST75161_SET_VOP(contrast)};

	if (contrast < GAMMA_MIN || contrast > GAMMA_MAX)
	{
		return false;
	}

	//LCD_DELAY_MS(1);
	lcd_tx_cmd(cmd_set_vop, sizeof(cmd_set_vop));
	return true;
}

void lcd_flush(int col_s, int col_e, int page_s, int page_e, uint8_t *buf)
{
	uint8_t tmp;
	int byteCnt = (col_e - col_s + 1) * (page_e - page_s + 1);
	// Setting page and column address
	lcd_set_area(col_s, col_e, page_s, page_e);
	// Writing data to DDRAM
	tmp = ST75161_WRITE_DATA;
	lcd_set_dc(0);
	spi_write_bytes(&tmp, 1);
	lcd_set_dc(1);
	spi_write_bytes(buf, byteCnt);
}

/* LCD clear screen */
void lcd_clear(uint8_t val)
{
	uint8_t tmp;

	// Prepare display buffer to send by SPI
	memset(lcd_buf, val, sizeof(lcd_buf));
	// Setting page and column address
	lcd_set_area(0, LCD_WIDTH - 1, 0, lcd_row2page(LCD_HEIGHT-1));
	// Writing data to DDRAM
	tmp = ST75161_WRITE_DATA;
	lcd_set_dc(0);
	spi_write_bytes(&tmp, 1);
	lcd_set_dc(1);
	spi_write_allBytes(val, LCD_WIDTH * LCD_HEIGHT / 8);
}

/* Execute the initialization sequence */
static void lcd_init_cmd(void)
{
	const int *seqs = default_init_sequence;

	while (*seqs != -3)
    {
		if (*seqs < 0)
        {
			int idx = 0, delay = -*seqs;
			uint8_t cmd, data[64];

			LCD_DELAY_US(delay*50);
			// cmd
			seqs++;
			cmd = (uint8_t)(*seqs);
			lcd_set_dc(0);
			spi_write_bytes(&cmd, 1);
			// data
			seqs++;
			while (*seqs >= 0)
            {
				data[idx++] = (uint8_t)(*seqs++);
			}
			if (idx > 0)
			{
				lcd_set_dc(1);
				spi_write_bytes(data, idx);
			}
		}
	}
}

static void IRAM_ATTR spi_event_callback(int event, void *arg)
{
	switch (event)
	{
	case SPI_INIT_EVENT:
		break;
	case SPI_TRANS_START_EVENT:
		LCD_CD(lcd_dc_level);
		LCD_CSN(0);
		break;
	case SPI_TRANS_DONE_EVENT:
		LCD_CSN(1);
		break;
	case SPI_DEINIT_EVENT:
		break;
	default:
		break;
	}
}

void lcd_spi_init(void)
{
	// SPI init
	spi_config_t spi_config;
	// Load default interface parameters
	// CS_EN:1, MISO_EN:1, MOSI_EN:1
	// BYTE_TX_ORDER:1, BYTE_TX_ORDER:1, BIT_RX_ORDER:0, BIT_TX_ORDER:0
	// CPHA:0, CPOL:0
	spi_config.interface.val = SPI_DEFAULT_INTERFACE;
	// Cancel hardware cs
	spi_config.interface.cs_en = 0;
	// MISO pin is used for DC
	spi_config.interface.miso_en = 0;
	// CPOL: 1, CPHA: 1
	spi_config.interface.cpol = 1;
	spi_config.interface.cpha = 1;

	// Load default interrupt enable
	// TRANS_DONE: true, WRITE_STATUS: false, READ_STATUS: false, WRITE_BUFFER: false, READ_BUFFER: false
	spi_config.intr_enable.val = SPI_MASTER_DEFAULT_INTR_ENABLE;
	// Set SPI to master mode
	// 8266 Only support half-duplex
	spi_config.mode = SPI_MASTER_MODE;
	// Set the SPI clock frequency division factor
	spi_config.clk_div = SPI_4MHz_DIV;
	// Register SPI event callback function
	spi_config.event_cb = spi_event_callback;
	spi_init(LCD_SPIDEV, &spi_config);
}

int lcd_spiiokey_get()
{
	int keyValue = 0;

	// MUX spi to gpio input as keys
	gpio_config_t io_conf;
	io_conf.intr_type = GPIO_INTR_DISABLE;
	io_conf.mode = GPIO_MODE_INPUT;
	// 16-cd; 14-sclk; 13-mosi; 12-(miso)not used
	io_conf.pin_bit_mask = ((1ULL<<LCD_PIN_CD) | (1ULL<<14) | (1ULL<<13) | (1ULL<<12)); // RST, CD & CSN
	io_conf.pull_down_en = 0;
	io_conf.pull_up_en = 0;
	gpio_config(&io_conf);

	// read key values
	vTaskDelay(1 / portTICK_RATE_MS);
	keyValue = gpio_get_level(12) << 1 | gpio_get_level(13) | gpio_get_level(14) << 2 | gpio_get_level(16) << 3;

	// set ios to correct function for lcd operation
	io_conf.intr_type = GPIO_INTR_DISABLE;
	io_conf.mode = GPIO_MODE_OUTPUT;
	io_conf.pin_bit_mask = 1ULL<<LCD_PIN_CD;
	io_conf.pull_down_en = 0;
	io_conf.pull_up_en = 1;
	gpio_config(&io_conf);

	// following codes come from spi_set_interface() from spi.c
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTMS_U, FUNC_HSPI_CLK); //GPIO14 is SPI CLK pin (Clock)
	PIN_PULLUP_EN(PERIPHS_IO_MUX_MTCK_U);
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTCK_U, FUNC_HSPID_MOSI); //GPIO13 is SPI MOSI pin (Master Data Out)

	return keyValue;
}

/* GPIO & SPI initialization */
static bool lcd_itf_init()
{
	// GPIO init
	gpio_config_t io_conf;
	io_conf.intr_type = GPIO_INTR_DISABLE;
	io_conf.mode = GPIO_MODE_OUTPUT;
	io_conf.pin_bit_mask = LCD_PIN_PIN_SEL; // RST, CD & CSN
	io_conf.pull_down_en = 0;
	io_conf.pull_up_en = 1;
	gpio_config(&io_conf);
	gpio_set_level(LCD_PIN_BL, 0);
	gpio_set_level(LCD_PIN_CD, 1);
	gpio_set_level(LCD_PIN_CSN, 1);
	gpio_set_level(LCD_PIN_RESET, 1);

	lcd_spi_init();

	return true;
}

void lcd_BL(uint8_t state)
{
	LCD_BL(state);
}

/* LCD初始化 */
void lcd_init(void)
{
	lcd_itf_init();

	LCD_RST(0);
	LCD_DELAY_MS(10);
	LCD_RST(1);
	LCD_DELAY_MS(50);
	LCD_DELAY_MS(50);

	lcd_init_cmd();
}


