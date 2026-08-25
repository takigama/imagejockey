idf_component_register(
    SRCS
        "app_main.c"
        "button.c"
        "display.c"
        "logbuf.c"
        "media.c"
        "msc_disk.c"
        "ota.c"
        "sdcard.c"
        "settings.c"
        "ui.c"
        "web.c"
        "wifi.c"
    INCLUDE_DIRS "."
)

# See top-level CMakeLists.txt -- FIRMWARE_VERSION has to be applied here,
# not as a top-level add_compile_definitions(), for it to actually reach
# ota.c.
target_compile_definitions(${COMPONENT_LIB} PRIVATE FIRMWARE_VERSION="${FIRMWARE_VERSION}")
