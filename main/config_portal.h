#ifndef CONFIG_PORTAL_H
#define CONFIG_PORTAL_H

#include <stddef.h>
#include "esp_err.h"

esp_err_t config_portal_start(void);
void config_portal_get_url(char *buf, size_t buf_len);

#endif
