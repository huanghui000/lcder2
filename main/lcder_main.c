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

#include "esp_system.h"
#include "esp_log.h"
#include "esp_libc.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "lcd_st75161.h"
#include "startup_ui.h"
#include "wifi_sta.h"
#include "http_seniverse.h"
#include "config_portal.h"
#include "app_control.h"

#include "../GUI/lvgl.h"
#include "../GUI/lvgl_port/lv_port_disp.h"
#include "../GUI/src/extra/themes/lv_themes.h"
#include "../GUI/src/extra/themes/mono/lv_theme_mono.h"
#include "../GUI/src/misc/lv_log.h"
#include "../GUI/src/extra/libs/lv_libs.h"

#include "lwip/apps/sntp.h"

#include "ui.h"

#define WIFI_SSID       "test"
#define WIFI_PASSWD     "test@1234"

#define APP_KEY_UP      (1U << 3)
#define APP_KEY_DOWN    (1U << 2)
#define APP_KEY_LEFT    (1U << 1)
#define APP_KEY_RIGHT   (1U << 0)
#define QR_SCREEN_TIMEOUT_MS 10000
#define WEATHER_PAGE_INTERVAL_MS 10000
#define WEATHER_PAGE_ANIM_MS 500

static char ssid[33] = WIFI_SSID;
static char passwd[65] = WIFI_PASSWD;
static const char *TAG = "lvgl";

SemaphoreHandle_t xLvglMutex;

static lv_indev_t *s_keypad_indev;
static lv_group_t *s_keypad_group;
static lv_indev_drv_t s_keypad_drv;
static lv_obj_t *s_qr_screen;
static lv_obj_t *s_qr_code;
static lv_obj_t *s_qr_title_label;
static lv_obj_t *s_qr_link_label;
static uint8_t s_keys_filtered;
static volatile char s_force_weather_screen;
static TickType_t s_qr_deadline;
static TickType_t s_weather_page_deadline;
static uint8_t s_weather_page_index;

static uint32_t app_key_to_lvgl(uint8_t key_mask);
static void app_set_weather_page_locked(uint8_t page_index, char animated);

static void app_keypad_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    LV_UNUSED(drv);
    data->state = (s_keys_filtered != 0) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->key = app_key_to_lvgl(s_keys_filtered);
    data->continue_reading = false;
}

static uint8_t app_keys_get_filtered(void)
{
    static uint8_t last_raw;
    static uint8_t stable_raw;
    static uint8_t stable_count;
    uint8_t raw = (uint8_t)((~lcd_spiiokey_get()) & 0x0F);

    if (raw == last_raw)
    {
        if (stable_count < 3)
        {
            stable_count++;
        }
    }
    else
    {
        last_raw = raw;
        stable_count = 0;
    }

    if (stable_count >= 2)
    {
        stable_raw = raw;
    }

    return stable_raw;
}

static uint32_t app_key_to_lvgl(uint8_t key_mask)
{
    if (key_mask & APP_KEY_UP) return LV_KEY_UP;
    if (key_mask & APP_KEY_DOWN) return LV_KEY_DOWN;
    if (key_mask & APP_KEY_LEFT) return LV_KEY_LEFT;
    if (key_mask & APP_KEY_RIGHT) return LV_KEY_RIGHT;
    return 0;
}

static void app_update_qr_screen(void)
{
    char url[96];

    config_portal_get_url(url, sizeof(url));
    lv_qrcode_update(s_qr_code, url, strlen(url));
    lv_label_set_text(s_qr_link_label, url);
}

static void app_show_weather_screen_locked(void)
{
    app_set_weather_page_locked(s_weather_page_index, 0);
    lv_scr_load(ui_Screen1);
    s_qr_deadline = 0;
    s_weather_page_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(WEATHER_PAGE_INTERVAL_MS);
}

static void app_show_qr_screen_locked(void)
{
    app_update_qr_screen();
    lv_scr_load(s_qr_screen);
    s_qr_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(QR_SCREEN_TIMEOUT_MS);
}

void app_request_weather_screen(void)
{
    s_force_weather_screen = 1;
}

static void app_set_weather_page_x(void *obj, int32_t x)
{
    lv_obj_set_x((lv_obj_t *)obj, x);
}

static void app_set_weather_page_locked(uint8_t page_index, char animated)
{
    int32_t target_x;

    page_index = (uint8_t)(page_index % 3);
    target_x = -(int32_t)(page_index * 160);
    s_weather_page_index = page_index;

    lv_anim_del(ui_WeatherContent, app_set_weather_page_x);
    if (!animated)
    {
        lv_obj_set_x(ui_WeatherContent, target_x);
        return;
    }

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, ui_WeatherContent);
    lv_anim_set_exec_cb(&anim, app_set_weather_page_x);
    lv_anim_set_values(&anim, lv_obj_get_x(ui_WeatherContent), target_x);
    lv_anim_set_time(&anim, WEATHER_PAGE_ANIM_MS);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_start(&anim);
}

static void app_init_qr_screen(void)
{
    s_qr_screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_qr_screen);
    lv_obj_set_size(s_qr_screen, 160, 160);
    lv_obj_clear_flag(s_qr_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_qr_screen, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_qr_screen, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_qr_screen, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    s_qr_title_label = lv_label_create(s_qr_screen);
    lv_obj_set_width(s_qr_title_label, 144);
    lv_obj_set_style_text_align(s_qr_title_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_qr_title_label, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s_qr_title_label, &lv_font_montserrat_18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align_to(s_qr_title_label, s_qr_screen, LV_ALIGN_CENTER, 0, -56);
    lv_label_set_text(s_qr_title_label, "Weather App");

    s_qr_code = lv_qrcode_create(s_qr_screen, 72, lv_color_black(), lv_color_white());
    lv_obj_align(s_qr_code, LV_ALIGN_CENTER, 0, 0);

    s_qr_link_label = lv_label_create(s_qr_screen);
    lv_obj_set_width(s_qr_link_label, 136);
    lv_label_set_long_mode(s_qr_link_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_qr_link_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_qr_link_label, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s_qr_link_label, &lv_font_montserrat_18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align_to(s_qr_link_label, s_qr_code, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
    lv_label_set_text(s_qr_link_label, "http://0.0.0.0/");
}

static void app_init_keypad_indev(void)
{
    lv_indev_drv_init(&s_keypad_drv);
    s_keypad_drv.type = LV_INDEV_TYPE_KEYPAD;
    s_keypad_drv.read_cb = app_keypad_read_cb;

    s_keypad_indev = lv_indev_drv_register(&s_keypad_drv);
    s_keypad_group = lv_group_create();
    lv_group_set_default(s_keypad_group);
    lv_group_add_obj(s_keypad_group, ui_Screen1);
    lv_group_add_obj(s_keypad_group, s_qr_screen);
    lv_indev_set_group(s_keypad_indev, s_keypad_group);
}

void lv_task(void *pvParameters)
{
    uint8_t last_keys;
    char keypad_armed = 0;

    ESP_LOGI(TAG, "lv_task start.");
    app_init_qr_screen();
    app_init_keypad_indev();
    last_keys = app_keys_get_filtered();
    s_keys_filtered = last_keys;
    app_show_weather_screen_locked();

    while (1)
    {
        TickType_t now;
        uint8_t keys_now;
        uint8_t key_press_edge;

        vTaskDelay(pdMS_TO_TICKS(10));
        xSemaphoreTake(xLvglMutex, portMAX_DELAY);

        keys_now = app_keys_get_filtered();
        key_press_edge = (uint8_t)(keys_now & (uint8_t)~last_keys);
        s_keys_filtered = keys_now;

        if (!keypad_armed)
        {
            if (keys_now == 0)
            {
                keypad_armed = 1;
            }
        }
        else if (key_press_edge != 0)
        {
            app_show_qr_screen_locked();
        }

        now = xTaskGetTickCount();
        if (s_force_weather_screen)
        {
            s_force_weather_screen = 0;
            app_show_weather_screen_locked();
        }
        else if (lv_scr_act() == ui_Screen1 && s_weather_page_deadline != 0 && now >= s_weather_page_deadline)
        {
            app_set_weather_page_locked((uint8_t)(s_weather_page_index + 1), 1);
            s_weather_page_deadline = now + pdMS_TO_TICKS(WEATHER_PAGE_INTERVAL_MS);
        }
        else if (lv_scr_act() == s_qr_screen && s_qr_deadline != 0 && now >= s_qr_deadline)
        {
            app_show_weather_screen_locked();
        }

        startup_ui_process();
        lv_task_handler();
        xSemaphoreGive(xLvglMutex);

        last_keys = keys_now;
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
    time_t now = 0;
    struct tm timeinfo = {0};
    int retry = 0;
    const int retry_count = 10;

    initialize_sntp();

    while (timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count)
    {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(pdMS_TO_TICKS(2000));
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

    if (timeinfo.tm_year < (2016 - 1900))
    {
        ESP_LOGI(TAG, "Time is not set yet. Connecting to WiFi and getting time over NTP.");
        obtain_time();
    }

    setenv("TZ", "CST-8", 1);
    tzset();

    while (1)
    {
        time(&now);
        localtime_r(&now, &timeinfo);

        if (timeinfo.tm_year < (2016 - 1900))
        {
            ESP_LOGE(TAG, "The current date/time error");
        }
        else
        {
            strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
            ESP_LOGI(TAG, "The current date/time in Shanghai is: %s", strftime_buf);
        }

        ESP_LOGI(TAG, "Free heap size: %d", esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

static void lv_esp8266_log(const char *buf)
{
    ESP_LOGI(TAG, "%s", buf);
}

void app_main(void)
{
    char buf[32];
    uint32_t secCount = 0;
    int min_last = -1;
    char last_wifi_connected = 0;

    ESP_ERROR_CHECK(nvs_flash_init());
    setenv("TZ", "CST-8", 1);
    tzset();

    vTaskDelay(pdMS_TO_TICKS(2000));
    lcd_init();
    lcd_BL(1);

    lv_log_register_print_cb(lv_esp8266_log);
    lv_init();
    lv_port_disp_init();

    xLvglMutex = xSemaphoreCreateMutex();
    xSemaphoreTake(xLvglMutex, portMAX_DELAY);
    ui_init();
    startup_ui_init();
    startup_ui_show_status("System", "Preparing UI and network", 8);
    startup_ui_process();
    lv_task_handler();
    xSemaphoreGive(xLvglMutex);

    http_seniverse_init();
    wifi_init_sta(ssid, passwd);
    xTaskCreate(&lv_task, "lv_task", 4096, NULL, 4, NULL);

    while (1)
    {
        time_t t;
        static char s_portal_started = 0;
        static char s_sntp_started = 0;

        vTaskDelay(pdMS_TO_TICKS(1000));

        if (is_wifi_connected != 0 && !last_wifi_connected)
        {
            secCount = 0;
            ESP_LOGI(TAG, "WiFi got IP, trigger immediate weather refresh");
            http_seniverse_request_refresh();
        }
        last_wifi_connected = is_wifi_connected;

        if (is_wifi_connected != 0)
        {
            if (!s_portal_started)
            {
                config_portal_start();
                s_portal_started = 1;
                ESP_LOGI(TAG, "config portal started after WiFi got IP, heap=%u",
                    (unsigned)esp_get_free_heap_size());
            }

            if (!s_sntp_started)
            {
                xTaskCreate(sntp_task, "sntp_task", 1536, NULL, 10, NULL);
                s_sntp_started = 1;
                ESP_LOGI(TAG, "sntp_task started after WiFi got IP, heap=%u",
                    (unsigned)esp_get_free_heap_size());
            }
        }

        if (secCount++ >= 300)
        {
            secCount = 0;
            if (is_wifi_connected != 0)
            {
                http_seniverse_request_refresh();
            }
        }

        t = time(0);
        if ((int)(t / 60) != min_last)
        {
            struct tm *local = localtime(&t);
            min_last = (int)(t / 60);

            xSemaphoreTake(xLvglMutex, portMAX_DELAY);
            strftime(buf, sizeof(buf), "%Y.%m.%d", local);
            lv_label_set_text(ui_LabelDate, buf);
            strftime(buf, sizeof(buf), "%H:%M", local);
            lv_label_set_text(ui_LabelTime, buf);
            xSemaphoreGive(xLvglMutex);
        }
    }
}
