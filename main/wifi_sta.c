#include "wifi_sta.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_smartconfig.h"
#include "nvs.h"
#include "startup_ui.h"

#include <string.h>

static const char *TAG = "wifi station";

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT		BIT0
#define WIFI_SMARTCONFIG_BIT	BIT2

#define SMARTCONFIG_TIMEOUT_SEC        45
#define SMARTCONFIG_ACK_TIMEOUT_SEC    15
#define WIFI_RECONNECT_MAX_RETRY       10

char is_wifi_connected;
static char s_smartconfig_running;
static char s_sc_got_credentials;
static char s_has_saved_wifi_config;
static uint32_t s_sc_session_id;
static uint32_t s_sta_disconnect_count;
static uint32_t s_wifi_reconnect_retry;

/* 从 NVS 读取上次配网成功保存的 Wi-Fi 凭据。 */
static esp_err_t load_wifi_credentials_from_nvs(char *ssid, size_t ssid_len, char *password, size_t password_len)
{
	nvs_handle nvs_handle;
	esp_err_t err;

	err = nvs_open("wifi_cfg", NVS_READONLY, &nvs_handle);
	if (err != ESP_OK)
	{
		ESP_LOGW(TAG, "nvs_open for wifi config failed: %s", esp_err_to_name(err));
		return err;
	}

	err = nvs_get_str(nvs_handle, "ssid", ssid, &ssid_len);
	if (err == ESP_OK)
	{
		err = nvs_get_str(nvs_handle, "password", password, &password_len);
	}

	nvs_close(nvs_handle);

	if (err == ESP_OK)
	{
		ESP_LOGI(TAG, "loaded wifi credentials from NVS, ssid=%s", ssid);
		startup_ui_show_status("读取网络配置", "已加载保存的 Wi-Fi 配置", 26);
	}
	else
	{
		ESP_LOGW(TAG, "failed to load wifi credentials from NVS: %s", esp_err_to_name(err));
		startup_ui_show_status("等待配网", "未找到已保存配置，准备进入配网", 18);
	}

	return err;
}

/* SmartConfig 成功后，将 SSID/密码持久化到 NVS，供下次开机直连。 */
static esp_err_t save_wifi_credentials_to_nvs(const char *ssid, const char *password)
{
	nvs_handle nvs_handle;
	esp_err_t err;

	err = nvs_open("wifi_cfg", NVS_READWRITE, &nvs_handle);
	if (err != ESP_OK)
	{
		ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
		return err;
	}

	err = nvs_set_str(nvs_handle, "ssid", ssid);
	if (err == ESP_OK)
	{
		err = nvs_set_str(nvs_handle, "password", password);
	}
	if (err == ESP_OK)
	{
		err = nvs_commit(nvs_handle);
	}

	nvs_close(nvs_handle);

	if (err == ESP_OK)
	{
		ESP_LOGI(TAG, "saved wifi credentials to NVS");
	}
	else
	{
		ESP_LOGE(TAG, "failed to save wifi credentials to NVS: %s", esp_err_to_name(err));
	}

	return err;
}

static const char *sc_event_to_name(int32_t event_id)
{
	switch (event_id)
	{
	case SC_EVENT_SCAN_DONE:
		return "SCAN_DONE";
	case SC_EVENT_FOUND_CHANNEL:
		return "FOUND_CHANNEL";
	case SC_EVENT_GOT_SSID_PSWD:
		return "GOT_SSID_PSWD";
	case SC_EVENT_SEND_ACK_DONE:
		return "SEND_ACK_DONE";
	default:
		return "UNKNOWN";
	}
}

static void stop_smartconfig_with_log(const char *reason, uint32_t session_id)
{
	esp_err_t err;

	if (!s_smartconfig_running)
	{
		return;
	}

	ESP_LOGW(TAG, "smartconfig stop, session=%lu, reason=%s", (unsigned long)session_id, reason);
	err = esp_smartconfig_stop();
	if (err != ESP_OK)
	{
		ESP_LOGW(TAG, "esp_smartconfig_stop returned %s", esp_err_to_name(err));
	}
	s_smartconfig_running = 0;
}

/* 配网任务只负责等待关键事件：
 * 1. 收到 ACK 完成事件后结束 SmartConfig；
 * 2. 已拿到 IP 但手机端迟迟不确认时，超时退出并保留当前连接。
 */
static void smartconfig_example_task(void* parm)
{
	EventBits_t uxBits;
	uint32_t session_id = 0;
	uint32_t connected_seconds = 0;
	char connected_seen = 0;

	ESP_ERROR_CHECK(esp_smartconfig_set_type(SC_TYPE_ESPTOUCH));
	ESP_ERROR_CHECK(esp_esptouch_set_timeout(SMARTCONFIG_TIMEOUT_SEC));
	smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
	cfg.enable_log = false;
	session_id = ++s_sc_session_id;
	xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_SMARTCONFIG_BIT);

	ESP_LOGI(TAG, "smartconfig start, session=%lu, timeout=%us", (unsigned long)session_id, SMARTCONFIG_TIMEOUT_SEC);
	startup_ui_show_status("等待手机配网", "请打开配网 App 并发送 Wi-Fi 信息", 40);
	ESP_ERROR_CHECK(esp_smartconfig_start(&cfg));
	s_smartconfig_running = 1;
	s_sc_got_credentials = 0;

	while (1)
	{
		uxBits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_SMARTCONFIG_BIT, true, false, pdMS_TO_TICKS(1000));

		if (uxBits & WIFI_CONNECTED_BIT)
		{
			connected_seen = 1;
			connected_seconds = xTaskGetTickCount();
			ESP_LOGI(TAG, "WiFi Connected to ap, session=%lu", (unsigned long)session_id);
			startup_ui_show_status("网络连接成功", "正在等待手机确认配网结果", 92);
		}

		if (uxBits & WIFI_SMARTCONFIG_BIT) 
		{
			ESP_LOGI(TAG, "smartconfig over, session=%lu", (unsigned long)session_id);
			startup_ui_show_connected("Wi-Fi 配网完成");
			stop_smartconfig_with_log("ack completed", session_id);
			vTaskDelete(NULL);
		}

		if (connected_seen && (xTaskGetTickCount() - connected_seconds) >= pdMS_TO_TICKS(SMARTCONFIG_ACK_TIMEOUT_SEC * 1000))
		{
			ESP_LOGW(TAG, "smartconfig ack timeout, session=%lu, wifi already connected, stop smartconfig and keep connection", (unsigned long)session_id);
			startup_ui_show_connected("设备已联网，结束配网流程");
			stop_smartconfig_with_log("ack timeout after got ip", session_id);
			vTaskDelete(NULL);
		}
	}
}

/* 只有当前没有在跑 SmartConfig 时才启动，避免重复创建任务。 */
static void start_smartconfig_if_needed(void)
{
	if (s_smartconfig_running)
	{
		return;
	}

	ESP_LOGI(TAG, "start SmartConfig because no usable saved WiFi connection is available");
	startup_ui_show_status("进入配网模式", "正在监听手机发送的网络信息", 32);
	xTaskCreate(smartconfig_example_task, "smartconfig_example_task", 4096, NULL, 3, NULL);
}

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
	{
		ESP_LOGI(TAG, "event: WIFI_EVENT_STA_START");
		/* 开机优先尝试 NVS 中保存的配置；没有保存配置时再进入 SmartConfig。 */
		if (s_has_saved_wifi_config)
		{
			ESP_LOGI(TAG, "saved WiFi config found, connect first");
			startup_ui_show_status("连接已保存网络", "正在尝试连接上次成功的Wi-Fi", 34);
			esp_wifi_connect();
		}
		else
		{
			start_smartconfig_if_needed();
		}
	}
	else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
	{
		s_sta_disconnect_count++;
		s_wifi_reconnect_retry++;
		is_wifi_connected = 0;
		xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
		ESP_LOGW(TAG, "event: WIFI_EVENT_STA_DISCONNECTED, count=%lu, smartconfig_running=%d, got_credentials=%d",
			(unsigned long)s_sta_disconnect_count, s_smartconfig_running, s_sc_got_credentials);
		if (!s_smartconfig_running || s_sc_got_credentials)
		{
			/* 正常联网阶段优先重连；连续失败过多再退回 SmartConfig。 */
			if (s_wifi_reconnect_retry <= WIFI_RECONNECT_MAX_RETRY)
			{
				esp_wifi_connect();
				ESP_LOGI(TAG, "retry to connect to the AP... retry=%lu/%d",
					(unsigned long)s_wifi_reconnect_retry, WIFI_RECONNECT_MAX_RETRY);
				startup_ui_show_status("重连网络中", "正在尝试恢复 Wi-Fi 连接", 56);
			}
			else
			{
				ESP_LOGW(TAG, "saved WiFi connect failed too many times, fallback to SmartConfig");
				startup_ui_show_status("切换到配网模式", "已保存网络不可用，准备重新配网", 28);
				s_has_saved_wifi_config = 0;
				s_wifi_reconnect_retry = 0;
				start_smartconfig_if_needed();
			}
		}
		else
		{
			ESP_LOGI(TAG, "waiting for smartconfig credentials, keep current session alive");
		}
	}
	else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
	{
		ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
		ESP_LOGI(TAG, "event: IP_EVENT_STA_GOT_IP, ip=%s", ip4addr_ntoa(&event->ip_info.ip));
		xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
		is_wifi_connected = 1;
		s_wifi_reconnect_retry = 0;
		startup_ui_show_connected(ip4addr_ntoa(&event->ip_info.ip));
	}
	else if (event_base == SC_EVENT)
	{
		ESP_LOGI(TAG, "event: SC_EVENT_%s", sc_event_to_name(event_id));
		if (event_id == SC_EVENT_SCAN_DONE)
		{
			ESP_LOGI(TAG, "smartconfig scan finished, session=%lu", (unsigned long)s_sc_session_id);
			startup_ui_show_status("扫描附近网络", "正在搜索目标路由器与工作信道", 52);
		}
		else if (event_id == SC_EVENT_FOUND_CHANNEL)
		{
			ESP_LOGI(TAG, "smartconfig found channel, session=%lu", (unsigned long)s_sc_session_id);
			startup_ui_show_status("锁定目标信道", "已找到手机发送的配网信道", 66);
		}
		else if (event_id == SC_EVENT_GOT_SSID_PSWD)
		{
			ESP_LOGI(TAG, "smartconfig got ssid/password, session=%lu", (unsigned long)s_sc_session_id);
			startup_ui_show_status("收到网络信息", "正在保存凭据并连接路由器", 82);

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
			ESP_LOGI(TAG, "bssid_set=%d, smartconfig_type=%d", evt->bssid_set, evt->type);
			ESP_LOGI(TAG, "phone_ip=%d.%d.%d.%d, token=%u",
				evt->cellphone_ip[0], evt->cellphone_ip[1], evt->cellphone_ip[2], evt->cellphone_ip[3], evt->token);
			if (evt->type == SC_TYPE_ESPTOUCH_V2)
			{
				ESP_ERROR_CHECK( esp_smartconfig_get_rvd_data(rvd_data, sizeof(rvd_data)) );
				ESP_LOGI(TAG, "RVD_DATA:%s", rvd_data);
			}

			/* 先保存新凭据，再切换到新的 AP 配置。 */
			s_sc_got_credentials = 1;
			s_has_saved_wifi_config = 1;
			s_wifi_reconnect_retry = 0;
			ESP_ERROR_CHECK(save_wifi_credentials_to_nvs((const char *)ssid, (const char *)password));
			ESP_LOGI(TAG, "apply smartconfig credentials and reconnect");
			ESP_ERROR_CHECK(esp_wifi_disconnect());
			ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
			ESP_ERROR_CHECK(esp_wifi_connect());
		}
		else if (event_id == SC_EVENT_SEND_ACK_DONE)
		{
			ESP_LOGI(TAG, "smartconfig ack done, session=%lu", (unsigned long)s_sc_session_id);
			xEventGroupSetBits(s_wifi_event_group, WIFI_SMARTCONFIG_BIT);
		}
	}
}

void wifi_init_sta(char *ssid, char *passwd)
{
	wifi_config_t wifi_config = {0};
	char nvs_ssid[33] = { 0 };
	char nvs_password[65] = { 0 };

	is_wifi_connected = 0;
	s_has_saved_wifi_config = 0;
	s_wifi_reconnect_retry = 0;
	s_wifi_event_group = xEventGroupCreate();
	startup_ui_show_status("初始化网络模块", "正在准备 Wi-Fi 与事件系统", 14);

	tcpip_adapter_init();

	ESP_ERROR_CHECK(esp_event_loop_create_default());

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));
	ESP_ERROR_CHECK(esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));

	/* 启动时优先使用 NVS 里已保存的配网信息；读不到时退回编译期默认参数。 */
	if (load_wifi_credentials_from_nvs(nvs_ssid, sizeof(nvs_ssid), nvs_password, sizeof(nvs_password)) == ESP_OK)
	{
		strncpy((char *)wifi_config.sta.ssid, nvs_ssid, sizeof(wifi_config.sta.ssid));
		strncpy((char *)wifi_config.sta.password, nvs_password, sizeof(wifi_config.sta.password));
		s_has_saved_wifi_config = 1;
	}
	else if (ssid != NULL && passwd != NULL && strlen(ssid) > 0)
	{
		strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
		strncpy((char *)wifi_config.sta.password, passwd, sizeof(wifi_config.sta.password));
		s_has_saved_wifi_config = 1;
		ESP_LOGI(TAG, "using built-in WiFi credentials as fallback, ssid=%s", ssid);
		startup_ui_show_status("使用默认网络", "尝试连接内置默认网络", 24);
	}

	if (strlen((char *)wifi_config.sta.password))
	{
		wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
	}

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	if (s_has_saved_wifi_config)
	{
		ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
	}
	ESP_ERROR_CHECK(esp_wifi_start());
}
