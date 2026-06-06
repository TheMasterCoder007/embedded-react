# thermostat

A climate-control demo built around a draggable **270° arc dial** (a physical-thermostat metaphor).
Dragging the handle around the arc sets the target temperature; a −/+ stepper and a Heat/Cool/Auto/Off
mode selector round it out, with Humidity / Outdoor / Next-change metric cards beneath.

It's the default demo and is **responsive**: every dimension derives from the host-injected `screen`
global, so one source flexes between the 800×480 panel (ESP32-S3 / Flow A) and the 240×320 panel
(no-PSRAM ESP32 / Flow B). A `width < 400` breakpoint selects the compact layout.

## Build & run

```
cd bridges/quickjs/js
npm run build            # thermostat is the default; -> dist/app.bundle.js
```

Then run the desktop host (`examples/linux`) or flash a device (`examples/esp32/esp32-s3`) — see
their READMEs. The host injects the globals the bundle expects (`NativeUI`, `screen`, `console`,
timers).
