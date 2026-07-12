# tools/simulator

The embedded-react **simulator** — the dev tool with **hot reload**. It runs your JSX app on the
desktop SDL host and live-reloads the window whenever you save, the React Native inner loop on
embedded. (The desktop *demo* lives at `examples/linux` and is just a demo; this is the dev tool.)

It reuses the shared desktop host core (`examples/linux/host.c`) and adds a file-watch + full-remount
reload loop (`sim_main.c`).

## Use

```
# one-time: build the simulator binary
cmake -S tools/simulator -B tools/simulator/build [-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake]
cmake --build tools/simulator/build

# then run the SDL simulator against a demo, by name, from the JS package:
cd bridges/quickjs/js && npm run sim -- thermostat
# (omit the name for the default demo; `npm run build -- <demo>` / `npm run pack -- <demo>` build it too)
```

`npm run sim` runs `esbuild --watch` (rebundling on save) and launches the simulator, which reloads on
change. It reads `demos/<name>/index.jsx` directly against the in-repo library — no install needed. (For
the browser/WASM simulator instead, `cd demos/<name> && npm install && npm run dev`.) Remaining work
(on-device hot reload, Fast Refresh, a prebuilt sim binary) is tracked in
[`/ROADMAP.md`](../../ROADMAP.md).
