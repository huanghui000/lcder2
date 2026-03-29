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
#include "tcpip_adapter.h"
#include "startup_ui.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "wifi station";

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT      BIT0
#define WIFI_SMARTCONFIG_BIT    BIT2

#define SMARTCONFIG_TIMEOUT_SEC     45
#define SMARTCONFIG_ACK_TIMEOUT_SEC 15
#define WIFI_RECONNECT_MAX_RETRY    10
#define WIFI_SWITCH_TIMEOUT_MS      15000

char is_wifi_connected;
static char s_smartconfig_running;
static char s_sc_got_credentials;
static char s_has_saved_wifi_config;
static char s_manual_switch_running;
static uint32_t s_sc_session_id;
static uint32_t s_sta_disconnect_count;
static uint32_t s_wifi_reconnect_retry;
static char s_active_ssid[33];
static char s_active_password[65];

static void copy_credentials(char *dst, size_t dst_len, const uint8_t *src, size_t src_len)
{
    size_t copy_len;

    if (dst == NULL || dst_len == 0)
    {
        return;
    }

    copy_len = src_len;
    if (copy_len >= dst_len)
    {
        copy_len = dst_len - 1;
    }

    memcpy(dst, src, copy_len);
    dst[copy_len] = 0;
}

static void remember_active_credentials(const wifi_config_t *wifi_config)
{
    if (wifi_config == NULL)
    {
        return;
    }

    copy_credentials(s_active_ssid, sizeof(s_active_ssid),
        wifi_config->sta.ssid, sizeof(wifi_config->sta.ssid));
    copy_credentials(s_active_password, sizeof(s_active_password),
        wifi_config->sta.password, sizeof(wifi_config->sta.password));
}

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
        startup_ui_show_status("Load WiFi", "Using saved WiFi credentials", 26);
    }
    else
    {
        ESP_LOGW(TAG, "failed to load wifi credentials from NVS: %s", esp_err_to_name(err));
        startup_ui_show_status("Wait WiFi", "No saved WiFi credentials", 18);
    }

    return err;
}

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

static void smartconfig_example_task(void *parm)
{
    EventBits_t uxBits;
    uint32_t session_id = 0;
    uint32_t connected_ticks = 0;
    char connected_seen = 0;

    ESP_ERROR_CHECK(esp_smartconfig_set_type(SC_TYPE_ESPTOUCH));
    ESP_ERROR_CHECK(esp_esptouch_set_timeout(SMARTCONFIG_TIMEOUT_SEC));

    smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    cfg.enable_log = false;
    session_id = ++s_sc_session_id;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_SMARTCONFIG_BIT);

    ESP_LOGI(TAG, "smartconfig start, session=%lu, timeout=%us",
        (unsigned long)session_id, SMARTCONFIG_TIMEOUT_SEC);
    startup_ui_show_status("SmartConfig", "Waiting phone to send WiFi info", 40);
    ESP_ERROR_CHECK(esp_smartconfig_start(&cfg));
    s_smartconfig_running = 1;
    s_sc_got_credentials = 0;

    while (1)
    {
        uxBits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_SMARTCONFIG_BIT,
            true, false, pdMS_TO_TICKS(1000));

        if (uxBits & WIFI_CONNECTED_BIT)
        {
            connected_seen = 1;
            connected_ticks = xTaskGetTickCount();
            ESP_LOGI(TAG, "WiFi connected to AP, session=%lu", (unsigned long)session_id);
            startup_ui_show_status("WiFi OK", "Waiting phone confirmation", 92);
        }

        if (uxBits & WIFI_SMARTCONFIG_BIT)
        {
            ESP_LOGI(TAG, "smartconfig over, session=%lu", (unsigned long)session_id);
            startup_ui_show_connected("WiFi provisioned");
            stop_smartconfig_with_log("ack completed", session_id);
            vTaskDelete(NULL);
        }

        if (connected_seen &&
            (xTaskGetTickCount() - connected_ticks) >= pdMS_TO_TICKS(SMARTCONFIG_ACK_TIMEOUT_SEC * 1000))
        {
            ESP_LOGW(TAG, "smartconfig ack timeout, session=%lu", (unsigned long)session_id);
            startup_ui_show_connected("WiFi connected");
            stop_smartconfig_with_log("ack timeout after got ip", session_id);
            vTaskDelete(NULL);
        }
    }
}

static void start_smartconfig_if_needed(void)
{
    if (s_smartconfig_running || s_manual_switch_running)
    {
        return;
    }

    ESP_LOGI(TAG, "start SmartConfig because no usable saved WiFi connection is available");
    startup_ui_show_status("Config WiFi", "Listening for SmartConfig", 32);
    xTaskCreate(smartconfig_example_task, "smartconfig_example_task", 3072, NULL, 3, NULL);
}

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        ESP_LOGI(TAG, "event: WIFI_EVENT_STA_START");
        if (s_has_saved_wifi_config)
        {
            ESP_LOGI(TAG, "saved WiFi config found, connect first");
            startup_ui_show_status("Connect WiFi", "Trying saved WiFi", 34);
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

        if (s_manual_switch_running)
        {
            esp_wifi_connect();
            ESP_LOGI(TAG, "manual switch reconnecting to candidate AP");
            return;
        }

        if (!s_smartconfig_running || s_sc_got_credentials)
        {
            if (s_wifi_reconnect_retry <= WIFI_RECONNECT_MAX_RETRY)
            {
                esp_wifi_connect();
                ESP_LOGI(TAG, "retry to connect to the AP... retry=%lu/%d",
                    (unsigned long)s_wifi_reconnect_retry, WIFI_RECONNECT_MAX_RETRY);
                startup_ui_show_status("Reconnect WiFi", "Trying to restore WiFi", 56);
            }
            else
            {
                ESP_LOGW(TAG, "saved WiFi connect failed too many times, fallback to SmartConfig");
                startup_ui_show_status("Config WiFi", "Saved WiFi is unavailable", 28);
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
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED)
    {
        wifi_event_sta_connected_t *event = (wifi_event_sta_connected_t *)event_data;
        ESP_LOGI(TAG, "event: WIFI_EVENT_STA_CONNECTED, ssid=%.*s, channel=%d",
            event->ssid_len, event->ssid, event->channel);
        startup_ui_show_status("WiFi Linked", "Waiting for DHCP address", 72);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "event: IP_EVENT_STA_GOT_IP, ip=%s", ip4addr_ntoa(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        is_wifi_connected = 1;
        s_wifi_reconnect_retry = 0;
        s_manual_switch_running = 0;
        startup_ui_show_connected(ip4addr_ntoa(&event->ip_info.ip));
    }
    else if (event_base == SC_EVENT)
    {
        ESP_LOGI(TAG, "event: SC_EVENT_%s", sc_event_to_name(event_id));
        if (event_id == SC_EVENT_SCAN_DONE)
        {
            startup_ui_show_status("Scan WiFi", "Searching nearby AP", 52);
        }
        else if (event_id == SC_EVENT_FOUND_CHANNEL)
        {
            startup_ui_show_status("Channel OK", "SmartConfig channel found", 66);
        }
        else if (event_id == SC_EVENT_GOT_SSID_PSWD)
        {
            smartconfig_event_got_ssid_pswd_t *evt = (smartconfig_event_got_ssid_pswd_t *)event_data;
            wifi_config_t wifi_config;
            uint8_t ssid[33] = {0};
            uint8_t password[65] = {0};
            uint8_t rvd_data[33] = {0};

            startup_ui_show_status("Got WiFi", "Saving and reconnecting", 82);
            memset(&wifi_config, 0, sizeof(wifi_config));
            memcpy(wifi_config.sta.ssid, evt->ssid, sizeof(wifi_config.sta.ssid));
            memcpy(wifi_config.sta.password, evt->password, sizeof(wifi_config.sta.password));
            wifi_config.sta.bssid_set = evt->bssid_set;

            if (wifi_config.sta.bssid_set)
            {
                memcpy(wifi_config.sta.bssid, evt->bssid, sizeof(wifi_config.sta.bssid));
            }

            memcpy(ssid, evt->ssid, sizeof(evt->ssid));
            memcpy(password, evt->password, sizeof(evt->password));
            if (evt->type == SC_TYPE_ESPTOUCH_V2)
            {
                ESP_ERROR_CHECK(esp_smartconfig_get_rvd_data(rvd_data, sizeof(rvd_data)));
                ESP_LOGI(TAG, "RVD_DATA:%s", rvd_data);
            }

            s_sc_got_credentials = 1;
            s_has_saved_wifi_config = 1;
            s_wifi_reconnect_retry = 0;
            copy_credentials(s_active_ssid, sizeof(s_active_ssid), ssid, sizeof(ssid));
            copy_credentials(s_active_password, sizeof(s_active_password), password, sizeof(password));
            ESP_ERROR_CHECK(save_wifi_credentials_to_nvs((const char *)ssid, (const char *)password));
            ESP_ERROR_CHECK(esp_wifi_disconnect());
            ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
            ESP_ERROR_CHECK(esp_wifi_connect());
        }
        else if (event_id == SC_EVENT_SEND_ACK_DONE)
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_SMARTCONFIG_BIT);
        }
    }
}

void wifi_init_sta(char *ssid, char *passwd)
{
    wifi_config_t wifi_config = {0};
    char nvs_ssid[33] = {0};
    char nvs_password[65] = {0};

    is_wifi_connected = 0;
    s_has_saved_wifi_config = 0;
    s_manual_switch_running = 0;
    s_wifi_reconnect_retry = 0;
    memset(s_active_ssid, 0, sizeof(s_active_ssid));
    memset(s_active_password, 0, sizeof(s_active_password));
    s_wifi_event_group = xEventGroupCreate();
    startup_ui_show_status("Init WiFi", "Preparing WiFi service", 14);

    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));

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
        startup_ui_show_status("Default WiFi", "Trying built-in WiFi", 24);
    }

    if (strlen((char *)wifi_config.sta.password) > 0)
    {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (s_has_saved_wifi_config)
    {
        remember_active_credentials(&wifi_config);
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
}

esp_err_t wifi_get_active_credentials(char *ssid, size_t ssid_len, char *passwd, size_t passwd_len)
{
    if (ssid != NULL && ssid_len > 0)
    {
        strncpy(ssid, s_active_ssid, ssid_len);
        ssid[ssid_len - 1] = 0;
    }

    if (passwd != NULL && passwd_len > 0)
    {
        strncpy(passwd, s_active_password, passwd_len);
        passwd[passwd_len - 1] = 0;
    }

    return (s_active_ssid[0] != 0) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t wifi_try_update_credentials(const char *ssid, const char *passwd, char *status_msg, size_t status_msg_len)
{
    wifi_config_t old_config = {0};
    wifi_config_t new_config = {0};
    EventBits_t bits;
    esp_err_t err;
    char old_ssid[33] = {0};

    if (status_msg != NULL && status_msg_len > 0)
    {
        status_msg[0] = 0;
    }

    if (ssid == NULL || ssid[0] == 0)
    {
        if (status_msg != NULL && status_msg_len > 0)
        {
            snprintf(status_msg, status_msg_len, "SSID is empty.");
        }
        return ESP_ERR_INVALID_ARG;
    }

    err = esp_wifi_get_config(ESP_IF_WIFI_STA, &old_config);
    if (err != ESP_OK)
    {
        if (status_msg != NULL && status_msg_len > 0)
        {
            snprintf(status_msg, status_msg_len, "Read current WiFi failed: %s", esp_err_to_name(err));
        }
        return err;
    }

    copy_credentials(old_ssid, sizeof(old_ssid), old_config.sta.ssid, sizeof(old_config.sta.ssid));
    strncpy((char *)new_config.sta.ssid, ssid, sizeof(new_config.sta.ssid));
    if (passwd != NULL)
    {
        strncpy((char *)new_config.sta.password, passwd, sizeof(new_config.sta.password));
    }
    if (strlen((char *)new_config.sta.password) > 0)
    {
        new_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }

    s_manual_switch_running = 1;
    s_wifi_reconnect_retry = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    esp_wifi_disconnect();

    err = esp_wifi_set_config(ESP_IF_WIFI_STA, &new_config);
    if (err == ESP_OK)
    {
        err = esp_wifi_connect();
    }

    if (err == ESP_OK)
    {
        bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
            pdFALSE, pdFALSE, pdMS_TO_TICKS(WIFI_SWITCH_TIMEOUT_MS));
        if (bits & WIFI_CONNECTED_BIT)
        {
            remember_active_credentials(&new_config);
            s_has_saved_wifi_config = 1;
            err = save_wifi_credentials_to_nvs((const char *)new_config.sta.ssid,
                (const char *)new_config.sta.password);
            if (err == ESP_OK)
            {
                if (status_msg != NULL && status_msg_len > 0)
                {
                    snprintf(status_msg, status_msg_len, "Switched to WiFi \"%s\" successfully.", ssid);
                }
                s_manual_switch_running = 0;
                return ESP_OK;
            }
        }
        else
        {
            err = ESP_ERR_TIMEOUT;
        }
    }

    ESP_LOGW(TAG, "switch to new WiFi failed, restoring previous credentials");
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    esp_wifi_disconnect();
    esp_wifi_set_config(ESP_IF_WIFI_STA, &old_config);
    remember_active_credentials(&old_config);
    s_wifi_reconnect_retry = 0;
    esp_wifi_connect();
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(WIFI_SWITCH_TIMEOUT_MS));
    s_manual_switch_running = 0;

    if (status_msg != NULL && status_msg_len > 0)
    {
        snprintf(status_msg, status_msg_len,
            "Connect to new WiFi \"%s\" failed (%s), restored previous SSID \"%s\".",
            ssid, esp_err_to_name(err), old_ssid);
    }

    return err;
}
