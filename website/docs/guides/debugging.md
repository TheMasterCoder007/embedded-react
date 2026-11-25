---
title: 'Debugging on a board'
description: 'Debug a Flow B app on the board through a debug probe: GDB commands that read the app by its own names, expressions over its locals and state, breakpoints with conditions and stepping in its source, a stop when the app faults, and the same from VS Code.'
---

A Flow B app on a board can be debugged the way its firmware is: through a debug probe, with GDB. A script in the
npm package teaches GDB the app, so a halted board answers in the app's terms: the source line it is on, its state
and locals by name, the node tree, and why a package was refused. Breakpoints and stepping work on the app's source
lines, and C breakpoints work beside them in the same session.

Nothing here needs a serial port or a build of the app made for debugging. The commands that read run no code on
the board, so they work on a board that has stopped answering.

With no board at hand, the same editor session runs against the simulator:
[In the simulator, with no board](#in-the-simulator-with-no-board).

## What it needs

- **A probe and a GDB server for it.** An ST-Link on an STM32, the built-in JTAG on an ESP32-S3, a CMSIS-DAP debug
  probe on an RP2040. OpenOCD drives all three.
- **A GDB for the board, built with Python** (3.8 or later). `gdb -batch -ex "python print(1)"` prints `1` when it
  has one. Debugging from an editor needs GDB 14 or later. [A GDB for the board](#a-gdb-for-the-board) says which
  toolchain's GDB does.
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

### A GDB for the board

Not every toolchain's GDB has Python, and some are older than 14. As of November 2026:

| Toolchain                              | Its GDB                                                                                                                                                                                       |
| -------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Arm GNU Toolchain                      | `arm-none-eabi-gdb` has no Python. `arm-none-eabi-gdb-py` beside it has: on Linux from 13.3.Rel1, on Windows and macOS from 14.3.Rel1. Releases before 13.3.Rel1 have a GDB older than 14     |
| STM32CubeIDE and STM32CubeCLT          | No Python, in any release on any system. Use another GDB from this table                                                                                                                      |
| ESP-IDF                                | Works from ESP-IDF 5.2.2. ESP-IDF 5.1 and 5.2 to 5.2.1 have GDB 12                                                                                                                            |
| Pico SDK, from the VS Code extension   | On Windows and macOS it is the Arm GNU Toolchain's, in `~/.pico-sdk/toolchain`: use `arm-none-eabi-gdb-py` there, which SDK 2.3.0 and later have. On Linux it is the system's `gdb-multiarch` |
| `gdb-multiarch` on Debian and Ubuntu   | Works on Debian 13 and Ubuntu 24.04. Debian 12 has GDB 13 and Ubuntu 22.04 GDB 12                                                                                                             |
| `gdb` from Homebrew, on Apple silicon  | Works, for Arm boards too                                                                                                                                                                     |
| `gdb-multiarch` from MSYS2, on Windows | Works, with MSYS2's `mingw64\bin` on `PATH`                                                                                                                                                   |

`arm-none-eabi-gdb-py` doesn't carry its Python. It is linked against one version, which has to be installed:
Python 3.8 on Linux (`libpython3.8`) and Windows, and a Homebrew Python on macOS. It doesn't start without it, and
the loader's error names the version.

Espressif's GDB has a build for each Python from 3.8 up to the newest at its release (3.14 for GDB 17.1). It runs
the one for the first `python3` on `PATH`, or `python` on Windows, and falls back to a build with no Python
without saying so when that version is newer or older than its builds.

From an editor the debug adapter checks all of this before a session starts, and its error says which of these is
wrong and what to use. For Espressif's GDB it finds a Python the GDB has a build for by itself.

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

The Arm GNU toolchain's `arm-none-eabi-gdb` and STM32CubeIDE's have no Python. Use the Arm GNU toolchain's
`arm-none-eabi-gdb-py`, or your system's `gdb` or `gdb-multiarch`:
[A GDB for the board](#a-gdb-for-the-board). Attaching halts the board where it was, without a reset.

### ESP32-S3, over its built-in JTAG

The chip's own USB port is the probe. A port behind a USB-to-UART bridge isn't. With ESP-IDF's environment
exported:

```bash
openocd -f board/esp32s3-builtin.cfg -c "init; reset halt"
```

```bash
xtensa-esp32s3-elf-gdb build-flow-b/embedded-react-esp32.elf -ex "target extended-remote :3333"
```

Use Espressif's GDB: a stock GDB can't read this chip's registers. When it says it has no Python, the first
`python3` on `PATH` is a version it has no build for: put one it has first. The server resets the chip as it
starts, so the app isn't loaded yet when GDB connects. Set breakpoints, then `continue`.

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
| `erb print EXPR`     | An expression's value, over the current function's locals and the app's state               |
| `erb state [NAME]`   | Every state by name, or one. `erb state all` adds the state the compiler made               |
| `erb set NAME VALUE` | Sets a state, a field of a list's row, or a local                                           |
| `erb watch NAME`     | Stops when a state changes, and names the line that wrote it                                |
| `erb error`          | The last fault or refused load: the reason, the amounts, and the source line                |
| `erb pools`          | What the app and the engine use of their pools                                              |
| `er nodes`           | The node tree: each node's type, its rect on the screen, and the source line of its element |
| `erb break [LINE]`   | Stops at a source line (`Card.tsx:12`, or `41` in `App.tsx`). With no line, lists them      |
| `erb log LINE TEXT`  | Prints a message each time a source line is reached, without stopping                       |
| `erb delete [N...]`  | Deletes breakpoints by number, or all of them                                               |
| `erb step`           | Runs to the next statement, into a function the app calls                                   |
| `erb next`           | Runs to the next statement, over a function the app calls                                   |
| `erb finish`         | Runs until the function returns to its caller                                               |
| `erb faults [MODE]`  | Stops when the app faults or a load is refused: `on`, the default, or `off`                 |

The last seven need the hook. `help erb` lists them in GDB.

- **A state's name** is the one the source gives it. A component's state reads `Counter.taps`, and
  `Counter:16.taps` or `Counter:16[2].taps` when more than one instance has it.
- **`erb set`** takes a name with members and indexes, as [`erb print`](#expressions) reads them: `count`,
  `box.pos.x`, `rows[2].done`, `item.label`. The value is a number, `true` or `false`, a string, or an enum's
  value by its text; `undefined` takes away a field a row may leave out. After a state or a row's field is set
  the app runs its update at its next tick, as after a setter. A local is the value the next statement reads. A
  whole list, row or object can't be set, only what it holds.
- **`erb watch`** uses one of the chip's hardware watchpoints. A list stops when rows are added or removed, not
  when a row's field changes.
- **A breakpoint set before the app loads** is written when it does, so one set before `continue` on a board that
  has just reset catches the app's first code.
- **A step past the end of the app's code** stops at the next statement the app runs, whatever runs it: the next
  tap, timer or update.
- **`bt`** shows the app's line beside the VM's frame, `erb_vm_run [App.tsx:41  onPress]`, so a stop in C that the
  app called reads from the driver up to the app's line.

GDB's own commands still work on the firmware: `break`, `step`, `print`, `watch`, `bt`.

### Expressions

`erb print` evaluates an expression as the app's source writes it, over the current function's parameters and
locals and the app's state:

```text
(gdb) erb print item.label
item.label = "two"
(gdb) erb print count + 1
count + 1 = 6
(gdb) erb print rows.length > 3 && !rows[2].done
rows.length > 3 && !rows[2].done = true
```

An expression takes names, members and indexes (`item.label`, `rows[2].done`, `rows.length`), numbers, strings,
`true`, `false`, `null` and `undefined`, and JavaScript's arithmetic, comparison, logical, bitwise and conditional
operators, with `?.` and `??`. A row's optional field that it leaves out is `undefined`, and so is a row past the
list's end.

Numbers are worked out as JavaScript does, in the debugger. The app's whole numbers saturate at the ends of their
range, so an expression can differ from the app there: `count + 1` goes past the largest int where the app stays
on it.

It reads the board and runs nothing on it, so it can't call the app's functions or a native module, and it can't
assign. The error names what it met:

```text
(gdb) erb print setCount(5)
`setCount(5)` has a call, which the debugger doesn't evaluate: it reads names, members and indexes, literals and
operators, and runs nothing on the board
```

An object reads as its fields, `box.pos.x`, for an object state, an object a handler declares, and a row's
nested fields. A field that is a constant shows its value and can't be set: the compiler folded it into the code.
A local the compiler folded away with no value of its own, an animation or an inlined helper's parameter, shows
as `<optimised out>` and can't be used in an operator.

### Conditions, hit counts and logpoints

A handler that runs sixty times a second can be stopped on the one pass that matters:

```text
(gdb) erb break App.tsx:34 if count === 5
erb breakpoint 1 at App.tsx:34  if count === 5
(gdb) erb break App.tsx:41 hits 3
erb breakpoint 2 at App.tsx:41  hits 3
(gdb) erb log App.tsx:34 pressed {count} of {rows.length}
erb breakpoint 3 at App.tsx:34  log pressed {count} of {rows.length}
```

- **`if EXPRESSION`** stops when the expression is true, read as `erb print` reads it. One that can't be worked
  out at a pass stops there and says why.
- **`hits N`** stops from the Nth pass on. `hits ==N` stops on that pass only, `hits >N` and `hits <N` after and
  before it, and `hits %N` on every Nth. With a condition too, only the passes it holds at are counted.
- **`erb log`** prints its message at each pass and doesn't stop. Each `{expression}` in it is worked out.

The debugger tests these, not the board. Each pass halts the board, reads what the expression names, and resumes
it, so a condition on a line that runs every frame slows the app while it is set.

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

A fault the runtime finds between runs of the app's code stops the same way with its reason. State that keeps
changing in effects shows the line where the last effect changed it; others have no line of the app's to show.

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
paths. The adapter finds a Python for Espressif's GDB itself, the one ESP-IDF's installer made or another on
`PATH`, and the Debug Console says which.

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
  state by name. A list opens to its rows, a row to its fields, and an object to its fields; a long list opens a
  page at a time. **Set Value** edits a state, a row's field or a local: the app runs its update when the board
  resumes, and a local is the value the next statement reads. An enum's type lists the values it takes.
- **The step buttons** step the app: over a statement, into a function the app calls, out to its caller.

- **Hover and Watch** take an [expression](#expressions): hover over `item.label` in the source, or watch
  `rows.length > 3`. A watched expression that names only state still reads at a stop in C. In the Debug Console,
  `erb print count + 1` evaluates in the frame selected in the call stack.
- **A breakpoint's condition, hit count and log message** work as
  [`erb break` and `erb log`](#conditions-hit-counts-and-logpoints) do: **Edit Breakpoint** in the gutter. A
  logpoint's message goes to the Debug Console. One the debugger can't read is shown as unverified, with why.

### When a state changes

**Break on Value Change** on a state in **App state** stops the board when the app changes it, as
[`erb watch`](#example-a-state-that-changes-when-it-shouldnt) does. It needs no hook in the firmware.

- **The stop** shows the old and the new value, `target: 21 → 18`, and the editor opens the line that wrote it.
  When the app has moved on from that line by the stop, as it has for a string, the line has a frame of its own
  on top of the call stack. When the firmware wrote it, the call stack is the C that did.
- **The Breakpoints pane** lists each one, to turn off or remove.
- **A list** stops when rows are added or removed, not when a row's field changes.
- Each one uses a hardware watchpoint of the chip's: an ESP32-S3 has two. One past what the chip has is shown as
  unverified, with why.

They last for the session: set them again in the next one.

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

## In the simulator, with no board

The Flow B simulator runs the app as a package, in the same runtime a board runs, and an editor debugs it there
with nothing else installed: no probe, no GDB and no firmware.

```bash
npx embedded-react dev --aot
```

That compiles the app, serves the simulator's page, and reloads the app on every save. `--port` picks the port
(3333 when left out), and `--screen 800x480` the panel's size. Without `--screen` the panel is the size the app's
first layout was built for, and the page's size menu changes it.

With the [extension](#install-the-extension) installed, this configuration starts the simulator on F5, shows it
in a split to the right of the code, and ends it with the session:

```json
{
  "type": "embedded-react",
  "request": "launch",
  "name": "Debug the app in the simulator",
  "simulator": true,
  "cwd": "${workspaceFolder}"
}
```

To debug a simulator you started yourself, attach to it:

```json
{
  "type": "embedded-react",
  "request": "attach",
  "name": "Attach to the simulator",
  "simulator": "localhost:3333"
}
```

The editor's side is what it is on a board: breakpoints with conditions, hit counts and log messages, the step
buttons, the app's frames, its locals and state in the Variables pane, hover and Watch, Break on Value Change on a
state, and the stop at a fault. What differs:

- **A stop freezes the panel.** The page dims it and says so. The app counts no time while it is stopped, so
  timers and animations carry on from where they were.
- **A save reloads the app during a session**, and the breakpoints move to the new build. The app starts over, as
  it does on a board. A save made while the app is stopped loads once it runs on.
- **There is no C.** The call stack has the app's frames alone, and the Debug Console evaluates the app's
  expressions directly.
- **Pause stops the app between its events**, where it has state but no locals.
- **A package that needs native modules doesn't load.** The simulator has none, and says which module is missing.

## When it doesn't work

| Symptom                                                                         | Cause and fix                                                                                                                                                              |
| ------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `source` fails with `Error in sourced command file` and `Undefined command: ""` | This GDB has no Python. Use one that has: [A GDB for the board](#a-gdb-for-the-board)                                                                                      |
| The editor's session fails to start, with an error about GDB                    | The error says what is wrong with the GDB and what to use. [A GDB for the board](#a-gdb-for-the-board) has the list                                                        |
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
| `No simulator at localhost:3333`                                                | An attach found no Flow B simulator there. Start one with `embedded-react dev --aot`, or use the launch configuration                                                      |
| A simulator launch says `Port 3333 is already in use`                           | A launch starts its own simulator. End the one that runs there, give the configuration another `"port"`, or attach to it instead                                           |
| The simulator says the prebuilt Flow B simulator is missing                     | The package was installed from source. Build it with `node tools/web-sim/build.mts`, which needs the Emscripten SDK                                                        |

A board with no probe attached when it faulted has a different tool: the firmware records the fault, and
`embedded-react decode-fault` traces it on the host.
[A hard fault with no probe attached](../internals/flow-b-packages.md#a-hard-fault-with-no-probe-attached) has
the details, and [The debugger's descriptors](../internals/flow-b-packages.md#the-debuggers-descriptors) how the
script reads a board.
