# tools/simulator — the SDL simulator

The desktop hot-reload simulator: runs a demo on the native SDL host (`examples/linux/host.c`) and
reloads the window whenever a file is saved, with component state preserved and a red box for
JavaScript errors. It is the maintainers' engine-debug tool, since the real C engine runs natively
under gdb or lldb; the shipped app-developer loop is the browser simulator (`npx embedded-react dev`).

**Docs:** [Hot reload](https://embedded-react.dev/guides/hot-reload#on-the-desktop).

```
# once: build the simulator binary
cmake -S tools/simulator -B tools/simulator/build [-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake]
cmake --build tools/simulator/build

# then run it against a demo, by name, from the JS package
cd packages/embedded-react && npm run sim -- thermostat
```

`npm run sim` runs esbuild in watch mode and launches the simulator, reading `demos/<name>/index.jsx`
directly against the in-repo library. Press R for a clean reset that also forgets persisted state.
`ER_ROTATION=90` (or 180, 270) runs it behind a panel mounted that way, the app upright in the window.
Press T to turn the panel a quarter turn while the app runs: the window takes the new shape and the app lays out
again with its state kept, as on a device that rotates live.
