#include "config_portal.h"

#include "app_control.h"
#include "http_seniverse.h"
#include "wifi_sta.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "tcpip_adapter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "cfg_portal";

typedef struct
{
    SemaphoreHandle_t mutex;
    SemaphoreHandle_t done;
    char busy;
    char query[96];
    char resolved_name[96];
    char resolved_detail[192];
    esp_err_t result;
} portal_city_lookup_t;

static httpd_handle_t s_server;
static SemaphoreHandle_t s_portal_mutex;
static char s_status_msg[384] = "就绪。";
static char s_html_buf[3200];
static portal_city_lookup_t s_city_lookup;

static void portal_set_status(const char *msg)
{
    if (s_portal_mutex == NULL || msg == NULL)
    {
        return;
    }

    xSemaphoreTake(s_portal_mutex, portMAX_DELAY);
    strncpy(s_status_msg, msg, sizeof(s_status_msg));
    s_status_msg[sizeof(s_status_msg) - 1] = 0;
    xSemaphoreGive(s_portal_mutex);
}

static void portal_get_status(char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0 || s_portal_mutex == NULL)
    {
        return;
    }

    xSemaphoreTake(s_portal_mutex, portMAX_DELAY);
    strncpy(buf, s_status_msg, buf_len);
    buf[buf_len - 1] = 0;
    xSemaphoreGive(s_portal_mutex);
}

static esp_err_t portal_redirect_home(httpd_req_t *req)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

static void url_decode(const char *src, char *dst, size_t dst_len)
{
    size_t di = 0;

    if (src == NULL || dst == NULL || dst_len == 0)
    {
        return;
    }

    while (*src && di + 1 < dst_len)
    {
        if (*src == '+')
        {
            dst[di++] = ' ';
            src++;
        }
        else if (*src == '%' &&
                 ((src[1] >= '0' && src[1] <= '9') || (src[1] >= 'A' && src[1] <= 'F') || (src[1] >= 'a' && src[1] <= 'f')) &&
                 ((src[2] >= '0' && src[2] <= '9') || (src[2] >= 'A' && src[2] <= 'F') || (src[2] >= 'a' && src[2] <= 'f')))
        {
            char hex[3];
            hex[0] = src[1];
            hex[1] = src[2];
            hex[2] = 0;
            dst[di++] = (char)strtol(hex, NULL, 16);
            src += 3;
        }
        else
        {
            dst[di++] = *src++;
        }
    }

    dst[di] = 0;
}

static void parse_form_value(const char *body, const char *key, char *out, size_t out_len)
{
    const char *pos;
    const char *start;
    size_t len = 0;
    char raw[192];

    if (body == NULL || key == NULL || out == NULL || out_len == 0)
    {
        return;
    }

    out[0] = 0;
    pos = strstr(body, key);
    if (pos == NULL)
    {
        return;
    }

    if (pos != body && pos[-1] != '&')
    {
        return;
    }

    start = pos + strlen(key);
    if (*start != '=')
    {
        return;
    }
    start++;

    while (start[len] != 0 && start[len] != '&' && len + 1 < sizeof(raw))
    {
        raw[len] = start[len];
        len++;
    }
    raw[len] = 0;
    url_decode(raw, out, out_len);
}

static esp_err_t portal_recv_body(httpd_req_t *req, char **body_out)
{
    char *body;
    int remaining;
    int offset = 0;

    if (req == NULL || body_out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    body = malloc(req->content_len + 1);
    if (body == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    remaining = req->content_len;
    while (remaining > 0 && offset < req->content_len)
    {
        int ret = httpd_req_recv(req, body + offset, remaining);
        if (ret <= 0)
        {
            free(body);
            return ESP_FAIL;
        }
        offset += ret;
        remaining -= ret;
    }

    body[offset] = 0;
    *body_out = body;
    return ESP_OK;
}

void config_portal_get_url(char *buf, size_t buf_len)
{
    tcpip_adapter_ip_info_t ip_info;

    if (buf == NULL || buf_len == 0)
    {
        return;
    }

    if (tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip_info) == ESP_OK && ip_info.ip.addr != 0)
    {
        snprintf(buf, buf_len, "http://%s/", ip4addr_ntoa(&ip_info.ip));
    }
    else
    {
        snprintf(buf, buf_len, "http://0.0.0.0/");
    }
}

static esp_err_t portal_render_home(httpd_req_t *req)
{
    char location_id[64];
    char location_name[64];
    char current_ssid[64];
    char current_passwd[64];
    char status[384];
    char url[64];

    http_seniverse_get_location(location_id, sizeof(location_id), location_name, sizeof(location_name));
    wifi_get_active_credentials(current_ssid, sizeof(current_ssid), current_passwd, sizeof(current_passwd));
    portal_get_status(status, sizeof(status));
    config_portal_get_url(url, sizeof(url));

    ESP_LOGI(TAG, "render home, heap=%u stack_hwm=%u",
        (unsigned)esp_get_free_heap_size(),
        (unsigned)uxTaskGetStackHighWaterMark(NULL));

    snprintf(s_html_buf, sizeof(s_html_buf),
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>天气配置</title>"
        "<style>"
        "body{font-family:Arial,\"Microsoft YaHei\",sans-serif;max-width:760px;margin:0 auto;padding:18px;background:#f4f7fb;color:#1f2937;}"
        ".card{background:#fff;border-radius:14px;padding:16px 18px;box-shadow:0 8px 28px rgba(0,0,0,.08);margin-bottom:14px;}"
        "input{width:100%%;padding:10px 12px;margin:8px 0 12px;border:1px solid #cbd5e1;border-radius:10px;box-sizing:border-box;font-size:15px;}"
        "button{color:#fff;border:0;border-radius:10px;padding:12px 18px;font-size:15px;}"
        ".btn-city{background:#0284c7;}"
        ".btn-wifi{background:#0f766e;}"
        ".muted{color:#64748b;font-size:14px;line-height:1.6;}"
        "h2,h3{margin-top:0;}"
        ".status{white-space:pre-wrap;background:#ecfeff;border-radius:10px;padding:12px;}"
        "</style></head><body>"
        "<div class=\"card\"><h2>ESP8266 天气配置</h2>"
        "<div class=\"muted\">访问地址：%s</div>"
        "<div class=\"muted\">当前城市：%s (ID: %s)</div>"
        "<div class=\"muted\">当前 WiFi：%s</div></div>"
        "<div class=\"card\"><h3>城市设置</h3>"
        "<div class=\"muted\">支持输入中文或英文城市名，例如：烟台、Yantai、牟平。</div>"
        "<form method=\"post\" action=\"/config/city\">"
        "<label>城市名称</label><input name=\"city\" placeholder=\"例如：北京 或 Beijing\">"
        "<button class=\"btn-city\" type=\"submit\">更新城市</button></form></div>"
        "<div class=\"card\"><h3>WiFi 设置</h3>"
        "<div class=\"muted\">如果连接新 WiFi 失败，设备会自动切回原来的 WiFi。</div>"
        "<form method=\"post\" action=\"/config/wifi\">"
        "<label>WiFi 名称 (SSID)</label><input name=\"ssid\" placeholder=\"留空则不修改\">"
        "<label>WiFi 密码</label><input type=\"password\" name=\"password\" placeholder=\"留空则不修改\">"
        "<button class=\"btn-wifi\" type=\"submit\">更新 WiFi</button></form></div>"
        "<div class=\"card\"><h3>执行结果</h3><div class=\"status\">%s</div></div>"
        "</body></html>",
        url, location_name, location_id, current_ssid, status);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, s_html_buf, strlen(s_html_buf));
}

static esp_err_t portal_root_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /");
    return portal_render_home(req);
}

static esp_err_t portal_config_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /config");
    return portal_render_home(req);
}

static esp_err_t portal_ping_get_handler(httpd_req_t *req)
{
    char url[64];
    char body[128];

    config_portal_get_url(url, sizeof(url));
    ESP_LOGI(TAG, "GET /ping heap=%u stack_hwm=%u",
        (unsigned)esp_get_free_heap_size(),
        (unsigned)uxTaskGetStackHighWaterMark(NULL));

    snprintf(body, sizeof(body), "ok %s", url);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, body, strlen(body));
}

static esp_err_t portal_favicon_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /favicon.ico");
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static void portal_city_lookup_task(void *arg)
{
    char query[96];
    char resolved_name[96];
    char resolved_detail[192];
    esp_err_t result;

    (void)arg;

    xSemaphoreTake(s_city_lookup.mutex, portMAX_DELAY);
    strncpy(query, s_city_lookup.query, sizeof(query));
    query[sizeof(query) - 1] = 0;
    xSemaphoreGive(s_city_lookup.mutex);

    ESP_LOGI(TAG, "city lookup task start, query=%s, heap=%u stack_hwm=%u",
        query,
        (unsigned)esp_get_free_heap_size(),
        (unsigned)uxTaskGetStackHighWaterMark(NULL));

    result = http_seniverse_set_location_by_query(query,
        resolved_name, sizeof(resolved_name),
        resolved_detail, sizeof(resolved_detail));

    xSemaphoreTake(s_city_lookup.mutex, portMAX_DELAY);
    s_city_lookup.result = result;
    strncpy(s_city_lookup.resolved_name, resolved_name, sizeof(s_city_lookup.resolved_name));
    s_city_lookup.resolved_name[sizeof(s_city_lookup.resolved_name) - 1] = 0;
    strncpy(s_city_lookup.resolved_detail, resolved_detail, sizeof(s_city_lookup.resolved_detail));
    s_city_lookup.resolved_detail[sizeof(s_city_lookup.resolved_detail) - 1] = 0;
    s_city_lookup.busy = 0;
    xSemaphoreGive(s_city_lookup.mutex);

    xSemaphoreGive(s_city_lookup.done);
    vTaskDelete(NULL);
}

static esp_err_t portal_city_post_handler(httpd_req_t *req)
{
    char *body = NULL;
    char city[96];
    char final_status[384];
    BaseType_t task_ok;
    esp_err_t err;

    ESP_LOGI(TAG, "POST /config/city heap=%u stack_hwm=%u content_len=%d",
        (unsigned)esp_get_free_heap_size(),
        (unsigned)uxTaskGetStackHighWaterMark(NULL),
        req->content_len);

    err = portal_recv_body(req, &body);
    if (err != ESP_OK)
    {
        httpd_resp_send_500(req);
        return err;
    }

    parse_form_value(body, "city", city, sizeof(city));
    free(body);
    final_status[0] = 0;

    if (city[0] == 0)
    {
        portal_set_status("没有提交有效的城市名称。");
        return portal_redirect_home(req);
    }

    if (s_city_lookup.mutex == NULL || s_city_lookup.done == NULL)
    {
        httpd_resp_send_500(req);
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_city_lookup.mutex, portMAX_DELAY);
    if (s_city_lookup.busy)
    {
        xSemaphoreGive(s_city_lookup.mutex);
        portal_set_status("城市查询正在进行中，请稍后再试。");
        return portal_redirect_home(req);
    }

    while (xSemaphoreTake(s_city_lookup.done, 0) == pdTRUE)
    {
    }

    s_city_lookup.busy = 1;
    s_city_lookup.result = ESP_FAIL;
    s_city_lookup.resolved_name[0] = 0;
    s_city_lookup.resolved_detail[0] = 0;
    strncpy(s_city_lookup.query, city, sizeof(s_city_lookup.query));
    s_city_lookup.query[sizeof(s_city_lookup.query) - 1] = 0;
    xSemaphoreGive(s_city_lookup.mutex);

    task_ok = xTaskCreate(portal_city_lookup_task, "portal_city_lookup", 6144, NULL, 4, NULL);
    if (task_ok != pdPASS)
    {
        xSemaphoreTake(s_city_lookup.mutex, portMAX_DELAY);
        s_city_lookup.busy = 0;
        xSemaphoreGive(s_city_lookup.mutex);
        portal_set_status("城市查询任务启动失败，请稍后重试。");
        return portal_redirect_home(req);
    }

    if (xSemaphoreTake(s_city_lookup.done, pdMS_TO_TICKS(15000)) != pdTRUE)
    {
        portal_set_status("城市查询超时，请稍后重试。");
        return portal_redirect_home(req);
    }

    xSemaphoreTake(s_city_lookup.mutex, portMAX_DELAY);
    if (s_city_lookup.result == ESP_OK)
    {
        snprintf(final_status, sizeof(final_status),
            "城市已更新为：%s\n%s",
            s_city_lookup.resolved_name,
            s_city_lookup.resolved_detail);
        app_request_weather_screen();
    }
    else
    {
        snprintf(final_status, sizeof(final_status),
            "城市更新失败：%s",
            city);
    }
    xSemaphoreGive(s_city_lookup.mutex);

    portal_set_status(final_status);
    return portal_redirect_home(req);
}

static esp_err_t portal_wifi_post_handler(httpd_req_t *req)
{
    char *body = NULL;
    char ssid[64];
    char password[80];
    char wifi_result[160];
    esp_err_t err;

    ESP_LOGI(TAG, "POST /config/wifi heap=%u stack_hwm=%u content_len=%d",
        (unsigned)esp_get_free_heap_size(),
        (unsigned)uxTaskGetStackHighWaterMark(NULL),
        req->content_len);

    err = portal_recv_body(req, &body);
    if (err != ESP_OK)
    {
        httpd_resp_send_500(req);
        return err;
    }

    parse_form_value(body, "ssid", ssid, sizeof(ssid));
    parse_form_value(body, "password", password, sizeof(password));
    free(body);

    if (ssid[0] != 0)
    {
        err = wifi_try_update_credentials(ssid, password, wifi_result, sizeof(wifi_result));
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "wifi update failed");
        }
        portal_set_status(wifi_result);
    }
    else
    {
        portal_set_status("没有提交有效的 WiFi 名称。");
    }

    return portal_redirect_home(req);
}

esp_err_t config_portal_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = portal_root_get_handler,
        .user_ctx = NULL
    };
    httpd_uri_t config_get_uri = {
        .uri = "/config",
        .method = HTTP_GET,
        .handler = portal_config_get_handler,
        .user_ctx = NULL
    };
    httpd_uri_t city_post_uri = {
        .uri = "/config/city",
        .method = HTTP_POST,
        .handler = portal_city_post_handler,
        .user_ctx = NULL
    };
    httpd_uri_t wifi_post_uri = {
        .uri = "/config/wifi",
        .method = HTTP_POST,
        .handler = portal_wifi_post_handler,
        .user_ctx = NULL
    };
    httpd_uri_t ping_uri = {
        .uri = "/ping",
        .method = HTTP_GET,
        .handler = portal_ping_get_handler,
        .user_ctx = NULL
    };
    httpd_uri_t favicon_uri = {
        .uri = "/favicon.ico",
        .method = HTTP_GET,
        .handler = portal_favicon_get_handler,
        .user_ctx = NULL
    };

    if (s_server != NULL)
    {
        return ESP_OK;
    }

    config.stack_size = 4096;
    config.max_open_sockets = 3;
    config.max_uri_handlers = 6;
    config.max_resp_headers = 2;
    config.backlog_conn = 1;
    config.lru_purge_enable = true;

    if (s_portal_mutex == NULL)
    {
        s_portal_mutex = xSemaphoreCreateMutex();
        if (s_portal_mutex == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_city_lookup.mutex == NULL)
    {
        s_city_lookup.mutex = xSemaphoreCreateMutex();
        if (s_city_lookup.mutex == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_city_lookup.done == NULL)
    {
        s_city_lookup.done = xSemaphoreCreateBinary();
        if (s_city_lookup.done == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    portal_set_status("就绪。");

    ESP_LOGI(TAG, "starting config portal, heap=%u",
        (unsigned)esp_get_free_heap_size());

    if (httpd_start(&s_server, &config) == ESP_OK)
    {
        httpd_register_uri_handler(s_server, &root_uri);
        httpd_register_uri_handler(s_server, &config_get_uri);
        httpd_register_uri_handler(s_server, &city_post_uri);
        httpd_register_uri_handler(s_server, &wifi_post_uri);
        httpd_register_uri_handler(s_server, &ping_uri);
        httpd_register_uri_handler(s_server, &favicon_uri);
        ESP_LOGI(TAG, "config portal started");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "failed to start config portal");
    return ESP_FAIL;
}
