# examples/esp32/esp32-s3 — ESP32-S3 host (Flow A)

The reference Flow A board: a real React reconciler on QuickJS, with the JavaScript heap in PSRAM.
The firmware brings up the panel, the touch controller and QuickJS, then loads the app as a config
container (`app.erpkg`) from a dedicated flash partition, memory-mapped and verified. To ship a new UI
you write one partition; the firmware is untouched.

**Board:** Waveshare **ESP32-S3-Touch-LCD-7**: a 7" 800×480 RGB-parallel IPS panel, 8 MB octal PSRAM,
16 MB flash, a GT911 capacitive touch controller and a CH422G I²C expander.

**Guide:** [embedded-react.dev/guides/boards/esp32-s3](https://embedded-react.dev/guides/boards/esp32-s3)
covers the build, flashing an app, hot reload over USB, the optional WiFi build, the backend, adapting
the project to another panel, and the things that go wrong.

## Layout

```
CMakeLists.txt            top-level IDF project; FetchContent of QuickJS-ng (v0.15.0) + embedded-react
sdkconfig.defaults        ESP32-S3, octal PSRAM, 16 MB flash, a 64 KB main task stack
sdkconfig.defaults.wifi   layered over the defaults for the WiFi build
partitions.csv            factory (firmware) + a 2 MB 'config' data partition for app.erpkg
main/
  main.c                  host: PSRAM JS heap, map + load the config partition, frame loop, touch
  board.c / board.h       Waveshare 7" bring-up: CH422G expander, RGB panel, GT911 touch
  hotreload_usb.c         the USB receiver (built only with -DER_HOTRELOAD=1)
  network.c / network_js.c   the WiFi station, SNTP, and the __erWifi / __erClock host globals (WiFi build)
components/               CMakeLists-only wrappers that compile the fetched (or in-tree) engine, bridge,
                          QuickJS and esp32-lcd backend sources
```

In-tree the example uses the local engine and bridge; copied out on its own it fetches them from
GitHub at a pinned tag, so the folder is a copy-out-ready template.

## Build

ESP-IDF v6.x, exported (`. $IDF_PATH/export.sh`). Flash through the **UART** USB-C port.

```bash
cd examples/esp32/esp32-s3
idf.py set-target esp32s3
idf.py build flash monitor          # "No config loaded" on the panel means the firmware works
```

Then, from your app project (or `cd bridges/quickjs/js && npm run pack` for a repo demo):

```bash
npx embedded-react build                                                  # → dist/app.erpkg
parttool.py write_partition --partition-name=config --input dist/app.erpkg
```

Optional builds: `idf.py -DER_HOTRELOAD=1 build flash` for hot reload over the native USB port
(`npx embedded-react dev --device`), `-DER_PERF_DETAIL=1` to add the engine's frame timings to the on-panel metrics, and
`idf.py -B build-wifi -D SDKCONFIG=sdkconfig.wifi -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.wifi" build flash`
for the Wi-Fi build.
