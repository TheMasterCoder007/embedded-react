# ESP32-2432S028R (CYD) Flow B host: CYD board bring-up + the lean SPI backend + the Flow B runtime. No QuickJS, and
# no app at build time: the firmware loads a package from the erb_a and erb_b partitions (see ../partitions.csv).
idf_component_register(
    SRCS "main.c" "board.c" "native_modules.c"
         # shared with the other ESP32 example: records a panic for the next boot
         "${EMBEDDED_REACT_SRC_DIR}/examples/esp32/common/fault_record.c"
    INCLUDE_DIRS "." "${EMBEDDED_REACT_SRC_DIR}/examples/esp32/common"
                 "${EMBEDDED_REACT_SRC_DIR}/examples/device/include" # the device package's generated headers
    REQUIRES engine erb-runtime esp32-spi-lcd-backend esp_lcd driver esp_driver_uart esp_timer
)

# fault_record.c's panic handler runs ahead of ESP-IDF's, which it then calls.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")

# On-device hot reload over the UART port (dev only): `embedded-react dev --device <port> --aot`. A release firmware
# shouldn't carry the receiver, so it is off unless you build with `idf.py -DER_HOTRELOAD=1`.
option(ER_HOTRELOAD "Enable on-device hot reload over the UART (dev only)" OFF)
target_compile_definitions(${COMPONENT_LIB} PRIVATE ER_HOTRELOAD=$<BOOL:${ER_HOTRELOAD}>)

# A forced panic 5 s after boot, to check the error screen on hardware (see main.c): -DER_BOARD_CRASH_TEST=1.
if(DEFINED ER_BOARD_CRASH_TEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ER_BOARD_CRASH_TEST=${ER_BOARD_CRASH_TEST})
endif()
