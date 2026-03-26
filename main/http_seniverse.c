#include "http_seniverse.h"

#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <netdb.h>
#include <sys/socket.h>

#include <cJSON.h>

#include "ui.h"

extern SemaphoreHandle_t xLvglMutex;

EventGroupHandle_t eg_http_get;

#define WEB_SERVER	"api.seniverse.com"
#define WEB_PORT	80
// #define WEB_URL		"http://api.seniverse.com/v3/weather/daily.json?key=rrpd2zmqkpwlsckt&location=laishan&language=zh-Hans&unit=c&start=0&days=3"
// #define WEB_URL		"https://api.seniverse.com/v3/weather/daily.json?key=SCRu02Zdatz4cWpDk&location=yantai&language=zh-Hans&unit=c&start=0&days=2"
#define WEB_URL		"http://api.seniverse.com/v3/weather/daily.json?key=smtq3n0ixdggurox&location=laishan&language=zh-Hans&unit=c&start=1&days=1"
#define WEB_URL1	"http://api.seniverse.com/v3/weather/now.json?key=smtq3n0ixdggurox&location=laishan&language=zh-Hans&unit=c"

static const char *REQUEST = "GET " WEB_URL "\r\n";
static const char *REQUEST1 = "GET " WEB_URL1 "\r\n";

// #define USE_SUNING   // quan.suning.com not working 20251116

#ifdef USE_SUNING
#define WEB_SERVER2	"quan.suning.com"
#define WEB_PORT2	80
#define WEB_URL2	"/getSysTime.do"

// static const char *REQUEST2 = "GET " WEB_URL2 " HTTP/1.1\r\nHost: quan.suning.com\r\nConnection: close\r\n\r\n";
static const char *REQUEST2 = "GET " WEB_URL2 " HTTP/1.1\r\nHost: quan.suning.com\r\n\r\n";
// #define WEB_URL2	"https://quan.suning.com/getSysTime.do"
// static const char *REQUEST2 = "GET " WEB_URL2 "\r\n";

#endif
static const char *TAG = "Http-GET";


static char * http_response_get_body(char *respond, size_t len)
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
		while (*idx == '\r' && len > 0)
		{
			idx++;
			len--;
		}
		if (lfCnt ==2)
		{
			break;
		}
	}
	return idx;
}

#ifdef USE_SUNING
extern int settimeofday(const struct timeval* tv, const struct timezone* tz);
#endif
static const char weatherIconTbl[][3] =
{
	{0,'A',0},
	{1,'A',0},
	{2,'A',0},
	{3,'A',0},
	{4,'B',0},
	{5,'B',0},
	{6,'B',0},
	{7,'B',0},
	{8,'B',0},
	{9,'C',0},
	{10,'D',0},
	{11,'D',0},
	{12,'E',0},
	{13,'F',0},
	{14,'G',0},
	{15,'H',0},
	{16,'H',0},
	{17,'H',0},
	{18,'H',0},
	{19,'I',0},
	{20,'I',0},
	{21,'J',0},
	{22,'J',0},
	{23,'K',0},
	{24,'L',0},
	{25,'L',0},
	{26,'M',0},
	{27,'M',0},
	{28,'M',0},
	{29,'M',0},
	{30,'N',0},
	{31,'O',0},
	{32,'P',0},
	{33,'P',0},
	{34,'P',0},
	{35,'P',0},
	{36,'P',0},
	{37,'Z',0},
	{38,'A',0},
	{99,'Z',0},
};

static const char * lookupWeatherCode(char * code_day)
{
	char code;
	int i;
	if(code_day[1] == 0)
	{
		code = code_day[0] - '0';
	}
	else
	{
		code = code_day[1]-'0' + (code_day[0]-'0')*10;
	}
	i = 0;
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

int http_get_response(char *buf, int buf_len, char *server, const char *req)
{
	const struct addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	struct addrinfo *res;
	struct in_addr *addr;

	int s, r, err;

	err = getaddrinfo(server, "80", &hints, &res);
	if (err != 0 || res == NULL)
	{
		ESP_LOGE(TAG, "DNS lookup failed err=%d res=%p", err, res);
		return -1;
	}
	/* Code to print the resolved IP.
	Note: inet_ntoa is non-reentrant, look at ipaddr_ntoa_r for "real" code */
	addr = &((struct sockaddr_in *)res->ai_addr)->sin_addr;
	ESP_LOGI(TAG, "DNS lookup succeeded. IP=%s", inet_ntoa(*addr));

	// Create socket and connect
	s = socket(res->ai_family, res->ai_socktype, 0);
	if (s < 0)
	{
		ESP_LOGE(TAG, "... Failed to allocate socket.");
		freeaddrinfo(res);
		return -2;
	}
	ESP_LOGI(TAG, "... allocated socket");

	if (connect(s, res->ai_addr, res->ai_addrlen) != 0)
	{
		ESP_LOGE(TAG, "... socket connect failed errno=%d", errno);
		close(s);
		freeaddrinfo(res);
		return -3;
	}
	ESP_LOGI(TAG, "... connected");
	freeaddrinfo(res);

	ESP_LOGI(TAG, "REQ : %s", req);
	// Get Command
	if (write(s, req, strlen(req)) < 0)
	{
		ESP_LOGE(TAG, "... socket send failed");
		close(s);
		return -4;
	}
	ESP_LOGI(TAG, "... socket send success");

	struct timeval rcv_to = {5, 0}; // timeout value : 5 seconds
	if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rcv_to, sizeof(rcv_to)) < 0)
	{
		ESP_LOGE(TAG, "... failed to set socket receiving timeout");
		close(s);
		return -5;
	}
	ESP_LOGI(TAG, "... set socket receiving timeout success");

	// Read HTTP response
	bzero(buf, buf_len);
	int idx = 0;
	do
	{
		r = read(s, buf + idx, buf_len - 1 - idx);
		ESP_LOGI(TAG, buf + idx);
		idx += r;
	} while (r > 0 && idx < (buf_len - 1));

	if (r < 0)
	{
		ESP_LOGI(TAG, "... reading error, Last read return=%d errno=%d\r\n", r, errno);
		close(s);
		return -6;
	}

	if (idx >= buf_len - 1)
	{
		ESP_LOGI(TAG, "Overflow of receive buffer. %d bytes are not enough! idx = %d", buf_len, idx);
		close(s);
		return -7;
	}
	close(s);
	ESP_LOGI(TAG, "... done reading from socket. Last read return=%d errno=%d", r, errno);
	return 0;
}

#ifdef USE_SUNING
struct tm *get_tm_from_str(char *strTime)
{
	static struct tm tm_ = {0, 0, 0, 0, 0, 0, 0, 0, 0};
	int i = 0;
	int p[14];
	while (i < 14)
	{
		p[i] = strTime[i] - '0';
		if (p[i] >= 0 && p[i] <= 9)
		{
			i++;
			continue;
		}
		else
		{
			memset(&tm_, 0, sizeof(struct tm));
			return &tm_;
		}
	}
	tm_.tm_year = p[0] * 1000 + p[1] * 100 + p[2] * 10 + p[3] - 1900;
	tm_.tm_mon = p[4] * 10 + p[5] - 1;
	tm_.tm_mday = p[6] * 10 + p[7];
	tm_.tm_hour = p[8] * 10 + p[9];
	tm_.tm_min = p[10] * 10 + p[11];
	tm_.tm_sec = p[12] * 10 + p[13];
	tm_.tm_isdst = 0;
	return &tm_;
}
#endif

void http_get_task(void *pvParameters)
{
	int err;
	char recv_buf[2048];

	while (1)
	{
	// Wait for main task set the event bits
		EventBits_t bits = xEventGroupWaitBits(eg_http_get,
			HTTP_GET_TIME_BIT | HTTP_GET_WEATHER_BIT,
			pdTRUE,				//xClearOnExit
			pdFALSE,			//xWaitForAllBits
			portMAX_DELAY);
	// getting weather of next day
		if (bits & HTTP_GET_WEATHER_BIT)
		{
			ESP_LOGI(TAG, "Getting weather...");
			err = http_get_response(recv_buf, sizeof(recv_buf), WEB_SERVER, REQUEST);
			if(err == 0)
			{
				// char *pBody = http_response_get_body(recv_buf, idx);
				char *pBody = recv_buf;
				// ESP_LOGI(TAG, "Http response body : %s", pBody);
				cJSON *pjRoot = cJSON_Parse(pBody);
				if (pjRoot != NULL)
				{
					cJSON *pjResult = cJSON_GetObjectItem(pjRoot, "results");
					if (pjResult && cJSON_IsArray(pjResult))
					{
						int arrSize = cJSON_GetArraySize(pjResult);
						if(1 == arrSize)
						{
							cJSON *pjRes0 = cJSON_GetArrayItem(pjResult, 0);
							// location
							cJSON *pjLoc = cJSON_GetArrayItem(pjRes0, 0);
							ESP_LOGI(TAG, "Location : %s", cJSON_GetObjectItem(pjLoc, "name")->valuestring);
							// daily
							cJSON *pjDaily = cJSON_GetArrayItem(pjRes0, 1);
							cJSON *day = cJSON_GetArrayItem(pjDaily, 0);
							ESP_LOGI(TAG, "%s : %s %s~%s", cJSON_GetObjectItem(day, "date")->valuestring,
									cJSON_GetObjectItem(day, "text_day")->valuestring,
									cJSON_GetObjectItem(day, "low")->valuestring,
									cJSON_GetObjectItem(day, "high")->valuestring);

							xSemaphoreTake(xLvglMutex, portMAX_DELAY);
							lv_label_set_text(ui_LabelWeather1,
								cJSON_GetObjectItem(day, "text_day")->valuestring);
							lv_label_set_text_fmt(ui_LabelTemp1, "%s/%s℃",
								cJSON_GetObjectItem(day, "low")->valuestring,
								cJSON_GetObjectItem(day, "high")->valuestring);
							lv_label_set_text(ui_LabelWeatherIcon1,
								lookupWeatherCode(cJSON_GetObjectItem(day, "code_day")->valuestring));
							xSemaphoreGive(xLvglMutex);
						}
						else
						{
							ESP_LOGE(TAG, "Error - there are %d items in results.", arrSize);
						}
					}
					else
					{
						ESP_LOGE(TAG, "Item of 'results' is not found!");
					}
					cJSON_Delete(pjRoot);
				}
				else
				{
					ESP_LOGE(TAG, "Http response is not json.");
					ESP_LOGE(TAG, "Http response - %s", pBody);
				}
			}
			else
			{
				ESP_LOGE(TAG, "Weather tomorrow getting error %d!", err);
			}
		// geting current weather information
			err = http_get_response(recv_buf, sizeof(recv_buf), WEB_SERVER, REQUEST1);
			if(err == 0)
			{
				// char *pBody = http_response_get_body(recv_buf, idx);
				char *pBody = recv_buf;
				//ESP_LOGI(TAG, "Http response body : %s", pBody);
				cJSON *pjRoot = cJSON_Parse(pBody);
				if (pjRoot != NULL)
				{
					cJSON *pjResult = cJSON_GetObjectItem(pjRoot, "results");
					if (pjResult && cJSON_IsArray(pjResult))
					{
						int arrSize = cJSON_GetArraySize(pjResult);
						if(1 == arrSize)
						{
							cJSON *pjRes0 = cJSON_GetArrayItem(pjResult, 0);
							// location
							cJSON *pjLoc = cJSON_GetArrayItem(pjRes0, 0);
							ESP_LOGI(TAG, "Location : %s",
								cJSON_GetObjectItem(pjLoc, "name")->valuestring);
							// now
							cJSON *pjNow = cJSON_GetArrayItem(pjRes0, 1);
							ESP_LOGI(TAG, "%s,code: %s,%s℃",
								cJSON_GetObjectItem(pjNow, "text")->valuestring,
								cJSON_GetObjectItem(pjNow, "code")->valuestring,
								cJSON_GetObjectItem(pjNow, "temperature")->valuestring);
							// refresh the content of LCD
							xSemaphoreTake(xLvglMutex, portMAX_DELAY);
							lv_label_set_text_fmt(ui_LabelAddr, "%s",
								cJSON_GetObjectItem(pjLoc, "name")->valuestring);

							lv_label_set_text(ui_LabelWeather,
								cJSON_GetObjectItem(pjNow, "text")->valuestring);
							lv_label_set_text(ui_LabelWeatherIcon,
								lookupWeatherCode(cJSON_GetObjectItem(pjNow, "code")->valuestring));
							lv_label_set_text_fmt(ui_LabelTemp, "%s℃",
								cJSON_GetObjectItem(pjNow, "temperature")->valuestring);
							xSemaphoreGive(xLvglMutex);
						}
						else
						{
							ESP_LOGE(TAG, "Error - there are %d items in results.", arrSize);
						}
					}
					else
					{
						ESP_LOGE(TAG, "Item of 'results' is not found!");
					}
					cJSON_Delete(pjRoot);
				}
				else
				{
					ESP_LOGE(TAG, "Http response is not json.");
					ESP_LOGE(TAG, "Http response - %s", pBody);
				}
			}
			else
			{
				ESP_LOGE(TAG, "Weather now getting error %d!", err);
			}
		}

#ifdef USE_SUNING
		if (bits & HTTP_GET_TIME_BIT)
		{
			ESP_LOGI(TAG, "Getting time...");

			err = http_get_response(recv_buf, sizeof(recv_buf), WEB_SERVER2, REQUEST2);
			if(err == 0)
			{
				char *pBody = http_response_get_body(recv_buf, strlen(recv_buf));
				ESP_LOGI(TAG, "Http response body : %s", pBody);
				// example : {"sysTime2":"2025-04-19 23:02:35","sysTime1":"20250419230235"}
				cJSON *pjRoot = cJSON_Parse(pBody);
				if (pjRoot != NULL)
				{
					ESP_LOGI(TAG, "sysTime1 : %s", cJSON_GetObjectItem(pjRoot, "sysTime1")->valuestring);
					time_t t_ = mktime(get_tm_from_str(cJSON_GetObjectItem(pjRoot, "sysTime1")->valuestring));
					struct timeval tv;
					tv.tv_sec = t_;
					tv.tv_usec = 0;
					settimeofday(&tv, 0);

					cJSON_Delete(pjRoot);
				}
				else
				{
					ESP_LOGE(TAG, "Http response is not json.");
					ESP_LOGE(TAG, "Http response - %s", pBody);
				}
			}
			else
			{
				ESP_LOGE(TAG, "Time getting error %d!", err);
			}

			ESP_LOGI(TAG, "Done getting time!");
		}
#endif
	}
}
