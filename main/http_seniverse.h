#ifndef HTTP_SENIVERSE_H
#define HTTP_SENIVERSE_H

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define HTTP_GET_TIME_BIT	    BIT0
#define HTTP_GET_WEATHER_BIT	BIT1

extern EventGroupHandle_t eg_http_get;

void http_get_task(void *pvParameters);

#endif