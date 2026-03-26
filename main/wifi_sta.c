#include "wifi_sta.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_smartconfig.h"
#include "smartconfig_ack.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include <string.h>

static const char *TAG = "wifi station";

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT		BIT0
#define WIFI_FAIL_BIT			BIT1
#define WIFI_SMARTCONFIG_BIT	BIT2

char is_wifi_connected;

static void smartconfig_example_task(void* parm)
{
	EventBits_t uxBits;
	ESP_ERROR_CHECK(esp_smartconfig_set_type(SC_TYPE_ESPTOUCH));
	smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_smartconfig_start(&cfg));

	while (1)
	{
		uxBits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_SMARTCONFIG_BIT, true, false, portMAX_DELAY);

		if (uxBits & WIFI_CONNECTED_BIT)
		{
			ESP_LOGI(TAG, "WiFi Connected to ap");
			vTaskDelete(NULL);
	   }

		if (uxBits & WIFI_SMARTCONFIG_BIT) 
		{
			ESP_LOGI(TAG, "smartconfig over");
			esp_smartconfig_stop();
			vTaskDelete(NULL);
		}
	}
}

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
	{
		xTaskCreate(smartconfig_example_task, "smartconfig_example_task", 4096, NULL, 3, NULL);
	}
	else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
	{
		is_wifi_connected = 0;
		esp_wifi_connect();
		xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
		// xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
		ESP_LOGI(TAG, "connect to the AP fail!");
		ESP_LOGI(TAG, "retry to connect to the AP...");
	}
	else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
	{
		ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
		ESP_LOGI(TAG, "got ip:%s", ip4addr_ntoa(&event->ip_info.ip));
		xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
		is_wifi_connected = 1;
	}
	else if (event_base == SC_EVENT && event_id == SC_EVENT_SCAN_DONE)
	{
		ESP_LOGI(TAG, "Scan done");
	}
	else if (event_base == SC_EVENT && event_id == SC_EVENT_FOUND_CHANNEL)
	{
		ESP_LOGI(TAG, "Found channel");
	}
	else if (event_base == SC_EVENT && event_id == SC_EVENT_GOT_SSID_PSWD)
	{
		ESP_LOGI(TAG, "Got SSID and password");

		smartconfig_event_got_ssid_pswd_t* evt = (smartconfig_event_got_ssid_pswd_t*)event_data;
		wifi_config_t wifi_config;
		uint8_t ssid[33] = { 0 };
		uint8_t password[65] = { 0 };
		uint8_t rvd_data[33] = { 0 };

		bzero(&wifi_config, sizeof(wifi_config_t));
		memcpy(wifi_config.sta.ssid, evt->ssid, sizeof(wifi_config.sta.ssid));
		memcpy(wifi_config.sta.password, evt->password, sizeof(wifi_config.sta.password));
		wifi_config.sta.bssid_set = evt->bssid_set;

		if (wifi_config.sta.bssid_set == true)
		{
			memcpy(wifi_config.sta.bssid, evt->bssid, sizeof(wifi_config.sta.bssid));
		}

		memcpy(ssid, evt->ssid, sizeof(evt->ssid));
		memcpy(password, evt->password, sizeof(evt->password));
		ESP_LOGI(TAG, "SSID:%s", ssid);
		ESP_LOGI(TAG, "PASSWORD:%s", password);
		if (evt->type == SC_TYPE_ESPTOUCH_V2)
		{
			ESP_ERROR_CHECK( esp_smartconfig_get_rvd_data(rvd_data, sizeof(rvd_data)) );
			ESP_LOGI(TAG, "RVD_DATA:%s", rvd_data);
		}

		ESP_ERROR_CHECK(esp_wifi_disconnect());
		ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
		ESP_ERROR_CHECK(esp_wifi_connect());
	}
	else if (event_base == SC_EVENT && event_id == SC_EVENT_SEND_ACK_DONE)
	{
		xEventGroupSetBits(s_wifi_event_group, WIFI_SMARTCONFIG_BIT);
	}
}

void wifi_init_sta(char *ssid, char *passwd)
{
	is_wifi_connected = 0;
	s_wifi_event_group = xEventGroupCreate();

	tcpip_adapter_init();

	ESP_ERROR_CHECK(esp_event_loop_create_default());

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));
	ESP_ERROR_CHECK(esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));

#if 0
	wifi_config_t wifi_config = {0};
	strncpy((char *)&wifi_config.sta.ssid, ssid, 32);
	strncpy((char *)&wifi_config.sta.password, passwd, 32);

	/* Setting a password implies station will connect to all security modes including WEP/WPA.
	 * However these modes are deprecated and not advisable to be used. Incase your Access point
	 * doesn't support WPA2, these mode can be enabled by commenting below line */

	if (strlen((char *)wifi_config.sta.password))
	{
		wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
	}
#endif
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	// ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
	ESP_ERROR_CHECK(esp_wifi_start());
#if 0
	xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

	ESP_LOGI(TAG, "wifi_init_sta finished.");
#endif
}
