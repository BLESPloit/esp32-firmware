# Glob everything that always compiles
file(GLOB BASE_SRCS
    "api/*.c"
    "ble/*.c"
    "common/*.c"
    "graphics/graphics.c"
    "graphics/websocket_gfx.c"
    "hw/button.c"
    "hw/board.c"
    "interface/*.c"
    "lua/*.c"
    "main.c"
)

if(CONFIG_BLESPLOIT_BOARD_M5STICKS3)
    set(DISPLAY_SRC "hw/display_m5sticks3.c" "hw/m5stack_pmic.c")
elseif(CONFIG_BLESPLOIT_BOARD_LILYGO_T_QT_PRO)
    set(DISPLAY_SRC "hw/display_t_qt_pro.c")
else()
    set(DISPLAY_SRC "")
endif()

if(CONFIG_BLESPLOIT_BOARD_BARE)
    set(LVGL_SRCS "graphics/lvgl_stub.c")
else()
    set(LVGL_SRCS
        "graphics/lvgl_core.c"
        "graphics/lvgl_svg.c"
        "graphics/lvgl_ui.c"
    )
endif()

file(GLOB HTML_FILES "html/*")

idf_component_register(
    SRCS ${BASE_SRCS} ${DISPLAY_SRC} ${LVGL_SRCS}
    INCLUDE_DIRS "."
    EMBED_FILES ${HTML_FILES}
)
