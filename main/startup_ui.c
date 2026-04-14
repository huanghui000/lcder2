#include "startup_ui.h"

#include "../GUI/lvgl.h"
#include "ui.h"

#include <string.h>

#define STARTUP_UI_MIN_HOLD_MS 1000

typedef struct
{
    char title[32];
    char detail[96];
    char hide_after_update;
} startup_ui_state_t;

static lv_obj_t *s_overlay;
static lv_obj_t *s_title;
static lv_obj_t *s_detail;
static lv_timer_t *s_hide_timer;
static char s_ui_ready;
static char s_has_pending_state;
static char s_hold_visible;
static uint32_t s_last_apply_tick;
static startup_ui_state_t s_cached_state = {"", "", 0};
static startup_ui_state_t s_pending_state;

static void startup_ui_hide_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);

    if (s_overlay)
    {
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_hide_timer)
    {
        lv_timer_del(s_hide_timer);
        s_hide_timer = NULL;
    }
}

static void startup_ui_apply_state(const startup_ui_state_t *state)
{
    if (!s_ui_ready || !s_overlay)
    {
        s_cached_state = *state;
        return;
    }

    if (s_hide_timer)
    {
        lv_timer_del(s_hide_timer);
        s_hide_timer = NULL;
    }

    lv_label_set_text(s_title, state->title);
    lv_label_set_text(s_detail, state->detail);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    s_cached_state = *state;
    s_last_apply_tick = lv_tick_get();

    if (state->hide_after_update)
    {
        if (!s_hold_visible)
        {
            s_hide_timer = lv_timer_create(startup_ui_hide_timer_cb, 1200, NULL);
            lv_timer_set_repeat_count(s_hide_timer, 1);
        }
    }
}

static void startup_ui_post(const startup_ui_state_t *state)
{
    s_pending_state = *state;
    s_has_pending_state = 1;

    if (!s_ui_ready)
    {
        s_cached_state = *state;
    }
}

void startup_ui_init(void)
{
    if (s_ui_ready)
    {
        return;
    }

    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, 160, 160);
    lv_obj_set_style_bg_color(s_overlay, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    s_title = lv_label_create(s_overlay);
    lv_obj_set_width(s_title, 140);
    lv_obj_align(s_title, LV_ALIGN_CENTER, 0, -14);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_title, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(s_title, "");

    s_detail = lv_label_create(s_overlay);
    lv_obj_set_width(s_detail, 144);
    lv_obj_align(s_detail, LV_ALIGN_CENTER, 0, 18);
    lv_obj_set_style_text_align(s_detail, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_detail, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s_detail, &lv_font_montserrat_18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(s_detail, "");

    s_ui_ready = 1;
    startup_ui_apply_state(&s_cached_state);
}

void startup_ui_process(void)
{
    uint32_t now;

    if (!s_ui_ready || !s_has_pending_state)
    {
        return;
    }

    now = lv_tick_get();
    if (s_last_apply_tick != 0 && (now - s_last_apply_tick) < STARTUP_UI_MIN_HOLD_MS)
    {
        return;
    }

    s_has_pending_state = 0;
    startup_ui_apply_state(&s_pending_state);
}

void startup_ui_show_status(const char *title, const char *detail, int progress)
{
    startup_ui_state_t state;

    LV_UNUSED(progress);
    memset(&state, 0, sizeof(state));
    strncpy(state.title, title ? title : "", sizeof(state.title) - 1);
    strncpy(state.detail, detail ? detail : "", sizeof(state.detail) - 1);
    startup_ui_post(&state);
}

void startup_ui_show_connected(const char *detail)
{
    startup_ui_state_t state;

    memset(&state, 0, sizeof(state));
    strncpy(state.title, "WiFi Ready", sizeof(state.title) - 1);
    strncpy(state.detail, detail ? detail : "Network connected", sizeof(state.detail) - 1);
    state.hide_after_update = 1;
    startup_ui_post(&state);
}

void startup_ui_hide_delayed(uint32_t delay_ms)
{
    LV_UNUSED(delay_ms);
    startup_ui_show_connected("Network connected");
}

void startup_ui_set_hold_visible(char hold_visible)
{
    s_hold_visible = hold_visible ? 1 : 0;
}
