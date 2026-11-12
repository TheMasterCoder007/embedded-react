# ESP32-2432S028R (CYD) Flow B host: CYD board bring-up + the lean SPI backend + the Flow B runtime. No QuickJS, and
# no app at build time: the firmware loads a package from the erb_a and erb_b partitions (see ../partitions.csv).
idf_component_register(
    SRCS "main.c" "board.c" "native_modules.c"
    INCLUDE_DIRS "." "${EMBEDDED_REACT_SRC_DIR}/examples/device/include" # the device package's generated headers
    REQUIRES engine erb-runtime esp32-spi-lcd-backend esp_lcd driver esp_driver_uart esp_timer
)

# On-device hot reload over the UART port (dev only): `embedded-react dev --device <port> --aot`. A release firmware
# shouldn't carry the receiver, so it is off unless you build with `idf.py -DER_HOTRELOAD=1`.
option(ER_HOTRELOAD "Enable on-device hot reload over the UART (dev only)" OFF)
target_compile_definitions(${COMPONENT_LIB} PRIVATE ER_HOTRELOAD=$<BOOL:${ER_HOTRELOAD}>)
