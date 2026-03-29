set(COMPONENT_SRCS
    "lcder_main.c"
    "wifi_sta.c"
    "startup_ui.c"
    "http_seniverse.c"
    "config_portal.c"
    "ui.c"
    "ui_helpers.c"
    "screens/ui_Screen1.c"
    "screens/ui_Screen2.c"
    "../GUI/src/extra/libs/qrcode/lv_qrcode.c"
    "../GUI/src/extra/libs/qrcode/qrcodegen.c"
)

register_component()
