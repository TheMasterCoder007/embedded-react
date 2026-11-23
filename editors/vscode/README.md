# Embedded React for VS Code

Debugs a Flow B app on a board from the editor: breakpoints in the gutter of the app's source, the step buttons, the
app's frames in the call stack, and its state and locals in the Variables pane. C breakpoints and stepping work in the
same session.

The extension only registers the `embedded-react` debug type. The debug adapter is in the project's `embedded-react`
package (`embedded-react debug-adapter`), so it always matches the compiler that wrote `app.erdbg`.

## Needs

- A project with `embedded-react` installed, and an app built with `embedded-react build --aot`.
- Firmware built with `ERB_DEBUG_HOOK=1`, for breakpoints and stepping in the app.
- GDB 14 or later for the board, built with Python.
- A GDB server for the probe, such as OpenOCD.

## Use

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
  "server": ["openocd", "-f", "interface/stlink.cfg", "-f", "target/stm32f7x.cfg"],
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
