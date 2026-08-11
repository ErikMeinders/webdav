idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        erikmeinders__webdav
        esp_wifi
        esp_netif
        esp_event
)

# Note: requires a partition named "storage" of subtype "littlefs" in
# partitions.csv (see ../partitions.csv). Seeds the image with the contents
# of flash_data/ at build time so the example has something to browse on
# first boot.
littlefs_create_partition_image(storage ../flash_data FLASH_IN_PROJECT)
