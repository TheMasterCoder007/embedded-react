# ESP32-S3 host (uploaded-config model): brings up the Waveshare 7" RGB panel (board.c) + the portable
# QuickJS host core (er_runtime, JS heap in PSRAM), then loads the app as a config container (app.erpkg)
# memory-mapped from the 'config' flash partition (see ../partitions.csv) — NOT embedded in the
# firmware image. The container carries its own bytecode + assets, so neither is compiled in here.
#
# The firmware builds standalone (no app needed at build time, just like real firmware). Flash a config
# to the 'config' partition separately, then reboot:
#   (in bridges/quickjs/js)  npm run pack            # → dist/app.erpkg
#   parttool.py write_partition --partition-name=config \
#       --input ../../bridges/quickjs/js/dist/app.erpkg
# Until a config is flashed, the device shows a "No config loaded" panel.

idf_component_register(
    SRCS "main.c" "board.c" "hotreload_usb.c"
    INCLUDE_DIRS "."
    REQUIRES engine er-bridge-quickjs quickjs esp32-lcd-backend
             esp_psram esp_timer esp_lcd esp_driver_i2c esp_driver_gpio
             esp_partition spi_flash esp_driver_usb_serial_jtag
)

# Enable the on-screen performance overlay (FPS / CPU / free memory). main.c gates its host-side
# metric gathering on ER_PERF_OVERLAY; the engine component sets the same flag so the renderer draws
# the panel. Set here too so the host wiring is enabled regardless of requirement propagation order.
target_compile_definitions(${COMPONENT_LIB} PRIVATE ER_PERF_OVERLAY=1)

# On-device hot reload over USB-Serial-JTAG (DEV ONLY) — `embedded-react dev --device <port>` streams a
# fresh app.erpkg on every save and the frame loop swaps it in live. OFF by default: the standard dev
# workflow is the web simulator (`npx embedded-react dev`), and a release firmware should not carry the
# receiver. Opt a board in at build time with `idf.py -DER_HOTRELOAD=1 build` (or flip the default below).
# When off, the firmware only runs the config flashed to the 'config' partition.
option(ER_HOTRELOAD "Enable on-device hot reload over USB (dev only)" OFF)
target_compile_definitions(${COMPONENT_LIB} PRIVATE ER_HOTRELOAD=$<BOOL:${ER_HOTRELOAD}>)

# Whole-screen rotation (clockwise): 0 / 90 / 180 / 270. Change this for a differently-mounted panel;
# 0 (the default) is the fast path (a plain framebuffer copy at present). 90/270 give a portrait UI on
# the landscape panel; the thermostat demo stacks its layout for the narrower width.
target_compile_definitions(${COMPONENT_LIB} PRIVATE ER_DISPLAY_ROTATION=0)
