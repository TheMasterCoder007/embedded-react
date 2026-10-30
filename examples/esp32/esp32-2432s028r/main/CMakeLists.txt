# ESP32-2432S028R (CYD) Flow B host: CYD board bring-up + the lean SPI backend + the Flow B runtime. No QuickJS, and
# no app at build time: the firmware loads a package from the erb_a and erb_b partitions (see ../partitions.csv).
idf_component_register(
    SRCS "main.c" "board.c"
    INCLUDE_DIRS "."
    REQUIRES engine erb-runtime esp32-spi-lcd-backend esp_lcd driver esp_timer
)
