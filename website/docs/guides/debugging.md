---
title: 'Debugging on a board'
description: 'Debug a Flow B app on the board through a debug probe: GDB commands that read the app by its own names, breakpoints and stepping in its source, a stop when the app faults, and the same from VS Code.'
---

A Flow B app on a board can be debugged the way its firmware is: through a debug probe, with GDB. A script in the
npm package teaches GDB the app, so a halted board answers in the app's terms: the source line it is on, its state
and locals by name, the node tree, and why a package was refused. Breakpoints and stepping work on the app's source
lines, and C breakpoints work beside them in the same session.

Nothing here needs a serial port or a build of the app made for debugging. The commands that read run no code on
the board, so they work on a board that has stopped answering.

## What it needs

- **A probe and a GDB server for it.** An ST-Link on an STM32, the built-in JTAG on an ESP32-S3, a CMSIS-DAP debug
  probe on an RP2040. OpenOCD drives all three.
- **A GDB for the board, built with Python** (3.8 or later). `gdb -batch -ex "python print(1)"` prints `1` when it
  has one. Debugging from an editor needs GDB 14 or later.
- **The firmware's ELF**, as the board runs it. Only its symbols are read, so a release build works.
- **The app's `app.erdbg`.** Every `embedded-react build --aot` writes it beside the package in `dist/`, release or
  not. It has to be the one written with the package on the board: the script checks, and says when it isn't.
- **For breakpoints, stepping and the stop at a fault, firmware built with `ERB_DEBUG_HOOK=1`.** Reading the app
  needs no option.

`ERB_DEBUG_HOOK` is a CMake option of `runtime/`, off by default. The ESP32-S3 example takes it as
`idf.py -D ERB_DEBUG_HOOK=1`, and the RP2040 example as `-DERB_DEBUG_HOOK=1` when it is configured. A firmware that
compiles the runtime's sources itself adds `ERB_DEBUG_HOOK=1` to their compile definitions. With it the VM tests
one flag before each instruction, so leave it out of a release build.

The hook holds 16 places, which `ERB_DEBUG_STOPS` changes, at 8 bytes each. A place is one spot in the app's code.
A breakpoint takes one for each statement that starts on its line, so a line can take more than one, and
`erb break` says when the breakpoints need more places than the hook has. The RP2040 example has too little RAM
left beside its framebuffer for 16, and holds 8.

## Connect the probe

Each board family has its own GDB server command. Start it in one terminal and leave it running, then start GDB in
another from the app's project folder, so the script finds the source.

### STM32, with an ST-Link

```bash
openocd -f interface/stlink.cfg -f target/stm32h7x.cfg
```

Name the target file for your chip (`stm32f7x.cfg`, `stm32h7x.cfg`). On a dual-bank STM32H7, add
`-c "set DUAL_BANK 1"` before the target file, or breakpoints in the second bank never fire.

```bash
gdb build/firmware.elf -ex "target extended-remote :3333"
```

The Arm GNU toolchain's `arm-none-eabi-gdb` has no Python. Use its `arm-none-eabi-gdb-py` where it has one, or
your system's `gdb` or `gdb-multiarch`. Attaching halts the board where it was, without a reset.

### ESP32-S3, over its built-in JTAG

The chip's own USB port is the probe. A port behind a USB-to-UART bridge isn't. With ESP-IDF's environment
exported:

```bash
openocd -f board/esp32s3-builtin.cfg -c "init; reset halt"
```

```bash
xtensa-esp32s3-elf-gdb build-flow-b/embedded-react-esp32.elf -ex "target extended-remote :3333"
```

Use Espressif's GDB: a stock GDB can't read this chip's registers. The server resets the chip as it starts, so the
app isn't loaded yet when GDB connects. Set breakpoints, then `continue`.

### RP2040, with a debug probe

Wire a CMSIS-DAP probe, such as the Raspberry Pi Debug Probe, to the board's SWCLK, SWDIO and ground:

```bash
openocd -f interface/cmsis-dap.cfg -f target/rp2040.cfg -c "adapter speed 5000"
```

```bash
gdb build/embedded-react-watch.elf -ex "target extended-remote :3333"
```

### ESP32 CYD

The CYD has no probe to connect: its ESP32 has no built-in JTAG, and the display uses the chip's JTAG pins. A fault
there is traced from the log with
[`decode-fault`](../internals/flow-b-packages.md#a-hard-fault-with-no-probe-attached).

### Load the script

Once GDB is connected, load the script and the app's debug file:

```text
(gdb) source node_modules/embedded-react/gdb/erb.py
(gdb) erb file dist/app.erdbg
Loaded dist/app.erdbg: build 1a2b3c4d, 1 layout
```

A firmware without the hook gets a note here that breakpoints and stepping are off. The other commands work.

With the hook, the script keeps one breakpoint on the hook's function from here on. Breakpoints, steps and the stop
at a fault all share it, so they take one of the chip's hardware breakpoints between them.

### Ending a session

GDB leaves the board halted when it goes. Resume it first:

```text
(gdb) monitor resume
(gdb) disconnect
```

## While the board is halted

A halted core runs nothing: no frames, no touch, no timers.

- **The watchdog** has to stop too, or it resets the board in the middle of a session. On an STM32, freeze the
  independent watchdog while the core is halted: `__HAL_DBGMCU_FREEZE_IWDG()` at start-up
  (`__HAL_DBGMCU_FREEZE_IWDG1()` on an H7). On an RP2040, pass `true` as `watchdog_enable()`'s second argument,
  as the example does. On an ESP32-S3, Espressif's OpenOCD stops the chip's watchdogs at each halt.
- **The panel** keeps the last frame where it holds its own memory (an SPI panel) or the chip scans a framebuffer
  out by itself (an STM32's LTDC). An ESP32-S3's RGB panel is fed by the chip as it runs, so it goes black or shows
  glitches during a halt, and the picture comes back when the board runs again. A touch made while halted is lost.
- **The app's clock** doesn't count the halt. The script tells the runtime and the engine at every stop, so
  timers and animations carry on from where they were instead of jumping ahead.

## Commands

| Command              | What it does                                                                                |
| -------------------- | ------------------------------------------------------------------------------------------- |
| `erb file PATH`      | Loads the app's debug file                                                                  |
| `erb where`          | The app's source line and the lines that called it, with each function's parameters         |
| `erb locals [N]`     | The current function's parameters and locals, or those of the Nth caller in `erb where`     |
| `erb state [NAME]`   | Every state by name, or one. `erb state all` adds the state the compiler made               |
| `erb set NAME VALUE` | Sets a state. The app runs its update at its next tick, as after a setter                   |
| `erb watch NAME`     | Stops when a state changes, and names the line that wrote it                                |
| `erb error`          | The last fault or refused load: the reason, the amounts, and the source line                |
| `erb pools`          | What the app and the engine use of their pools                                              |
| `er nodes`           | The node tree: each node's type, its rect on the screen, and the source line of its element |
| `erb break [LINE]`   | Stops at a source line (`App.tsx:41`, or `41`). With no line, lists the breakpoints         |
| `erb delete [N...]`  | Deletes breakpoints by number, or all of them                                               |
| `erb step`           | Runs to the next statement, into a function the app calls                                   |
| `erb next`           | Runs to the next statement, over a function the app calls                                   |
| `erb finish`         | Runs until the function returns to its caller                                               |
| `erb faults [MODE]`  | Stops when the app faults or a load is refused: `on`, the default, or `off`                 |

The last six need the hook. `help erb` lists them in GDB.

- **A state's name** is the one the source gives it. A component's state reads `Counter.taps`, and
  `Counter:16.taps` or `Counter:16[2].taps` when more than one instance has it.
- **`erb set`** takes numbers, `true` and `false`, a string, and an enum's value by its text. A list can't be set.
- **`erb watch`** uses one of the chip's hardware watchpoints. A list stops when rows are added or removed, not
  when a row's field changes.
- **A breakpoint set before the app loads** is written when it does, so one set before `continue` on a board that
  has just reset catches the app's first code.
- **A step past the end of the app's code** stops at the next statement the app runs, whatever runs it: the next
  tap, timer or update.
- **`bt`** shows the app's line beside the VM's frame, `erb_vm_run [App.tsx:41  onPress]`, so a stop in C that the
  app called reads from the driver up to the app's line.

GDB's own commands still work on the firmware: `break`, `step`, `print`, `watch`, `bt`.

A session on a counter:

```text
(gdb) erb break App.tsx:34
erb breakpoint 1 at App.tsx:34
(gdb) continue
erb breakpoint 1, App.tsx:34  onPress
34	    setTotal(total + step);
(gdb) erb locals
step = 1
(gdb) erb state
total = 0
even = false
(gdb) erb next
App.tsx:35  onPress
35	    setEven((total + step) % 2 === 0);
(gdb) erb finish
onPress returns to the runtime
```

## Example: an app that faults

The screen goes to the error screen after a tap. With the hook, the board stops by itself when the app faults,
before the runtime tears the app down:

```text
(gdb) continue
App fault: list read past its end
  check: list row past the list's end
  at App.tsx:18  onPress
  build 1a2b3c4d
18	    setPicked(items[row].n);
(gdb) erb locals
row = 5
```

The handler on line 18 read row 5 of a list that has fewer. `erb where`, `erb locals` and `erb state` read the app
as it was when it faulted. `continue` runs the firmware on to its error screen, and a step has nowhere to go.

A fault the app's code didn't raise, such as state that keeps changing in an effect, stops the same way with its
reason, but with no line of the app's to show.

A refused load stops too, with `Load refused:` and what `erb error` says. `erb faults off` turns both stops off.

Without the hook nothing stops the board. Halt it and ask `erb error`, which keeps the reason and the line.

## Example: a package the firmware refuses

The board shows the error screen after a new package is flashed, and the screen's text is cut short or nobody is
near the panel. Attach, and ask:

```text
(gdb) erb error
needs more than the firmware reserves
  arena: needs 57552, the firmware reserves 32768
  check: needs more than the firmware reserves
  build 6a5dbb3b
The app was not loaded.
```

The app needs a 57,552-byte arena and this firmware gives it 32,768. Either the firmware's arena grows, or the app
shrinks. The first word names what ran out: the arena, nodes, vector nodes, animations, images or fonts.

Other refusals read the same way. `no layout for this screen` prints the screen's size, `built for another engine
ABI` the ABI the package wants, and a verifier rule the check that failed.

A firmware with two package slots falls back to the last good package when it refuses a new one, and clears the
refused slot at that boot. After the next reset the old app loads cleanly and `erb error` has nothing to show. The
ESP32-S3's debug server resets the chip as it starts, so on that board a refusal with a good package behind it is
read from the error screen or the log, not the probe.

A firmware with the hook stops by itself at each package it refuses. Without the hook, put a C breakpoint on the
load and step out of it. It stops once for each package the firmware tries:

```text
(gdb) break erb_load
(gdb) continue
(gdb) finish
Run till exit from #0  erb_load (...)
Value returned is $1 = false
(gdb) erb error
```

## Example: a state that changes when it shouldn't

A thermostat's target drops back to 18 now and then, and nothing in the app looks like it sets it. Watch it:

```text
(gdb) erb watch target
Watching target (watchpoint 2); it is 21
(gdb) continue
target: 21 → 18, written at App.tsx:96  applySchedule
(gdb) erb where
App.tsx:96  applySchedule
  called from App.tsx:142  onPress
```

The board stopped on the write. A handler on line 142 called `applySchedule`, which wrote the target: the
schedule button is the culprit, and `erb locals 1` shows what its handler held when it made the call.

When the app's code didn't write it, the line says so:

```text
target: 21 → 18, written at the runtime (a host input, a load, or a reset)
(gdb) bt
#0  erb_input_set_int (...)
#1  thermostat_sync (...) at Src/thermostat.c:88
```

Here the firmware set it through an input, and the backtrace names the C that did.

## From an editor

The same session runs inside VS Code: breakpoints in the gutter of the app's source, the step buttons, and the
app's values in the Variables pane. It needs everything above, with GDB 14 or later and the hook in the firmware.

### Install the extension

The extension is in the repository, in `editors/vscode`:

```bash
cd editors/vscode
npm install
npm run package
```

That writes a `.vsix`. Install it with **Extensions: Install from VSIX**. The extension only registers the
`embedded-react` debug type. The debug adapter is in your project's own `embedded-react` package, so it always
matches the compiler that wrote `app.erdbg`.

### A launch configuration

Add one to `.vscode/launch.json` for your board. Each starts the GDB server with the session and ends it after.

An STM32 with an ST-Link:

```json
{
  "type": "embedded-react",
  "request": "attach",
  "name": "Debug the app on the STM32",
  "program": "${workspaceFolder}/build/firmware.elf",
  "debugFile": "${workspaceFolder}/dist/app.erdbg",
  "gdb": "gdb",
  "server": [
    "openocd",
    "-f",
    "interface/stlink.cfg",
    "-f",
    "target/stm32h7x.cfg"
  ],
  "serverReady": "Listening on port 3333 for gdb connections",
  "target": "localhost:3333",
  "disconnectCommands": ["monitor resume"]
}
```

On a dual-bank STM32H7 the server's arguments gain `"-c", "set DUAL_BANK 1"` before the target file.

An ESP32-S3 over its built-in JTAG:

```json
{
  "type": "embedded-react",
  "request": "attach",
  "name": "Debug the app on the ESP32-S3",
  "program": "${workspaceFolder}/build-flow-b/embedded-react-esp32.elf",
  "debugFile": "${workspaceFolder}/dist/app.erdbg",
  "gdb": "xtensa-esp32s3-elf-gdb",
  "server": [
    "openocd",
    "-f",
    "board/esp32s3-builtin.cfg",
    "-c",
    "init; reset halt"
  ],
  "serverReady": "Listening on port 3333 for gdb connections",
  "target": "localhost:3333",
  "disconnectCommands": ["monitor resume"]
}
```

VS Code has to find Espressif's tools: start it from a shell with ESP-IDF's environment exported, or give the full
paths. Espressif's GDB picks its Python from the first `python3` on `PATH` and has none for a version it wasn't
built for. If the session fails to start, add an `environment` with a `PATH` that puts a Python it has first.

An RP2040 with a debug probe:

```json
{
  "type": "embedded-react",
  "request": "attach",
  "name": "Debug the app on the RP2040",
  "program": "${workspaceFolder}/build/embedded-react-watch.elf",
  "debugFile": "${workspaceFolder}/dist/app.erdbg",
  "gdb": "gdb",
  "server": [
    "openocd",
    "-f",
    "interface/cmsis-dap.cfg",
    "-f",
    "target/rp2040.cfg",
    "-c",
    "adapter speed 5000"
  ],
  "serverReady": "Listening on port 3333 for gdb connections",
  "target": "localhost:3333",
  "disconnectCommands": ["monitor resume"]
}
```

Once attached, the board keeps running; `"stopOnAttach": true` leaves it halted.
[The debug adapter](../internals/flow-b-packages.md#the-debug-adapter) lists every field.

### At a stop in the app

A breakpoint in the gutter of the app's source file stops on that line. One on a line with no code moves to the
next line that has some.

- **The call stack** has the app's frames on top, each a function of the app and its line. Under them is the C
  that ran the app: the VM, the runtime, and the firmware's frame loop.
- **The Variables pane** has **Locals**, the selected frame's parameters and locals, and **App state**, every
  state by name. A list opens to its rows. A state can be edited there, and the app runs its update when the
  board resumes.
- **The step buttons** step the app: over a statement, into a function the app calls, out to its caller.

A breakpoint with a condition isn't supported yet, and is shown as unverified.

### At a fault

When the app faults, the editor stops on the line that faulted and shows the reason as an exception. The call stack
and the Variables pane are those of the handler that faulted, as at a breakpoint. **Continue** runs the firmware on
to its error screen. A refused package stops the same way, with why it was refused.

**Stop on app faults** in the Breakpoints pane turns this off.

### At a stop in C

A breakpoint in a C file works as in any C debug session, and so does **Pause**.

- **The call stack** is the C call stack. When the app called the C, through a native module say, the app's
  frames sit inside the VM's frame, so the stack reads from the C down to the app's line that made the call.
- **The Variables pane** has the C frame's own variables, and **App state** on every frame.
- **The step buttons** step C.

## When it doesn't work

| Symptom                                                                         | Cause and fix                                                                                                                                                              |
| ------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `source` fails with `Error in sourced command file` and `Undefined command: ""` | This GDB has no Python. Use `arm-none-eabi-gdb-py`, a system `gdb` or `gdb-multiarch`, or Espressif's GDB with a `python3` on `PATH` it was built for                      |
| The editor's session ends as it starts                                          | The GDB is older than 14, or has no Python. `gdb --version`, then the row above                                                                                            |
| A breakpoint never fires on an ESP32-S3                                         | The chip has had a debug session since its last power-on reset, and the server couldn't read the flash mapping. End the server's arguments with `-c "init; reset halt"`    |
| A breakpoint never fires on a dual-bank STM32H7                                 | The server's memory map leaves out the second bank, so GDB writes a software breakpoint into flash. Add `-c "set DUAL_BANK 1"` before the target file                      |
| `erb break` says the firmware has no breakpoint hook                            | Build the firmware with `ERB_DEBUG_HOOK=1`                                                                                                                                 |
| `the board runs build …, but dist/app.erdbg is build …`                         | The debug file is from another build. Load the one written with the package on the board, or flash the package you just built                                              |
| The debug file is another format, or the descriptor another version             | The script, the compiler and the firmware's runtime are from different releases. Use the script from the `embedded-react` release the firmware and the app were built with |
| `erb_debug doesn't hold the runtime's descriptor`                               | The ELF isn't the one the board runs                                                                                                                                       |
| The server won't start, or the editor says the port is in use                   | A debug server from an earlier session is still running and holds port 3333. End it                                                                                        |
| The board resets during a halt                                                  | The watchdog ran on: [While the board is halted](#while-the-board-is-halted)                                                                                               |
| The board stays frozen after GDB quits                                          | GDB left it halted. `monitor resume` before `disconnect`, or `disconnectCommands` in the editor                                                                            |
| `continue` says `Could not insert hardware watchpoint`                          | More states are watched than the chip has hardware watchpoints: an ESP32-S3 has two. Delete one                                                                            |

A board with no probe attached when it faulted has a different tool: the firmware records the fault, and
`embedded-react decode-fault` traces it on the host.
[A hard fault with no probe attached](../internals/flow-b-packages.md#a-hard-fault-with-no-probe-attached) has
the details, and [The debugger's descriptors](../internals/flow-b-packages.md#the-debuggers-descriptors) how the
script reads a board.
