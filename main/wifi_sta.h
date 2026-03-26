/* 
    Common functions to establish Wi-Fi connection.
 */

#ifndef WIFI_STA_H
#define WIFI_STA_H

#ifdef __cplusplus
extern "C" {
#endif

/* The examples use WiFi configuration that you can set via project configuration menu

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/

extern char is_wifi_connected;

void wifi_init_sta(char *ssid, char *passwd);

#ifdef __cplusplus
}
#endif

#endif //WIFI_STA_H
