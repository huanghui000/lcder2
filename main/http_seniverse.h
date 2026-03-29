#ifndef HTTP_SENIVERSE_H
#define HTTP_SENIVERSE_H

#include <stddef.h>

#include "esp_err.h"

void http_get_task(void *pvParameters);
void http_seniverse_init(void);
esp_err_t http_seniverse_get_location(char *id, size_t id_len, char *name, size_t name_len);
esp_err_t http_seniverse_set_location_by_query(const char *query,
    char *resolved_name, size_t resolved_name_len,
    char *resolved_detail, size_t resolved_detail_len);
void http_seniverse_request_refresh(void);

#endif
