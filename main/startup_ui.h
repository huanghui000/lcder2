#ifndef STARTUP_UI_H
#define STARTUP_UI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

void startup_ui_init(void);
void startup_ui_process(void);
void startup_ui_show_status(const char *title, const char *detail, int progress);
void startup_ui_show_connected(const char *detail);
void startup_ui_hide_delayed(uint32_t delay_ms);
void startup_ui_set_hold_visible(char hold_visible);

#ifdef __cplusplus
}
#endif

#endif
