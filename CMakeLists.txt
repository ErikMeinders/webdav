idf_component_register(
    SRCS
        "src/esp_webdav.c"
        "src/esp_webdav_propfind.c"
        "src/esp_webdav_copy_move.c"
        "src/esp_webdav_util.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_http_server
    PRIV_REQUIRES esp_hw_support
)
