#include "http_seniverse.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <netdb.h>
#include <sys/socket.h>

#include <cJSON.h>

#include "ui.h"

extern SemaphoreHandle_t xLvglMutex;
extern char is_wifi_connected;

#define WEB_SERVER              "api.seniverse.com"
#define WEB_PORT                "80"
#define SENIVERSE_API_KEY       "SkEFlh-N-DtiJTcnf"
#define WEATHER_CFG_NAMESPACE   "weather_cfg"
#define DEFAULT_LOCATION_ID     "laishan"
#define DEFAULT_LOCATION_NAME   "Laishan"
#define HTTP_REQUEST_RETRY_COUNT 2
#define HTTP_REQUEST_RETRY_DELAY_MS 1000
#define HTTP_GET_TASK_STACK_SIZE 3072

static const char *TAG = "Http-GET";

static SemaphoreHandle_t s_weather_cfg_mutex;
static SemaphoreHandle_t s_http_task_mutex;
static TaskHandle_t s_http_task_handle;
static char s_refresh_pending;
static char s_location_id[64] = DEFAULT_LOCATION_ID;
static char s_location_name[64] = DEFAULT_LOCATION_NAME;

static const char weatherIconTbl[][3] =
{
    {0, 'A', 0}, {1, 'A', 0}, {2, 'A', 0}, {3, 'A', 0}, {4, 'B', 0},
    {5, 'B', 0}, {6, 'B', 0}, {7, 'B', 0}, {8, 'B', 0}, {9, 'C', 0},
    {10, 'D', 0}, {11, 'D', 0}, {12, 'E', 0}, {13, 'F', 0}, {14, 'G', 0},
    {15, 'H', 0}, {16, 'H', 0}, {17, 'H', 0}, {18, 'H', 0}, {19, 'I', 0},
    {20, 'I', 0}, {21, 'J', 0}, {22, 'J', 0}, {23, 'K', 0}, {24, 'L', 0},
    {25, 'L', 0}, {26, 'M', 0}, {27, 'M', 0}, {28, 'M', 0}, {29, 'M', 0},
    {30, 'N', 0}, {31, 'O', 0}, {32, 'P', 0}, {33, 'P', 0}, {34, 'P', 0},
    {35, 'P', 0}, {36, 'P', 0}, {37, 'Z', 0}, {38, 'A', 0}, {99, 'Z', 0},
};

static char *http_response_get_body(char *respond, size_t len)
{
    char *idx = respond;
    int lfCnt = 0;

    while (len > 0)
    {
        if (*idx == '\n')
        {
            lfCnt++;
        }
        else
        {
            lfCnt = 0;
        }

        idx++;
        len--;

        while (len > 0 && *idx == '\r')
        {
            idx++;
            len--;
        }

        if (lfCnt == 2)
        {
            break;
        }
    }

    return idx;
}

static const char *lookupWeatherCode(const char *code_day)
{
    int code;
    int i = 0;

    if (code_day == NULL || code_day[0] == 0)
    {
        return &weatherIconTbl[39][1];
    }

    code = atoi(code_day);
    while (code != weatherIconTbl[i][0])
    {
        if (i >= 39)
        {
            break;
        }
        i++;
    }

    return &weatherIconTbl[i][1];
}

static const char *json_string_or_placeholder(cJSON *item)
{
    if (item != NULL && cJSON_IsString(item) && item->valuestring != NULL && item->valuestring[0] != 0)
    {
        return item->valuestring;
    }

    return "--";
}

static const char *wind_speed_to_level(cJSON *wind_speed)
{
    static char level_buf[8];
    static const float level_thresholds_kmh[] = {
        1.0f, 6.0f, 12.0f, 20.0f, 29.0f, 39.0f, 50.0f,
        62.0f, 75.0f, 89.0f, 103.0f, 118.0f
    };
    char *endptr;
    float speed_kmh;
    int level = 12;
    size_t i;

    if (wind_speed == NULL || !cJSON_IsString(wind_speed) || wind_speed->valuestring == NULL || wind_speed->valuestring[0] == 0)
    {
        return "--";
    }

    speed_kmh = strtof(wind_speed->valuestring, &endptr);
    if (endptr == wind_speed->valuestring)
    {
        return "--";
    }

    for (i = 0; i < (sizeof(level_thresholds_kmh) / sizeof(level_thresholds_kmh[0])); ++i)
    {
        if (speed_kmh < level_thresholds_kmh[i])
        {
            level = (int)i;
            break;
        }
    }

    snprintf(level_buf, sizeof(level_buf), "%d级", level);
    return level_buf;
}

static esp_err_t load_weather_cfg_from_nvs(void)
{
    nvs_handle nvs_handle;
    size_t len;
    esp_err_t err;

    err = nvs_open(WEATHER_CFG_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK)
    {
        return err;
    }

    len = sizeof(s_location_id);
    err = nvs_get_str(nvs_handle, "location_id", s_location_id, &len);
    if (err == ESP_OK)
    {
        len = sizeof(s_location_name);
        err = nvs_get_str(nvs_handle, "location_name", s_location_name, &len);
    }

    nvs_close(nvs_handle);
    return err;
}

static esp_err_t save_weather_cfg_to_nvs(const char *location_id, const char *location_name)
{
    nvs_handle nvs_handle;
    esp_err_t err;

    err = nvs_open(WEATHER_CFG_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_set_str(nvs_handle, "location_id", location_id);
    if (err == ESP_OK)
    {
        err = nvs_set_str(nvs_handle, "location_name", location_name);
    }
    if (err == ESP_OK)
    {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    return err;
}

static void copy_location_locked(char *id, size_t id_len, char *name, size_t name_len)
{
    if (id != NULL && id_len > 0)
    {
        strncpy(id, s_location_id, id_len);
        id[id_len - 1] = 0;
    }

    if (name != NULL && name_len > 0)
    {
        strncpy(name, s_location_name, name_len);
        name[name_len - 1] = 0;
    }
}

static void set_location_locked(const char *id, const char *name)
{
    strncpy(s_location_id, id, sizeof(s_location_id));
    s_location_id[sizeof(s_location_id) - 1] = 0;
    strncpy(s_location_name, name, sizeof(s_location_name));
    s_location_name[sizeof(s_location_name) - 1] = 0;
}

static void url_encode(const char *src, char *dst, size_t dst_len)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t di = 0;

    while (*src && di + 1 < dst_len)
    {
        unsigned char c = (unsigned char)*src++;

        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
        {
            dst[di++] = (char)c;
        }
        else if (di + 3 < dst_len)
        {
            dst[di++] = '%';
            dst[di++] = hex[(c >> 4) & 0x0F];
            dst[di++] = hex[c & 0x0F];
        }
        else
        {
            break;
        }
    }

    dst[di] = 0;
}

int http_get_response(char *buf, int buf_len, const char *server, const char *req)
{
    static const struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    static const struct timeval rcv_to = {5, 0};
    struct addrinfo *res = NULL;
    struct in_addr *addr;
    int s;
    int r;
    int err;
    int idx = 0;

    err = getaddrinfo(server, WEB_PORT, &hints, &res);
    if (err != 0 || res == NULL)
    {
        ESP_LOGE(TAG, "DNS lookup failed err=%d res=%p", err, res);
        return -1;
    }

    addr = &((struct sockaddr_in *)res->ai_addr)->sin_addr;
    ESP_LOGI(TAG, "DNS lookup succeeded. IP=%s", inet_ntoa(*addr));

    s = socket(res->ai_family, res->ai_socktype, 0);
    if (s < 0)
    {
        ESP_LOGE(TAG, "socket create failed errno=%d", errno);
        freeaddrinfo(res);
        return -2;
    }

    if (connect(s, res->ai_addr, res->ai_addrlen) != 0)
    {
        ESP_LOGE(TAG, "connect failed errno=%d", errno);
        close(s);
        freeaddrinfo(res);
        return -3;
    }

    freeaddrinfo(res);

    if (write(s, req, strlen(req)) < 0)
    {
        ESP_LOGE(TAG, "socket write failed errno=%d", errno);
        close(s);
        return -4;
    }

    if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rcv_to, sizeof(rcv_to)) < 0)
    {
        ESP_LOGE(TAG, "setsockopt SO_RCVTIMEO failed errno=%d", errno);
        close(s);
        return -5;
    }

    memset(buf, 0, buf_len);
    do
    {
        r = read(s, buf + idx, buf_len - 1 - idx);
        if (r > 0)
        {
            idx += r;
        }
    } while (r > 0 && idx < (buf_len - 1));

    close(s);

    if (r < 0)
    {
        ESP_LOGE(TAG, "socket read failed errno=%d", errno);
        return -6;
    }
    if (idx >= buf_len - 1)
    {
        return -7;
    }

    return 0;
}

static esp_err_t seniverse_request_json(const char *path, char *recv_buf, size_t recv_buf_len, cJSON **root_out)
{
    char *req;
    char *body;
    int err;
    int attempt;
    size_t req_len;

    req_len = strlen(path) + strlen(WEB_SERVER) + 48;
    req = malloc(req_len);
    if (req == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    snprintf(req, req_len,
        "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
        path, WEB_SERVER);

    err = -1;
    for (attempt = 1; attempt <= HTTP_REQUEST_RETRY_COUNT; ++attempt)
    {
        err = http_get_response(recv_buf, (int)recv_buf_len, WEB_SERVER, req);
        if (err == 0)
        {
            break;
        }

        if (attempt < HTTP_REQUEST_RETRY_COUNT)
        {
            ESP_LOGW(TAG, "HTTP request attempt %d/%d failed: %d, retry in %d ms, path=%s",
                attempt, HTTP_REQUEST_RETRY_COUNT, err, HTTP_REQUEST_RETRY_DELAY_MS, path);
            vTaskDelay(pdMS_TO_TICKS(HTTP_REQUEST_RETRY_DELAY_MS));
        }
    }

    free(req);
    if (err != 0)
    {
        ESP_LOGE(TAG, "HTTP request failed: %d, path=%s", err, path);
        return ESP_FAIL;
    }

    body = http_response_get_body(recv_buf, strlen(recv_buf));
    *root_out = cJSON_Parse(body);
    if (*root_out == NULL)
    {
        ESP_LOGE(TAG, "response is not valid JSON");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t seniverse_search_location(const char *query, char *location_id, size_t location_id_len,
    char *location_name, size_t location_name_len,
    char *location_detail, size_t location_detail_len)
{
    char encoded[192];
    char path[320];
    static char recv_buf[2048];
    cJSON *root = NULL;
    cJSON *results;
    cJSON *first;
    cJSON *id;
    cJSON *name;
    cJSON *path_item;
    cJSON *country;
    esp_err_t err;

    if (query == NULL || query[0] == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    url_encode(query, encoded, sizeof(encoded));
    snprintf(path, sizeof(path),
        "/v3/location/search.json?key=%s&q=%s&language=zh-Hans&limit=1",
        SENIVERSE_API_KEY, encoded);

    err = seniverse_request_json(path, recv_buf, sizeof(recv_buf), &root);
    if (err != ESP_OK)
    {
        return err;
    }

    results = cJSON_GetObjectItem(root, "results");
    first = (results && cJSON_IsArray(results) && cJSON_GetArraySize(results) > 0) ?
        cJSON_GetArrayItem(results, 0) : NULL;
    id = first ? cJSON_GetObjectItem(first, "id") : NULL;
    name = first ? cJSON_GetObjectItem(first, "name") : NULL;
    path_item = first ? cJSON_GetObjectItem(first, "path") : NULL;
    country = first ? cJSON_GetObjectItem(first, "country") : NULL;

    if (id == NULL || name == NULL || !cJSON_IsString(id) || !cJSON_IsString(name))
    {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    strncpy(location_id, id->valuestring, location_id_len);
    location_id[location_id_len - 1] = 0;
    strncpy(location_name, name->valuestring, location_name_len);
    location_name[location_name_len - 1] = 0;

    if (location_detail != NULL && location_detail_len > 0)
    {
        if (path_item && cJSON_IsString(path_item))
        {
            snprintf(location_detail, location_detail_len, "path: %s\nid: %s",
                path_item->valuestring, id->valuestring);
        }
        else if (country && cJSON_IsString(country))
        {
            snprintf(location_detail, location_detail_len, "country: %s\nid: %s",
                country->valuestring, id->valuestring);
        }
        else
        {
            snprintf(location_detail, location_detail_len, "id: %s", id->valuestring);
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static void update_addr_if_present(cJSON *location)
{
    cJSON *name = location ? cJSON_GetObjectItem(location, "name") : NULL;

    if (name != NULL && cJSON_IsString(name))
    {
        lv_label_set_text(ui_LabelAddr, name->valuestring);
    }
}

static void update_daily_page(lv_obj_t *label_text, lv_obj_t *label_temp, lv_obj_t *label_wind_dir,
    lv_obj_t *label_wind_speed, lv_obj_t *label_icon, cJSON *day)
{
    cJSON *text_day = cJSON_GetObjectItem(day, "text_day");
    cJSON *low = cJSON_GetObjectItem(day, "low");
    cJSON *high = cJSON_GetObjectItem(day, "high");
    cJSON *code_day = cJSON_GetObjectItem(day, "code_day");
    cJSON *wind_direction = cJSON_GetObjectItem(day, "wind_direction");
    cJSON *wind_speed = cJSON_GetObjectItem(day, "wind_speed");

    lv_label_set_text(label_text, json_string_or_placeholder(text_day));
    if (low != NULL && high != NULL && cJSON_IsString(low) && cJSON_IsString(high))
    {
        lv_label_set_text_fmt(label_temp, "%s/%s\xE2\x84\x83", low->valuestring, high->valuestring);
    }
    else
    {
        lv_label_set_text(label_temp, "--/--");
    }
    lv_label_set_text_fmt(label_wind_dir, "风向: %s", json_string_or_placeholder(wind_direction));
    lv_label_set_text_fmt(label_wind_speed, "风速: %s", wind_speed_to_level(wind_speed));
    if (code_day != NULL && cJSON_IsString(code_day))
    {
        lv_label_set_text(label_icon, lookupWeatherCode(code_day->valuestring));
    }
}

static esp_err_t fetch_daily_weather(const char *location_id, char *recv_buf, size_t recv_buf_len)
{
    char path[320];
    cJSON *root = NULL;
    cJSON *results;
    cJSON *result0;
    cJSON *location;
    cJSON *daily;
    cJSON *day0;
    cJSON *day1;
    esp_err_t err;

    snprintf(path, sizeof(path),
        "/v3/weather/daily.json?key=%s&location=%s&language=zh-Hans&unit=c&start=0&days=2",
        SENIVERSE_API_KEY, location_id);

    err = seniverse_request_json(path, recv_buf, recv_buf_len, &root);
    if (err != ESP_OK)
    {
        return err;
    }

    results = cJSON_GetObjectItem(root, "results");
    result0 = (results && cJSON_IsArray(results) && cJSON_GetArraySize(results) > 0) ?
        cJSON_GetArrayItem(results, 0) : NULL;
    location = result0 ? cJSON_GetObjectItem(result0, "location") : NULL;
    daily = result0 ? cJSON_GetObjectItem(result0, "daily") : NULL;
    day0 = (daily && cJSON_IsArray(daily) && cJSON_GetArraySize(daily) > 0) ? cJSON_GetArrayItem(daily, 0) : NULL;
    day1 = (daily && cJSON_IsArray(daily) && cJSON_GetArraySize(daily) > 1) ? cJSON_GetArrayItem(daily, 1) : NULL;

    xSemaphoreTake(xLvglMutex, portMAX_DELAY);
    update_addr_if_present(location);
    if (day0 != NULL)
    {
        update_daily_page(ui_LabelTodayText, ui_LabelTodayTemp, ui_LabelTodayWindDir,
            ui_LabelTodayWindSpeed, ui_LabelTodayIcon, day0);
    }
    if (day1 != NULL)
    {
        update_daily_page(ui_LabelTomorrowText, ui_LabelTomorrowTemp, ui_LabelTomorrowWindDir,
            ui_LabelTomorrowWindSpeed, ui_LabelTomorrowIcon, day1);
    }
    xSemaphoreGive(xLvglMutex);

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t fetch_now_weather(const char *location_id, char *recv_buf, size_t recv_buf_len)
{
    char path[320];
    cJSON *root = NULL;
    cJSON *results;
    cJSON *result0;
    cJSON *location;
    cJSON *now;
    esp_err_t err;

    snprintf(path, sizeof(path),
        "/v3/weather/now.json?key=%s&location=%s&language=zh-Hans&unit=c",
        SENIVERSE_API_KEY, location_id);

    err = seniverse_request_json(path, recv_buf, recv_buf_len, &root);
    if (err != ESP_OK)
    {
        return err;
    }

    results = cJSON_GetObjectItem(root, "results");
    result0 = (results && cJSON_IsArray(results) && cJSON_GetArraySize(results) > 0) ?
        cJSON_GetArrayItem(results, 0) : NULL;
    location = result0 ? cJSON_GetObjectItem(result0, "location") : NULL;
    now = result0 ? cJSON_GetObjectItem(result0, "now") : NULL;

    if (location != NULL && now != NULL)
    {
        cJSON *text = cJSON_GetObjectItem(now, "text");
        cJSON *code = cJSON_GetObjectItem(now, "code");
        cJSON *temperature = cJSON_GetObjectItem(now, "temperature");
        cJSON *wind_speed = cJSON_GetObjectItem(now, "wind_speed");
        cJSON *wind_direction = cJSON_GetObjectItem(now, "wind_direction");

        xSemaphoreTake(xLvglMutex, portMAX_DELAY);
        update_addr_if_present(location);
        lv_label_set_text(ui_LabelNowText, json_string_or_placeholder(text));
        if (temperature != NULL && cJSON_IsString(temperature))
        {
            lv_label_set_text_fmt(ui_LabelNowTemp, "%s\xE2\x84\x83", temperature->valuestring);
        }
        else
        {
            lv_label_set_text(ui_LabelNowTemp, "--");
        }
        lv_label_set_text_fmt(ui_LabelNowWindDir, "风向: %s", json_string_or_placeholder(wind_direction));
        lv_label_set_text_fmt(ui_LabelNowWindSpeed, "风速: %s", wind_speed_to_level(wind_speed));
        if (code != NULL && cJSON_IsString(code))
        {
            lv_label_set_text(ui_LabelNowIcon, lookupWeatherCode(code->valuestring));
        }
        xSemaphoreGive(xLvglMutex);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

void http_seniverse_init(void)
{
    s_weather_cfg_mutex = xSemaphoreCreateMutex();
    s_http_task_mutex = xSemaphoreCreateMutex();

    if (load_weather_cfg_from_nvs() == ESP_OK)
    {
        ESP_LOGI(TAG, "loaded weather location from NVS: %s (%s)", s_location_name, s_location_id);
    }
    else
    {
        ESP_LOGI(TAG, "using default weather location: %s (%s)", s_location_name, s_location_id);
    }
}

esp_err_t http_seniverse_get_location(char *id, size_t id_len, char *name, size_t name_len)
{
    if (s_weather_cfg_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_weather_cfg_mutex, portMAX_DELAY);
    copy_location_locked(id, id_len, name, name_len);
    xSemaphoreGive(s_weather_cfg_mutex);
    return ESP_OK;
}

esp_err_t http_seniverse_set_location_by_query(const char *query,
    char *resolved_name, size_t resolved_name_len,
    char *resolved_detail, size_t resolved_detail_len)
{
    char new_id[64];
    char new_name[64];
    char new_detail[160];
    esp_err_t err;

    err = seniverse_search_location(query, new_id, sizeof(new_id), new_name, sizeof(new_name),
        new_detail, sizeof(new_detail));
    if (err != ESP_OK)
    {
        return err;
    }

    xSemaphoreTake(s_weather_cfg_mutex, portMAX_DELAY);
    set_location_locked(new_id, new_name);
    xSemaphoreGive(s_weather_cfg_mutex);

    save_weather_cfg_to_nvs(new_id, new_name);

    if (resolved_name != NULL && resolved_name_len > 0)
    {
        strncpy(resolved_name, new_name, resolved_name_len);
        resolved_name[resolved_name_len - 1] = 0;
    }

    if (resolved_detail != NULL && resolved_detail_len > 0)
    {
        strncpy(resolved_detail, new_detail, resolved_detail_len);
        resolved_detail[resolved_detail_len - 1] = 0;
    }

    http_seniverse_request_refresh();
    return ESP_OK;
}

void http_seniverse_request_refresh(void)
{
    BaseType_t task_ok;

    if (is_wifi_connected == 0)
    {
        ESP_LOGW(TAG, "skip weather refresh request because WiFi is not connected yet");
        return;
    }

    if (s_http_task_mutex == NULL)
    {
        ESP_LOGW(TAG, "skip weather refresh request because HTTP task mutex is not ready");
        return;
    }

    xSemaphoreTake(s_http_task_mutex, portMAX_DELAY);
    if (s_http_task_handle != NULL)
    {
        s_refresh_pending = 1;
        xSemaphoreGive(s_http_task_mutex);
        ESP_LOGI(TAG, "weather refresh already running, mark one more refresh pending");
        return;
    }

    s_refresh_pending = 0;
    task_ok = xTaskCreate(http_get_task, "http_get_task", HTTP_GET_TASK_STACK_SIZE, NULL, 5, &s_http_task_handle);
    xSemaphoreGive(s_http_task_mutex);

    if (task_ok == pdPASS)
    {
        ESP_LOGI(TAG, "spawn weather refresh task");
    }
    else
    {
        xSemaphoreTake(s_http_task_mutex, portMAX_DELAY);
        s_http_task_handle = NULL;
        xSemaphoreGive(s_http_task_mutex);
        ESP_LOGE(TAG, "failed to create weather refresh task");
    }
}

void http_get_task(void *pvParameters)
{
    char location_id[64];
    char location_name[64];
    static char recv_buf[2048];

    (void)pvParameters;

    while (1)
    {
        char run_again = 0;

        ESP_LOGI(TAG, "weather refresh task running, heap=%u stack_hwm=%u",
            (unsigned)esp_get_free_heap_size(),
            (unsigned)uxTaskGetStackHighWaterMark(NULL));

        if (is_wifi_connected == 0)
        {
            ESP_LOGW(TAG, "drop weather refresh because WiFi is disconnected");
            break;
        }

        xSemaphoreTake(s_weather_cfg_mutex, portMAX_DELAY);
        copy_location_locked(location_id, sizeof(location_id), location_name, sizeof(location_name));
        xSemaphoreGive(s_weather_cfg_mutex);

        ESP_LOGI(TAG, "Getting weather for %s (%s)", location_name, location_id);

        fetch_daily_weather(location_id, recv_buf, sizeof(recv_buf));
        fetch_now_weather(location_id, recv_buf, sizeof(recv_buf));

        xSemaphoreTake(s_http_task_mutex, portMAX_DELAY);
        run_again = s_refresh_pending;
        s_refresh_pending = 0;
        if (!run_again)
        {
            s_http_task_handle = NULL;
        }
        xSemaphoreGive(s_http_task_mutex);

        if (!run_again)
        {
            ESP_LOGI(TAG, "weather refresh task finished");
            break;
        }

        ESP_LOGI(TAG, "run one more pending weather refresh");
    }

    if (s_http_task_mutex != NULL)
    {
        xSemaphoreTake(s_http_task_mutex, portMAX_DELAY);
        s_http_task_handle = NULL;
        s_refresh_pending = 0;
        xSemaphoreGive(s_http_task_mutex);
    }

    vTaskDelete(NULL);
}
