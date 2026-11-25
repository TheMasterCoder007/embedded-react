# Embedded React for VS Code

Debugs a Flow B app from the editor, on a board or in the simulator with no board: breakpoints in the gutter of the
app's source, the step buttons, the app's frames in the call stack, and its state and locals in the Variables pane.
An app fault stops on the line that faulted, with its reason. On a board, C breakpoints and stepping work in the same
session.

The extension only registers the `embedded-react` debug type. The debug adapter is in the project's `embedded-react`
package (`embedded-react debug-adapter`), so it always matches the compiler that wrote `app.erdbg`.

## In the simulator

A project with `embedded-react` installed is all it needs. Add this configuration to `.vscode/launch.json` (the snippet
"Embedded React: simulator" fills it in) and start it:

```json
{
  "type": "embedded-react",
  "request": "launch",
  "name": "Debug the app in the simulator",
  "simulator": true,
  "cwd": "${workspaceFolder}"
}
```

It starts the Flow B simulator (`embedded-react dev --aot`), shows it in a split to the right of the code, and ends it with the session. A
save reloads the app and keeps the breakpoints. The `embedded-react.openSimulator` setting opens the simulator in a
browser instead, or leaves it closed. "Embedded React: attach to the simulator" debugs one that already runs.

## On a board

- A project with `embedded-react` installed, and an app built with `embedded-react build --aot`.
- Firmware built with `ERB_DEBUG_HOOK=1`, for breakpoints and stepping in the app and the stop at a fault.
- GDB 14 or later for the board, built with Python. The session's error says when the GDB isn't, and what to use.
- A GDB server for the probe, such as OpenOCD.

Add a configuration to `.vscode/launch.json` (the snippets "Embedded React: board through OpenOCD" and "Embedded React:
ESP32-S3 built-in JTAG" fill one in), then start it:

```json
{
  "type": "embedded-react",
  "request": "attach",
  "name": "Debug the app on the board",
  "program": "${workspaceFolder}/build/firmware.elf",
  "debugFile": "${workspaceFolder}/dist/app.erdbg",
  "gdb": "arm-none-eabi-gdb",
  "server": [
    "openocd",
    "-f",
    "interface/stlink.cfg",
    "-f",
    "target/stm32f7x.cfg"
  ],
  "serverReady": "Listening on port 3333 for gdb connections",
  "target": "localhost:3333",
  "disconnectCommands": ["monitor resume"]
}
```

[Debugging on a board](https://embedded-react.dev/guides/debugging#from-an-editor) has a configuration for each board
family, and [The debug adapter](https://embedded-react.dev/internals/flow-b-packages#the-debug-adapter) describes each
field.

## Build

```bash
npm install
npm run build
```

`npm run package` writes a `.vsix` to install with "Extensions: Install from VSIX".
