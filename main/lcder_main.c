/* spi_oled example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// #include "esp8266/gpio_struct.h"
// #include "esp8266/spi_struct.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_libc.h"

#include "driver/gpio.h"
// #include "driver/spi.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "lcd_st75161.h"
#include "startup_ui.h"
#include "wifi_sta.h"
#include "http_seniverse.h"

#include "../GUI/lvgl.h"
#include "../GUI/lvgl_port/lv_port_disp.h"
#include "../GUI/src/extra/themes/lv_themes.h"
#include "../GUI/src/extra/themes/mono/lv_theme_mono.h"
#include "../GUI/src/misc/lv_log.h"

#include "../GUI/src/extra/libs/lv_libs.h"

#include "lwip/apps/sntp.h"

#include "ui.h"

#define WIFI_SSID		"test"
#define WIFI_PASSWD		"test@1234"
static char ssid[32] = WIFI_SSID;
static char passwd[32] = WIFI_PASSWD;

#if 0
void keys_init()
{
	gpio_config_t io_conf;
	io_conf.intr_type = GPIO_INTR_DISABLE;
	io_conf.mode = GPIO_MODE_INPUT;
	io_conf.pin_bit_mask = GPIO_Pin_0;
	io_conf.pull_down_en = 0;
	io_conf.pull_up_en = 0;
	gpio_config(&io_conf);
}

int keys_get()
{
	return gpio_get_level(GPIO_NUM_0);
}
#endif
static const char *TAG = "lvgl";

SemaphoreHandle_t xLvglMutex;

void lv_task(void *pvParameters)
{
	ESP_LOGI(TAG, "lv_task start.");

	static uint32_t inc= 0, on = 0;

	while (1)
	{
		int keyValues;
		// ESP_LOGI(TAG, "lv_task 2");
		vTaskDelay(10 / portTICK_RATE_MS);
		xSemaphoreTake(xLvglMutex, portMAX_DELAY);
		keyValues = lcd_spiiokey_get();
		startup_ui_process();
		lv_task_handler();
		xSemaphoreGive(xLvglMutex);
		
		if (inc++ == 200)
		{
			inc = 0;
			// ESP_LOGI(TAG, "key values = %d", keyValues);
			if (on == 0)
			{
				on = 1;
			}
			else
			{
				on = 0;
			}
		}
	}
}

static void initialize_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "ntp1.aliyun.com");
    sntp_setservername(1, "ntp2.aliyun.com");
    sntp_setservername(2, "ntp3.aliyun.com");
    sntp_init();
}

static void obtain_time(void)
{
    initialize_sntp();

    // wait for time to be set
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    const int retry_count = 10;

    while (timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count)
	{
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        time(&now);
        localtime_r(&now, &timeinfo);
    }
}

static void sntp_task(void *arg)
{
    time_t now;
    struct tm timeinfo;
    char strftime_buf[64];

    time(&now);
    localtime_r(&now, &timeinfo);

    // Is time set? If not, tm_year will be (1970 - 1900).
    if (timeinfo.tm_year < (2016 - 1900))
	{
        ESP_LOGI(TAG, "Time is not set yet. Connecting to WiFi and getting time over NTP.");
        obtain_time();
    }

    // Set timezone to Eastern Standard Time and print local time
    // setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0", 1);
    // tzset();

    // Set timezone to China Standard Time
    setenv("TZ", "CST-8", 1);
    tzset();

    while (1)
	{
        // update 'now' variable with current time
        time(&now);
        localtime_r(&now, &timeinfo);

        if (timeinfo.tm_year < (2016 - 1900)) {
            ESP_LOGE(TAG, "The current date/time error");
        } else {
            strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
            ESP_LOGI(TAG, "The current date/time in Shanghai is: %s", strftime_buf);
        }

        ESP_LOGI(TAG, "Free heap size: %d\n", esp_get_free_heap_size());
        vTaskDelay(30000 / portTICK_RATE_MS);
    }
}


static void lv_esp8266_log(const char *buf)
{
    ESP_LOGI(TAG, buf);
}

void app_main(void)
{
	
    ESP_ERROR_CHECK(nvs_flash_init());

	// ESP_LOGI(TAG, "init gpio");
	// ESP_LOGI(TAG, "init hspi");
	// ESP_LOGI(TAG, "init oled");

	vTaskDelay(2000/portTICK_RATE_MS);
	lcd_init();
	lcd_BL(1);
	// lcd_clear(0x00);

    lv_log_register_print_cb(lv_esp8266_log);
	lv_init();
	lv_port_disp_init();
	xLvglMutex = xSemaphoreCreateMutex();
	xSemaphoreTake(xLvglMutex, portMAX_DELAY);
	ui_init();
	startup_ui_init();
	startup_ui_show_status("系统启动中", "正在准备主界面与网络服务", 8);
	startup_ui_process();
	lv_task_handler();
	xSemaphoreGive(xLvglMutex);

	// lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_ALL, NULL); /*Assign a callback to the button*/

	// lv_obj_t *label = lv_label_create(btn); /*Add a label to the button*/
	// lv_label_set_text(label, "Button");		/*Set the labels text*/
	// lv_obj_center(label);

	wifi_init_sta(ssid, passwd);

	// SNTP service uses LwIP, please allocate large stack space.
    xTaskCreate(sntp_task, "sntp_task", 2048, NULL, 10, NULL);
	eg_http_get = xEventGroupCreate();
	xTaskCreate(&http_get_task, "http_get_task", 1024*4, NULL, 5, NULL);
	xTaskCreate(&lv_task, "lv_task", 16*1024, NULL, 4, NULL);

#if 0  //quan.suning.com not working 20251116
	// Update weather information on startup
	xEventGroupSetBits(eg_http_get, HTTP_GET_WEATHER_BIT);
	// wait until internet time got
	while (1)
	{
		xEventGroupSetBits(eg_http_get, HTTP_GET_TIME_BIT);
		vTaskDelay(2000/portTICK_RATE_MS);
		if (time(0) > 3600*24*365)
		{
			// when internet time got, time() return much bigger than seconds in a year
			break;
		}
	}
#endif

	char buf[32];
	uint32_t secCount=0;
	int min_last = 0;
	while (1)
	{
		vTaskDelay(1000 / portTICK_RATE_MS);
		if (secCount++ >= 60)
		{
			secCount = 0;
			if (is_wifi_connected != 0)
			{
				// xEventGroupSetBits(eg_http_get, HTTP_GET_TIME_BIT | HTTP_GET_WEATHER_BIT);
				// quan.suning.com not working 20251116
				xEventGroupSetBits(eg_http_get, HTTP_GET_WEATHER_BIT);
			}
		}

		time_t t = time(0);
		// if minutes changed, update relative labels
		if (t/60 != min_last)
		{
			min_last = t/60;
			struct tm *local = localtime(&t);
			ESP_LOGI(TAG, "Updating time label...");
			xSemaphoreTake(xLvglMutex, portMAX_DELAY);
			strftime(buf, 32, "%Y.%m.%d", local);
			lv_label_set_text(ui_LabelDate, buf);
			strftime(buf, 32, "%H:%M", local);
			lv_label_set_text(ui_LabelTime, buf);
			xSemaphoreGive(xLvglMutex);
		}
	}
}
