# thermostat

The default demo, and the one we're growing into a real **thermostat / climate-control UI**.

> Currently, it's still the starter features tour (it began life as `basic`); it will be reworked
> into a thermostat screen.

## Build & run

```
cd bridges/quickjs/js
npm run build            # thermostat is the default; -> dist/app.bundle.js
```

Then run the desktop host (`examples/linux`) or flash a device (`examples/esp32/esp32-s3`) — see
their READMEs. The host injects the globals the bundle expects (`NativeUI`, `screen`, `console`,
timers).
