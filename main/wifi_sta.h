/* 
    Common functions to establish Wi-Fi connection.
 */

#ifndef WIFI_STA_H
#define WIFI_STA_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#include "esp_err.h"

/* The examples use WiFi configuration that you can set via project configuration menu

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/

extern char is_wifi_connected;

void wifi_init_sta(char *ssid, char *passwd);
esp_err_t wifi_get_active_credentials(char *ssid, size_t ssid_len, char *passwd, size_t passwd_len);
esp_err_t wifi_try_update_credentials(const char *ssid, const char *passwd, char *status_msg, size_t status_msg_len);

#ifdef __cplusplus
}
#endif

#endif //WIFI_STA_H
