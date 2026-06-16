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

# then iterate from your app folder (React Native style):
cd demos/thermostat && npm run sim
# (equivalent: cd bridges/quickjs/js && npm run sim <demo>)
```

`npm run sim` runs `esbuild --watch` (rebundling on save) and launches the simulator, which reloads on
change. Remaining work (on-device hot reload, Fast Refresh, a prebuilt sim binary) is tracked in
[`/ROADMAP.md`](../../ROADMAP.md).
