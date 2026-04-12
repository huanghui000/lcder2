#ifndef UI_SPIFFS_FONT_H
#define UI_SPIFFS_FONT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "../GUI/lvgl.h"

lv_font_t * ui_spiffs_font_load(const char * path);
void ui_spiffs_font_free(lv_font_t * font);

#ifdef __cplusplus
}
#endif

#endif
