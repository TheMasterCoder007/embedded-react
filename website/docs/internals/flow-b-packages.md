---
title: 'Flow B packages'
description: 'The package the Flow B compiler emits and the runtime that verifies and runs it: container, program tables, memory, VM, verifier rules and fault policy.'
---

This page is the spec for Flow B's package: the file the compiler will emit in place of
`app.gen.c`, and the fixed runtime in the firmware that checks it and runs it. It is written for
the people building the loader, the VM, and the compiler's package output.

:::info[Runtime built, compiler output in progress]
The loader, verifier, and VM are in `runtime/` (`erb_*.c`; the wire format and opcode list are in
`erb_format.h`). `embedded-react build --aot` writes a package, with a layout for each panel size `--screen`
lists (`--screen 240x320,320x240`), and its debug table. It takes state, handlers, timers, effects, lists,
animations, PanResponder, `<Svg>`, `setKeyboardConfig` and values worked out from the screen size. `--release`
and `--bake` build the package too, and `embedded-react flash` writes it to a [slot](#package-slots). Once
packages pass the same tests as the C output (`--c`), the C backend is retired.
:::

## In one picture

```text
JSX ─► AOT compiler ─► typed IR ─► app.erpkg ──────────────────────────────┐
                                    tag "erb"                              │ flash slot, baked
                                    section 2: asset pack (ERPK)           │ array, or a copy
                                    section 4: program                     │ in RAM
                                    section 5: debug table (optional)      │
                                                                           ▼
             firmware:  erb_load()  ─►  verify everything  ─►  arena  ─►  VM  ─►  er_scene.h
                                        (once, at load)       (fixed)
```

The goals that shape every choice below:

- **One firmware, many apps.** The UI is data. Changing it needs no firmware rebuild.
- **No heap.** The firmware reserves one arena at compile time, and the package states exactly how
  much of it, and of each engine pool, it needs.
- **Read in place.** Code, tables, strings, and assets stay in memory-mapped flash. Only mutable state
  takes RAM.
- **Check once.** Everything that can be checked is checked at load, before a node is built. What is
  left for run time is a short, fixed list (see [Run-time checks](#run-time-checks)).
- **Nothing in a package can crash the device.** A failure shows the error screen with the reason,
  never a hard fault, a freeze, or a silent drop.
- **One set of semantics.** The VM keeps the C output's rules exactly (saturating arithmetic, the
  fixed limits and how they are reported), so the C backend can be retired without changing what any
  app does.

## The container

A package is an ERCF container, the same `.erpkg` format Flow A uses, read by the same
`er_container_read()` in `runtime/`. The container format stays at version 1; Flow B only adds
section types, and loaders already skip types they don't know.

| Field               | Flow B value                                                                                       |
| ------------------- | -------------------------------------------------------------------------------------------------- |
| Magic, version, CRC | Unchanged: `ERCF`, 1, CRC-32 over everything after the CRC field                                   |
| Tag                 | `erb`. A Flow A firmware rejects it at its tag check, and the Flow B runtime rejects anything else |
| Section 0, padding  | As today                                                                                           |
| Section 2, assets   | The ERPK asset pack, unchanged, registered in place                                                |
| Section 4, program  | **New.** The Flow B program, described below. Exactly one                                          |
| Section 5, debug    | **New, optional.** Bytecode offsets to source lines, for each panel. `--release` leaves it out     |

Sections 1 and 3 (QuickJS bytecode) never appear in a Flow B container. The emitter writes the asset
pack, then the program, then the debug table.

**Alignment.** Every section that is read in place now starts on a 4-byte boundary relative to the
container, not just the asset pack, and the emitter inserts padding sections to get there. The host
places the container at a 4-byte-aligned address, as it already must. Inside the program section,
every table starts 4-aligned and every record is a multiple of 4 bytes. This is not a nicety: the
RP2040's Cortex-M0+ faults on an unaligned word load, and the VM reads words straight from flash.
Little-endian throughout, like the rest of ERCF. A 64-bit value is stored as two 32-bit words, low
word first, so it never needs 8-byte alignment.

## The program section

The program section starts with a fixed header, then three arrays: the declared panels, the
resources the package needs, and a directory of tables. Offsets are from the first byte of the
section.

### Header

| Offset | Type  | Field          | Meaning                                                                                      |
| ------ | ----- | -------------- | -------------------------------------------------------------------------------------------- |
| 0      | `u16` | `format_major` | Package format and instruction set. The runtime accepts only its own major                   |
| 2      | `u16` | `format_minor` | Additions within a major. The runtime accepts a minor up to its own                          |
| 4      | `u16` | `engine_abi`   | The engine ABI level the package was built against (see below)                               |
| 6      | `u16` | `flags`        | Bit 0: debug build (decides the stale-handle policy). Other bits must be 0                   |
| 8      | `u32` | `build_id`     | CRC-32 of the program section with this field as 0. The debug section carries the same value |
| 12     | `u32` | `arena_bytes`  | Arena bytes needed: the largest of any panel                                                 |
| 16     | `u16` | `node_count`   | Engine nodes needed: the largest of any panel                                                |
| 18     | `u16` | `panel_count`  | Entries in the panel array, at least 1                                                       |
| 20     | `u16` | `need_count`   | Entries in the needs array                                                                   |
| 22     | `u16` | `table_count`  | Entries in the table directory                                                               |

Then, in order:

- **Panels**, 16 bytes each: `width u16`, `height u16`, `arena_bytes u32`, `node_count u16`,
  `root_template u16`, `boot_function u16`, `update_function u16` (`0xFFFF` for none). One entry per
  declared panel size, each compiled as the C output is for that size. Tables that are the same for
  every panel are stored once and shared; one that differs, such as a panel's templates, is stored
  per panel. Every panel's functions point into one `code` and one `types` table, so a function body
  that is the same in several panels is stored once.
- **Needs**, 8 bytes each: `resource u16`, `reserved u16`, `amount u32`. The engine pools the package
  uses besides nodes: vector nodes (1), animation values (2), concurrent animations (3), animation
  groups (4), registered images (5), fonts (6), glyphs (7) and extra glyphs (8), each at most once.
  A firmware that doesn't know a resource id rejects the package, so a later pool can be added
  without a format change.
- **Table directory**, 12 bytes each: `id u16`, `panel u16`, `offset u32`, `length u32`. `panel`
  (since minor 4) is the panel the table is for, or `0xFFFF` for every panel. A panel sees at most one
  table of each id, its own or the shared one, and the runtime skips the rest. Panels whose copies of a
  table are the same list one entry each, at the same bytes. Before minor 4 the field was reserved, and
  every table is every panel's.

**The engine ABI level** is a number the engine bumps when it adds a prop, node type, event, animated
property, or enum value, and it raises its oldest supported level when one is removed or changes
meaning. A package is accepted when `ER_ABI_MIN <= engine_abi <= ER_ABI`. The level is a fast,
nameable rejection ("built for engine ABI 9, this firmware runs 6 to 8"); the verifier still checks
every id the package uses against the firmware's own tables.

The package never encodes a struct layout from `er_scene.h`. Props are named by ABI id, and the
runtime maps each id to its `ERProps` field with a table built from `offsetof`. So a firmware built
with a different `ER_TEXT_MAX` or a reordered `ERProps` runs the same package.

### Tables

Each table is an array of fixed-size records unless it says otherwise. Fields are listed in order.
An index into another table is a `u16`, and `0xFFFF` means none.

Every record's size is a multiple of 4. A table with a header array starts with the array's length,
`count u32`, and its records follow the array. A table whose bytes don't end on a multiple of 4
(`strings`, `types`) is padded with zeros.

| Id  | Table       | Records                                                                                                                                                                                                                                                                  |
| --- | ----------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| 1   | `strings`   | `count u32`, then `count` offsets (`u32`), then the bytes. Each string is UTF-8 and NUL-terminated, so it can be handed to the engine in place                                                                                                                           |
| 2   | `const32`   | 32-bit constants, int or float bits. The instruction that loads one says which                                                                                                                                                                                           |
| 3   | `const64`   | 64-bit constants, two words each                                                                                                                                                                                                                                         |
| 4   | `arena`     | The arena's regions: `kind u16`, `count u16`, `offset u32`, `bytes u32`. See [The arena](#the-arena)                                                                                                                                                                     |
| 5   | `slots`     | One per state value, ref, boot value and runtime bookkeeping value: `type u8`, `flags u8`, `dirty_bit u16`, `offset u32`, `init u32`, `cap u16`, `aux u16`. `init` is an immediate, a constant index or a string index, by type. `aux` names an enum set or a row layout |
| 6   | `enums`     | Interned string sets: a header array of `first u16`, `count u16`, then the sets' string indices (`u16`), padded to a multiple of 4                                                                                                                                       |
| 7   | `rows`      | List row layouts: a header array of `size u16`, `first u16`, `field_count u16`, `reserved u16`, then 8-byte fields: `type u8`, `reserved u8`, `cap u16`, `offset u32`                                                                                                    |
| 8   | `props`     | Constant prop sets: a header array of `first u16`, `count u16`, then 8-byte entries: `prop u16`, `kind u8`, `reserved u8`, `value u32`                                                                                                                                   |
| 9   | `templates` | Tree templates: a header array of `first_node u16`, `node_count u16`, `kind u16` (root, branch or list row), `reserved u16`, then 12-byte node records                                                                                                                   |
| 10  | `bindings`  | What a template node is wired to: `kind u8`, `event u8`, `target u16`. An event to a function, a responder query to a predicate, an animated value to a prop, a vector tape                                                                                              |
| 11  | `functions` | `code u32` (word offset into `code`), `words u32`, `types u32` (offset into `types`), `registers u8`, `params u8`, `result u8`, `kind u8`                                                                                                                                |
| 12  | `types`     | Register types, one byte per register, for every function                                                                                                                                                                                                                |
| 13  | `code`      | The bytecode, 32-bit words                                                                                                                                                                                                                                               |
| 14  | `anims`     | Animation descriptors, 40 bytes each. See [Animations and vector tapes](#animations-and-vector-tapes)                                                                                                                                                                    |
| 15  | `tapes`     | A tape directory, then the op-tape floats, paints, gradients and stops. See [Animations and vector tapes](#animations-and-vector-tapes)                                                                                                                                  |
| 16  | `imports`   | Native functions the package calls: `name u16`, `version u16`, `signature u16`, `reserved u16` (string indices and a version number)                                                                                                                                     |
| 17  | `inputs`    | Host-fed values (`useHostValue`): `name u16`, `slot u16`                                                                                                                                                                                                                 |
| 18  | `interps`   | Interpolations an animated value is bound through, 72 bytes each (since minor 1). See [Animations and vector tapes](#animations-and-vector-tapes)                                                                                                                        |
| 19  | `keyboard`  | The on-screen keyboard's look and layout (since minor 3). See [The keyboard](#the-keyboard)                                                                                                                                                                              |

A template node record is `type u8`, `flags u8`, `children u16`, `props u16`, `handle u16`,
`bindings u16`, `binding_count u8`, `reserved u8`. Nodes are listed in pre-order with a child count,
so the builder needs no recursion and no pointers. `props` is the node's constant prop set and
`handle` the slot that keeps the node's handle when code needs to reach it later. In a row template
that slot is an array (`cap` handles), and each instance's handle goes in the element `BUILD` names.
No node flags are defined yet, so `flags` is 0.

The codes these records hold, as `runtime/erb_format.h` defines them:

- **Strings** are in order: each starts after the previous one's NUL, so checking them is one pass.
- **Value types**, for slots and registers: int (1), bool (2), float (3), int64 (4), double (5),
  string (6), enum (7), node (8), anim (9); slots also have animated value (10) and list (11). In the
  `types` table, 0 marks the second cell of a 64-bit register.
- **Slot flags**: bit 0 tracked (has a dirty bit), bit 1 written only by the boot function.
- **Arena regions** (`kind`, and what `count` is): slots (1), dirty bits (2, the bit count; kept twice,
  live and the pass's copy), string buffers (3), list pools (4), node handles (5), timers (6, 16 bytes
  each), running animations (7, 20 bytes each), register stack (8, cells), tape scratch (9, floats),
  event queue (10, 32 bytes each). Each kind at most once, in ascending order.
- **Bindings**: an event (1; `event` is the `EREventType`), a responder query (2), an animated value
  (3; `event` is the prop id, `target` the value's slot), a tape committed when the node is built (4),
  and an animated value through an interpolation (5, since minor 1; `event` is the prop id, `target` the
  interpolation). A color prop takes only an interpolation onto colors, as the engine binds one.
- **Function kinds**: below `0x40`, a handler for that `EREventType`; `0x40` plus an
  `ERResponderQuery`, a query (returns a bool); then timer (`0x80`), effect (`0x81`), update
  (`0x82`), boot (`0x83`), helper (`0x84`) and animation done (`0x85`). Only a helper can be called.
  An entry point takes no parameters, except a handler's or query's one int: the row instance it was
  built for. `params` counts register cells.
- **Inputs** are sorted by name, so names are unique, and the runtime can search them.
- **Prop ids** are the engine ABI's, listed in `runtime/erb_props.c`.

**What is data and what is code.** The tree's shape, every constant prop, the animation configs, the
static vector tapes, and the strings are data, applied by the runtime in tight C loops. Only what
depends on state is bytecode: handlers, effects, updates, boot-time values and the props and tape
points they compute. The [performance spike](#performance) is why: constant props as a table are a
large part of the gap between a naive VM and one within a few percent of C.

**Interned enums.** A string state whose every write is one of a closed set of literals (`page`,
`mode`) is an int slot holding an index into an `enums` set. Comparing two of them is an int compare,
and a `display` or `flexDirection` it drives is a table lookup. Only a string that is built at run
time, or read from data, takes a string buffer.

### Animations and vector tapes

An animation descriptor is `kind u8`, `easing u8`, `flags u8`, `reserved u8`, `target u16`, `prop u16`,
`first u16`, `count u16`, `amount u32`, `delay_ms u32`, `to u32`, then four parameters `param0`–`param3` (`f32`).

- `kind` is timing, spring or decay for a leaf, and sequence, parallel, stagger, or loop for a group.
- `target` is the slot holding the node or animated value, and `prop` the animated prop's ABI id
  (unused for an animated value). `easing` is the engine's `ERAnimEasing`.
- A group's children are `count` records starting at `first`, all after the group itself, so the
  descriptors form trees. A leaf has none.
- `amount` is the duration in ms for timing, the gap in ms for stagger, and the iterations for loop (0
  repeats forever). A timing leaf with no target (`0xFFFF`) is a delay of `amount` ms.
- Groups' child ranges are in ascending order and don't overlap, so no descriptor has two parents.
  A tree `ASTART` starts has at most 32 descriptors, laid out in a block of running-animation entries.
- A loop over an animated value's leaf sets the value back to where the loop started before each pass.
- `to` is the target value (`f32`). Flag bit 2 means it comes from a register instead: `ASTART` passes
  those in leaf order. Bits 0 and 1 are the engine's `loop` and `loop_reverse`.
- `param0`–`param3` are stiffness, damping, mass and velocity for spring, velocity and deceleration for decay,
  and the four control points for a bezier easing.
- A parallel or stagger group is done when every child is, and done as not finished as soon as one child
  stops; the others run on. This is how the C output ends a run joined at run time.
- Delay leaves and staggered waits count down in `erb_tick()` before the timers, and every one counts down
  before any that ran out moves on, so a wait a completion starts there waits its full time. The C output
  counts its completion waits the same way.

An interpolation is `slot u16` (the animated value), `point_count u8` (2 to `ER_INTERPOLATE_MAX_POINTS`),
`flags u8` (bit 0: the outputs are ARGB colors), `extrapolate_left u8` and `extrapolate_right u8` (the
engine's `ERExtrapolate`), `reserved u16`, then 8 inputs (`f32`) and 8 outputs (`f32`, or `u32` colors). The
runtime copies it into an `ERInterpolation` or `ERColorInterpolation` when it binds the node.

The `tapes` table starts with `count u32` and the byte offsets of its paints, gradients, and stops
(`u32` each), then `count` 16-byte tape records: `floats u32` (byte offset), `float_count u16`,
`paint_first u16`, `paint_count u16`, `grad_first u16`, `grad_count u16`, `reserved u16`.

- The floats are an op-tape exactly as `er_node_set_vector_ops` takes it.
- A paint is 24 bytes: `fill u32`, `stroke u32`, `stroke_w f32`, `miter f32`, `cap u8`, `join u8`,
  `fill_rule u8`, `reserved u8`, `fill_grad i16`, `stroke_grad i16`.
- A gradient is 24 bytes: `type u8`, `stop_count u8`, `stop_first u16`, then `ax`, `ay`, `bx`, `by`
  and `radius` (`f32`). A stop is `color u32`, `position f32`.

The runtime copies paints and gradients into the engine's `ERVectorPaint` and `ERVectorGradient` when it
commits a tape, so the package doesn't depend on how the firmware lays those structs out.

A drawing state drives is a tape too, its constant floats, paints and gradients in place, and the rest 0. The
update copies it into the scratch with `VTAPE`, writes each float state drives with `VSET`, and commits it.
When state drives a paint or a gradient too, it sets those fields of the scratch's copy (since minor 3) and
commits with `VCOMMITP`, which keeps them:

- `VPAINT A, B, C` sets field `C` of paint `B` to register `A`: the fill (0) and stroke (1) colors, the stroke
  width (2, a float), and the fill (3) and stroke (4) gradients, 1-based or 0 for none.
- `VGRAD A, B, C` sets field `C` of gradient `B`: `ax`, `ay`, `bx`, `by` and `radius` (0 to 4, floats), then
  stop _k_'s color at `8 + 2k` and its position (a float) after it.

### The keyboard

The `keyboard` table is `setKeyboardConfig`: a 28-byte header, `layer_count u16`, `row_count u16`,
`key_count u16`, `row_height u16`, `grid_cols u8`, `key_gap u8`, `key_radius u8`, `font_size u8`, then the
panel, key, active key and label colors (`u32` each). A 0 size or color is the engine's default, and no
layers keeps its built-in layout. Then `layer_count` layers and `row_count` rows, each `first u16`,
`count u16` naming a run of the rows or keys after the ones before it, and `key_count` 8-byte keys:
`label u16` and `text u16` (strings, or `0xFFFF` for none), `type u8` (the engine's `ERKeyboardKeyType`),
`layer u8`, `span u8` and `highlight u8` (`0xFF` for never).

The runtime copies the layout into the engine's structs, the strings left in place, and hands it to
`er_keyboard_set_config()` once the root is built, as the C output's `er_app_build()` does. It hands the
engine back its own keyboard when the app is torn down.

### The debug section

Section 5 holds debug tables, one after another. Panels can have their own `functions` tables, so a table is
for every panel, or for one panel when the panels' tables differ. Each table has four parts, in order:

1. A 20-byte header: `build_id u32`, which must match the program's, `entry_count u32`,
   `function_count u16`, `panel u16` (`0xFFFF` for every panel), `strings u32`, the byte offset of the
   string table from the table's start, and `length u32`, the table's bytes, a multiple of 4.
2. `entry_count` 16-byte entries, sorted by function and offset: `function u16`, `reserved u16`,
   `offset u32` (in words), `file u16`, `column u16` (1-based), `line u32`. An entry marks where the code
   lowered from one statement or element starts.
3. One name per program function, in the `functions` table's order (`u16` each), padded to a multiple
   of 4.
4. The string table, laid out like the program's `strings` table. `file` and the function names are
   0-based indices into it.

Before format minor 5, the section is one table for all panels, with a 16-byte header that has a
`function_count u32` and no `panel` or `length`.

The error screen uses the panel's table to turn a fault's function and offset into `App.jsx:41`. Without
one, the screen shows the function index, the offset, and the `build_id`. `--release` writes the same bytes
beside the package instead, as `app.erdbg`, and clears the debug flag, so the host can still map a fault
back to the source.

A debug table that doesn't match its program, or is malformed, fails the load like any other
section: a package is checked whole.

## Memory

### Read-only, in place

The program section, the debug section, and the asset pack are used where they sit. On a board with
memory-mapped flash (the ESP32 family, the RP2040, an STM32H7 with QSPI in memory-mapped mode) that is
flash; the VM fetches instructions and constants through the cache, and the engine reads pixels and
glyphs the same way it does for Flow A today.

On a board whose flash can't be mapped (an external SPI flash without XIP, an SD card), the firmware
reserves a second static region and the board copies the container into it before calling the
loader. The loader doesn't know the difference: it gets a pointer and a length. The CRC is checked
over the copy, which is what runs.

**Baked mode.** A board with no spare partition links the package into the firmware as a const array:
`embedded-react build --aot --bake` writes `app.erpkg.c` with the bytes as `const uint32_t app_erpkg[]`,
so they are 4-aligned with any compiler, and their length as `app_erpkg_bytes`. It goes through
the same loader and the same verifier. This is what replaces the C backend on such boards, so there
is one set of semantics, not two.

### The arena

Everything mutable that belongs to the app lives in one block the firmware reserves at compile time:

```c
#define ERB_ARENA_BYTES (24 * 1024)
static uint32_t s_arena[ERB_ARENA_BYTES / 4];
```

The compiler lays the arena out completely: every region's offset and size is in the `arena` table,
and every slot's offset is in `slots`. The runtime never allocates; at load it checks the layout,
zeroes the arena, and writes the initial values. Every record the arena holds has a size fixed by the
package format, not by the firmware's structs, so `arena_bytes` is exact on every board.

| Region            | Holds                                                                                        |
| ----------------- | -------------------------------------------------------------------------------------------- |
| Slots             | State, refs, boot-time values, effect dependencies and armed flags, gesture state, timer ids |
| Dirty bits        | One bit per tracked slot                                                                     |
| String buffers    | One per string slot or row field, each its own cap plus a NUL                                |
| List pools        | Per list: a row count, then `cap` rows of its row layout                                     |
| Node handles      | A handle per node that code reaches, and per row and branch instance                         |
| Timers            | The timer table, 16 bytes a timer, sized by a compiler option                                |
| Animation handles | Running animations and animated values the app owns, and completion waits                    |
| Register stack    | VM frames. Its size is the deepest call path the verifier finds, so it can't overflow        |
| Tape scratch      | One buffer the size of the largest state-driven vector tape                                  |
| Event queue       | Events that arrive while the VM is running (see [Running an event](#running-an-event))       |

Limits that were C defines in the generated code (`ER_AOT_MAX_TIMERS`, `ER_AOT_MAX_WAITS`) become
compiler options like `ER_AOT_LIST_CAP`: they size arena regions, so raising one needs no firmware
rebuild, only a package whose `arena_bytes` still fits.

**What is not in the arena.** The engine's pools (nodes, vector nodes, animations, the image registry,
fonts) stay where they are, sized by the firmware's `ERUI_*` flags; the package's needs are checked
against them. The runtime also keeps a few fixed statics of its own, the same size for every package:
one `ERProps` scratch, one paint table for a vector update, the current event's data, and, with the
engine's on-screen keyboard, a copy of a package's keyboard layout. They are part of the runtime's
footprint, not the package's.

Fonts need a change to the asset loader too. `er_assets_load_pack()` allocates each font's glyph tables
with `calloc`, and the Flow B runtime has no heap. It gets a variant that takes those tables from
storage the caller provides, arrays the firmware sizes (`ErbConfig::fonts`), and checked against the
package's font and glyph needs. The heap path lives in its own file (`er_assets_heap.c`), so a Flow B
firmware never links the allocator. The table layout depends on the firmware's structs, so it can't
live in the arena.

### The RP2040

On the slowest board the [spike](#performance) settled two placement rules:

- **The VM's dispatch loop runs from SRAM when it fits.** In flash, it and the engine evict each other from the
  16 KB XIP cache, which cost the spike's 2.1 KB loop about 6 points. The runtime marks the loop with
  `ERB_VM_HOT`, for a port to define to its RAM section. The whole instruction set makes it about 12 KB, more
  than the watch-face board has beside its framebuffer, so that example leaves it empty and the loop in flash.
- **Bytecode stays in mapped flash.** It is small and read once per instruction.

The interpreter doesn't recurse in C: a call pushes a frame on the register stack in the arena. So the
VM adds one fixed C frame to the stack, whatever the app does. That matters here: the main stack already
reaches about 7.5 KB, and `SCRATCH_X` is not free for code.

## The VM

### Values and registers

The VM is register-based. Each function declares how many registers it has and the type of each one, in
the `types` table, and a register keeps that type for the whole function. Registers are 32-bit cells; a
64-bit value or a node handle takes two adjacent cells, the first even.

| Type     | Holds                                                                                             |
| -------- | ------------------------------------------------------------------------------------------------- |
| `int`    | A 32-bit signed integer                                                                           |
| `bool`   | 0 or 1                                                                                            |
| `float`  | An IEEE single, as the C output uses                                                              |
| `int64`  | A 64-bit integer: times from `Date.now()` and `performance.now()`, and numbers past the int range |
| `double` | An IEEE double, only for values worked out at boot, where the C output also uses `double`         |
| `string` | A string reference: a constant's index, or a string buffer in the arena. Never a pointer          |
| `enum`   | An index into an interned set                                                                     |
| `node`   | A node handle: the engine's `ERNodeRef`, a pool tag and a creation serial                         |
| `anim`   | An animation or animated-value handle the app owns                                                |

There are no pointers anywhere in the VM's values. A string, a node, a list row, and a function are all
reached by index, through tables the verifier has checked or handles the runtime checks.

### Instructions

Every instruction is one 32-bit word, little-endian: the opcode in bits 0–7, `A` in 8–15, then either
`B` in 16–23 and `C` in 24–31, or a 16-bit `Bx` in 16–31. `Bx` is unsigned for an index and two's
complement for a branch offset, which counts words from the next instruction. An instruction that needs
more takes one extension word after it.

Each opcode has one fixed form and length, from one opcode list the compiler and the runtime share:
`ERB_OPCODES` in `runtime/erb_format.h`, which also gives each opcode's operands and extension word.
So the verifier walks a function from its first word and knows where every instruction starts; an
extension word is never an instruction, and a branch into one is rejected. The families:

| Family        | Instructions                                                                                                                                                                                     |
| ------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Moves         | `MOVE`, `LOADI` (signed 16-bit immediate), `LOADK`, `LOADK64`, `LOADS` (a string constant), `LOADE` (an enum value)                                                                              |
| Slots         | `GET`, `SET` per slot type. `SET` on a tracked slot sets its dirty bit when the value changes                                                                                                    |
| Lists         | `LEN`, `ROWGET`, `ROWSET`, `APPEND` (a row from consecutive registers), `CLEAR`, `POP`, `KEEP`, `SLICE`                                                                                          |
| Whole numbers | `ADD`, `SUB`, `MUL`, `NEG`, `ABS`, `MIN`, `MAX`, `DIV`, `FLOORDIV`, `CEILDIV`, `ROUNDDIV`, `MOD`, each in 32 and 64 bits                                                                         |
| Floats        | `FADD`, `FSUB`, `FMUL`, `FDIV`, `FNEG`, and `FMATH` with a function: `sin`, `cos`, `tan`, `sqrt`, `abs`, `round`, `floor`, `ceil`, `min`, `max`, `atan2`, `pow`, `mod`; the same set on `double` |
| Conversions   | `ITOF`, `FTOI`, `LTOF`, `ITOL`, `LTOI`, `DTOI`, `FTOD`, `DTOF`                                                                                                                                   |
| Comparisons   | `EQ`, `LT`, `LE` per numeric type, `SEQ` for strings, `NOT`, giving a `bool`                                                                                                                     |
| Control       | `JMP`, `JT`, `JF` (forward only), `JDIRTY` (any bit of a dirty set), `LOOPSET`, `LOOP` (the only backward branch), `RET`                                                                         |
| Calls         | `CALL` a package function, `CALLN` a native import                                                                                                                                               |
| Text          | `FMT` into a string buffer, a node's text or a span: literal, int, int64, float (as JS prints it), string, boolean word; `toFixed`, `padStart` and the other string helpers as they land         |
| Props         | `PROPS` (start the scratch from a constant prop set), `PSET` per value kind, `SPANS`, `COMMIT` (to a node)                                                                                       |
| Nodes         | `BUILD` (a branch or row template), `DESTROY`, `SCROLL`, `FOCUS`, `BLUR`, `INPUT` (read a text input)                                                                                            |
| Timers        | `TSTART` (function, ms, repeat; gives an id), `TCLEAR`                                                                                                                                           |
| Animation     | `ASTART` (a descriptor, with `toValue`s from registers), `ASTOP`, `AVSET`, `AVGET`                                                                                                               |
| Vectors       | `VTAPE` (a tape and its paints into the scratch), `VSET` (a float of it), `VCOMMIT` (to a node, with the tape's paints), `VDIRTY`; since minor 3, `VPAINT`, `VGRAD`, `VCOMMITP`                  |
| Event data    | `EVT` (`x`, `y`, `dx`, `dy`, `vx`, `vy`, the fingers down since minor 2, a layout field), `NOW`, `DATENOW`                                                                                       |
| Faults        | `TRAP`, with a reason: what a lowered `throw` or an unreachable branch becomes                                                                                                                   |
| Effects       | `EFFECT` (queue an effect to run after this update pass)                                                                                                                                         |

The set covers what the compiler lowers today and what the parity work adds. A new instruction is a
minor version bump; an old runtime rejects a package that uses one at its version check.

`PSET` converts as the C output does: a float dimension through `app_round_dim`'s rule, an opacity to the
engine's byte through `app_opacity`'s, a color as ARGB. The engine's own range checks still apply on top.

### Arithmetic

The rules are the C output's, which are JavaScript's wherever an int can hold the answer:

| Operation                                | Rule                                                                                                                                            |
| ---------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------- |
| `+`, `-`, `*`, negation, `Math.abs`      | Saturate at the int range (the int64 range in 64 bits). `-INT_MIN` and `abs(INT_MIN)` are `INT_MAX`                                             |
| `a / b` kept whole                       | Truncates toward zero. `b == 0` saturates by the sign of `a`, and `0 / 0` is 0. `INT_MIN / -1` is `INT_MAX`                                     |
| `Math.floor`, `ceil`, `round` of `a / b` | Exact, from a 64-bit quotient. Halves round up. The zero divisor and range as above                                                             |
| `a % b`                                  | The dividend's sign, as JS and C agree. `b == 0` and `b == -1` give 0                                                                           |
| Float to int                             | Truncates toward zero, NaN is 0, out of range saturates                                                                                         |
| Float `Math.round`                       | Halves up, as JS rounds, not away from zero as C's `roundf` does                                                                                |
| Float maths                              | IEEE single, through the C library's float functions. Division by zero gives an infinity or NaN, and the conversion rules handle it at the edge |
| A dimension prop                         | Rounded half up to a whole pixel, clamped to the `int16` range, NaN as 0                                                                        |
| Comparisons with NaN                     | False                                                                                                                                           |

None of these is a fault. An app's arithmetic always has an answer, so it can't stop the app.

### Calls and the stack

`CALL` takes its arguments from consecutive registers, which become the callee's first registers; the
callee's other registers start at zero, and its result comes back in the first argument register. Calls
go only to package functions by index, or to native imports by index. There are no function values, so
the call graph is known at load.

The call graph must be acyclic; the compiler already rejects recursion. The package lists a callee after
every function that calls it, so a call to an earlier function, or to itself, is rejected at once, and
the verifier works out each function's deepest path in one pass from the last function back. It checks
the register stack holds the deepest path from any function; a frame is its registers plus three cells
of bookkeeping. A stack overflow can't happen at run time.

A native import is called with its arguments as 32-bit cells and returns one result. It can't call
back into the VM. Work that takes time reports back later by posting an input or an event, which the
runtime delivers like any other.

### Loops and the instruction budget

`LOOP` is the only backward branch. Its counter register is set by a `LOOPSET` with a constant bound and
written by nothing else, so every loop runs at most a known number of times. With an acyclic call graph
that makes the worst case of every entry point a number the verifier works out at load.

Each run of an entry point has an instruction budget, the firmware's `ERB_EVENT_BUDGET` (a CMake
option, 100,000 by default), counted in instructions executed. A run is one call from the runtime: a handler, one update
pass, one effect, or one queued event. An event as a whole is bounded by the pass limit in
[Running an event](#running-an-event), not by the budget.

The verifier works out each entry point's worst case in the same unit:
straight-line code counts its length, a loop its body's worst case times its bound, a call its callee's
worst case, and a `BUILD` the nodes its template creates.

When an entry point's worst case fits the budget, it runs with no counting at all. Otherwise the VM
counts exactly: at every branch, `LOOP`, `CALL` and `RET` it charges the instructions run since the
last one, and `BUILD` charges its nodes. Between those, code runs straight through, so the count costs a
subtraction per branch rather than per instruction. Running out is a fault.

The unit is an instruction, not time. A native import counts as one; its own time is the firmware's.

The worst case for every entry point also goes into the build's resource report, so an app that comes
near the budget finds out on the host.

### Running an event

An event is anything the host or the engine delivers: a touch callback, a responder query, a timer
firing, an animation finishing, a host input changing, the wall clock being set, or the boot itself. One
event runs like this:

1. The runtime enters the function bound to it, with the event's data readable through `EVT`.
2. When it returns, if any dirty bit is set, the runtime runs the panel's update function. Updates test
   their dirty sets with `JDIRTY` and re-set only the nodes that read a changed slot, then check effect
   dependencies and run the effects whose dependencies changed.
3. If the effects set state, the update runs again. More than 50 passes is a fault, the same limit at
   which React reports an update loop.
4. Events that arrived while the VM was running (an animation stopped by the handler finishing at once,
   a layout callback) were queued; each now runs in turn, with its own budget.
5. Control goes back to the host, which commits the frame as it does now.

The VM is never re-entered. That is what keeps the stack bound in the arena exact. A full event queue is
a fault: it only happens when events keep raising each other. A responder query must answer at once, so
one that arrives while the VM runs says no.

An effect is queued by `EFFECT` from the update (or from the boot function, for a mount effect) and runs
after the pass, as its own run with its own budget, before any other queued event.

### Strings and text

A string register is 0 (the empty string), a constant's index plus 1, or the top bit and a buffer's offset
in the arena; only the VM makes one, so it never points anywhere else. A string is a constant or a buffer.
Building one (a template literal, `toFixed`, a text node's content)
goes through `FMT` into a string buffer, a node's text, or a span, and each target has a cap. Past the cap
the string is cut short and reported, as the C output does today. A constant string is never copied:
the engine gets a pointer to it in flash.

Floats print as JavaScript prints them where it matters: `Infinity`, `-Infinity`, `NaN`, and `0` for
negative zero.

### Nodes and handles

The VM never holds an `ERNode*`. A node is reached through the engine's `ERNodeRef`: the 16-bit pool tag
and the 32-bit serial the node got when it was created, so a slot reused by a newer node no longer
matches. The engine's touch state holds nodes the same way, through `er_node_ref` and `er_node_deref` in
`er_scene.h`. A handle takes two register cells, or 8 bytes in a slot.

Every instruction that touches a node resolves its handle first, and so does an animation leaf when it
starts. An empty handle, on a node not built yet, only stops the leaf. A template's nodes get their handles
as they are built; a node that code reaches later (a ref, an updated prop, a row) keeps its handle in a
slot.

## Loading and booting

`erb_load()` does everything below in order, and builds nothing until every check has passed. Steps 1
to 6 only read the new package, so a reload that fails there leaves the running app exactly as it was.

1. Read the container: magic, version, section bounds, CRC (`er_container_read`).
2. Check the tag, and that there is exactly one program section and at most one asset pack and debug
   section.
3. Check the header: format, ABI level, flags.
4. Pick the panel that matches the size the firmware passes. A size the package doesn't declare fails
   here.
5. Compare `arena_bytes`, `node_count` and every need with what the firmware reserved.
6. Verify every table and every function (the [rules](#verifier-rules) below), check the asset pack
   without registering anything, and resolve every import and input.
7. On a reload, tear down the running app (below). Then register the asset pack in place; it was
   checked whole in step 6, so registering it can't fail part way.
8. Zero the arena and write each slot's initial value.
9. Run the panel's boot function: the values worked out from the real screen size.
10. Build the root template: create the nodes, apply each constant prop set, wire the bindings.
11. Run the update with every dirty bit set, so every state-driven prop gets its first value.
12. Run the mount effects, then set the root.

A failure at any step stops the load, with a reason the error screen shows. From step 7 on, the old
app is gone. A failure there (a `TRAP` in the boot function, or nodes the engine couldn't give, which
step 5 should prevent) tears down what was built and shows the error screen; it doesn't bring the old
app back.

**Teardown**, for a reload or a failed load: cancel the timers, stop the animations, destroy the
animated values and the nodes (`er_reset`), clear the image and font registries, and clear the arena.
Two pieces are missing from the engine and asset loader today:

- **Clearing the image registry.** `er_reset` keeps registered images, and registering a pack only
  replaces images by name. So images the old pack had and the new one doesn't would still point at the
  old pack.
- **Checking a pack without registering it.** `er_assets_load_pack` changes the registries as it parses,
  so step 6 needs a check-only pass, as `er_container_scan` is for the container.

Both are in now: `er_image_clear()` and `er_font_clear()`, and `er_assets_check_pack()`. A pack whose
image pixels aren't word-aligned (only a version 1 pack can have them) is rejected, since the engine
reads them in place as words.

## Verifier rules

Every rule here is checked once, at load. Each has a reason the error screen shows; the runtime's tests
build a package that breaks one rule and check it is rejected with that reason.

| Area      | Rule                                                                                                                                                                                                                                                                                  | Reason shown                       |
| --------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------- |
| Container | `ERCF` magic, version 1, every section in bounds, the CRC matches                                                                                                                                                                                                                     | not a package / corrupt            |
| Container | The tag is `erb`; one program section; at most one asset pack and one debug section; each 4-aligned                                                                                                                                                                                   | not a Flow B package               |
| Header    | `format_major` equal, `format_minor` not newer, flag bits known                                                                                                                                                                                                                       | built for a newer runtime          |
| Header    | `engine_abi` within the firmware's range                                                                                                                                                                                                                                              | built for engine ABI _n_           |
| Header    | A panel matches the real size                                                                                                                                                                                                                                                         | no layout for _w_ × _h_            |
| Header    | The panel's arena bytes, nodes and each need fit what the firmware reserves; every resource id is known and named once                                                                                                                                                                | needs _n_ B, firmware reserves _m_ |
| Tables    | Every table in the section's bounds, 4-aligned, for a declared panel or every panel; none of the panel's tables overlapping another or listed twice; a length that is a whole number of records                                                                                       | malformed table                    |
| Tables    | The required tables are present (`arena`, `slots`, `templates`, `functions`, `types`, `code`); no unknown table id                                                                                                                                                                    | malformed table                    |
| Strings   | Every offset in bounds and in order; every string NUL-terminated inside the table and valid UTF-8                                                                                                                                                                                     | malformed string                   |
| Arena     | Regions inside `arena_bytes`, aligned, not overlapping; their sum is what the panel declares                                                                                                                                                                                          | bad arena layout                   |
| Slots     | A known type; inside its region, aligned to its type; not overlapping; a dirty bit in range for tracked slots; a valid `init` for the type; an enum set or row layout that exists; no more animated values than the needs declare                                                     | bad state layout                   |
| Lists     | Row fields inside the row size, a known type, a cap for each string field; the pool holds `cap` rows                                                                                                                                                                                  | bad state layout                   |
| Props     | Every prop id is in the firmware's ABI table; the value's kind matches the prop's; an enum value in range; a string index valid and, for a text prop, not past the firmware's cap; an image name in the asset pack or built in                                                        | bad prop                           |
| Templates | Known node types; child counts consistent, so the pre-order list is one tree; a node count within the panel's; handle slots of type `node`; an element's children allowed for its type                                                                                                | bad template                       |
| Bindings  | Each event bound to a function of the matching kind; each animated binding to an animated-value slot and an animatable prop that isn't a color; each interpolated one to an interpolation onto colors exactly when its prop is a color; each tape index valid                         | bad binding                        |
| Interps   | An animated-value slot; 2 to `ER_INTERPOLATE_MAX_POINTS` points; known flags and extrapolation modes                                                                                                                                                                                  | bad animation                      |
| Anims     | A known kind and easing; a group's children are later records and no child has two parents; at most `ERUI_MAX_GROUP_ENTRIES` children; the target a `node` or `anim` slot; the prop animatable                                                                                        | bad animation                      |
| Tapes     | Each tape's floats parse as ops with their argument counts and fit `ERUI_VECTOR_TAPE_MAX`; paint and gradient counts within the firmware's caps; every paint, gradient and stop index in range; 2 to `ER_VGRAD_MAX_STOPS` stops                                                       | bad vector tape                    |
| Functions | Code in bounds; register count and types valid; parameters and result typed; ends in `RET` or `TRAP`, so it can't run off its end                                                                                                                                                     | bad function                       |
| Code      | Every opcode known to this format; every register operand below the frame size and of the type the opcode takes; `int64`, `double` and `node` registers on an even cell                                                                                                               | bad bytecode                       |
| Code      | Every slot, constant, string, enum, prop, template, animation, tape and import index in range, and of the kind the opcode takes; `SET` only to writable slots; an enum write from the slot's own set                                                                                  | bad bytecode                       |
| Code      | Branch targets inside the function, on an instruction; only `LOOP` branches backward, on a counter set by `LOOPSET` with a constant bound and written by nothing else                                                                                                                 | bad control flow                   |
| Code      | `EVT` only in a function the runtime enters with an event of that kind                                                                                                                                                                                                                | bad bytecode                       |
| Calls     | `CALL` targets a helper later in the table, with matching parameter and result types, so the call graph has no cycle                                                                                                                                                                  | recursive call                     |
| Calls     | The deepest frame sum from any entry point fits the register stack region                                                                                                                                                                                                             | stack too deep                     |
| Imports   | Every import resolves to a native function the firmware registered with the same name, version and signature                                                                                                                                                                          | missing native module _name_       |
| Inputs    | Unique names, sorted, each on a slot of a type the host can set                                                                                                                                                                                                                       | bad input                          |
| Keyboard  | Whole records; each layer and row names the runs after the ones before it, with 1 to 255 rows or keys; every key a known type with valid strings, and one that types something has its text                                                                                           | malformed table                    |
| Keyboard  | No more layers, rows and keys than `ERB_MAX_KEYBOARD_LAYERS`, `ERB_MAX_KEYBOARD_ROWS` and `ERB_MAX_KEYBOARD_KEYS`                                                                                                                                                                     | needs more than firmware reserves  |
| Debug     | Each table for a declared panel or all; one at most for this panel. Its `build_id` matches; parts in order, not overlapping; `function_count` is the program's; entries sorted, each on an instruction; each `file` and name an index into its strings, checked like `strings`        | debug table does not match         |
| Assets    | The pack passes the check-only pass (images have a size and word-aligned pixels, fonts are drawable); its fonts fit the firmware's font storage and the font registry                                                                                                                 | asset pack failed to load          |

### Run-time checks

Because of the rules above, the VM never re-checks an opcode, a register index or type, a branch target,
a table index from an operand, a call target, or the stack depth. What it does check, because it depends
on values only known at run time:

- a node handle's serial, on every node instruction;
- a list row index that comes from data, against the list's current length;
- a string's length against its target's cap, when it is built at run time;
- room in a list pool, the timer table and the completion waits;
- the instruction budget, only for entry points whose worst case exceeds it;
- the update pass limit and the event queue.

## Fault policy

There is one error screen for every failure an app can cause. It draws on its own path, with no
dependency on the app's tree and no allocation, and shows the reason, the source line when the package
has a debug table, and the amounts involved where they apply. Everything a package can do wrong ends in
one of three ways: the error screen, a reported skip that the app runs on past, or nothing at all.

| Failure                                                                        | When     | Effect                                                                                              |
| ------------------------------------------------------------------------------ | -------- | --------------------------------------------------------------------------------------------------- |
| No package installed                                                           | Boot     | Error screen                                                                                        |
| Any verifier rule, a size that doesn't fit, a missing import                   | Load     | Error screen. Nothing is built, and a reload leaves the running app as it was                       |
| A fault while booting (steps 9 to 12)                                          | Load     | Error screen. On a reload the old app is already gone                                               |
| Instruction budget exceeded                                                    | Run      | Error screen. The app's state can't be trusted after a handler stopped part way                     |
| More than 50 update passes, or a full event queue                              | Run      | Error screen: the app is in a loop                                                                  |
| A list read past the list's end                                                | Run      | Error screen, as the `TypeError` would be in Flow A                                                 |
| `TRAP`                                                                         | Run      | Error screen with the trap's reason                                                                 |
| A native import reports failure                                                | Run      | Error screen, naming the module                                                                     |
| A stale node handle                                                            | Run      | Debug package: error screen. Release package: the instruction does nothing, and it is reported once |
| A full list, a string cut short, a full timer table, too many completion waits | Run      | Skipped or cut short and reported once per place, exactly as the C output does today                |
| A node the engine can't give (a branch or row being built)                     | Run      | Error screen. The load-time node check makes this a firmware bug, not an app one                    |
| An image name that isn't registered                                            | Run      | The image doesn't draw, and it is reported once                                                     |
| Arithmetic of any kind                                                         | Never    | Always has a defined answer                                                                         |
| A fault in native code: an engine or firmware bug, a watchdog reset            | Hardware | Recorded in no-init RAM, shown on the error screen after the reset                                  |

"Reported" means one line through the runtime's log hook, once for each place in the app that can hit
it, naming the limit to raise, as the C output's `AOT:` lines do today.

A fault inside an engine callback (a touch handler, an animation finishing) is recorded at once, and the
next `erb_tick()` tears the app down and shows the error screen, so the engine is never reset from inside
its own dispatch.

The release/debug split for stale handles comes from the package's debug flag, not the firmware's, so
the same firmware runs a strict development package and a forgiving release one.

## Performance

The spike hand-translated the thermostat's `app_update` and all 11 of its handlers to register bytecode
and ran them on the RP2040 at 125 MHz, against the generated C on the same board. The VM's output was
checked against the C: all 665 `set_props` calls were byte-identical, and the state matched after every
step. Each case is timed over 200 iterations, with a per-instruction budget check on throughout.

| Dial drag, per step                | C        | VM, naive      | VM, designed         |
| ---------------------------------- | -------- | -------------- | -------------------- |
| Handler + update, VM loop in flash | 1.76 ms  | 2.20 ms (+25%) | 1.97 ms (+11%)       |
| Handler + update, VM loop in SRAM  | 1.76 ms  | 2.09 ms (+18%) | **1.85 ms (+5%)**    |
| Whole frame, with render, SRAM     | 28.57 ms | 28.71 ms       | **28.66 ms (+0.3%)** |

The step, mode-switch and no-change cases come out the same: the designed VM in SRAM costs 4 to 7%.

What the design takes from it:

- **Constant props are a table, not code,** and **string state with a closed set of literals is an
  interned int slot.** Together they are the difference between the naive and the designed column.
- **The dispatch loop goes in SRAM on the RP2040,** and bytecode stays in mapped flash.
- **The program is smaller than the C.** The designed program is about 3.0 KB (2.0 KB of bytecode and
  0.8 KB of prop tables). The C handlers and update are 4.2 KB, and the interpreter is 2.1 KB once per
  firmware.
- **The engine dominates either way.** `set_props` is about 1.2 of the 1.76 ms, mostly hashing a
  1,120-byte `ERProps` for 19 nodes that rarely change. Cutting that engine work helps both backends
  more than any VM tuning could.
- The spike's budget check ran on every instruction. The design charges it only where a worst case can
  exceed the budget, so the numbers above are an upper bound.

## From the C output to a package

What each part of today's `app.gen.c` becomes:

| In `app.gen.c`                                    | In a package                                                         |
| ------------------------------------------------- | -------------------------------------------------------------------- |
| `s_state`, `s_ref_*`, `s_eff*`, `s_pan_*`         | Slots in the arena                                                   |
| `s_changed` and `app_update()`                    | Dirty bits, and the update function with `JDIRTY`                    |
| `er_app_build()`'s node construction              | The root template and its constant prop sets, applied by the runtime |
| Handlers, timer callbacks, effects, cleanups      | Functions, bound to events in the `bindings` table                   |
| `app_add`, `app_floordiv`, `app_f2i` and the rest | Instructions with the same rules                                     |
| `snprintf` into `p.text`                          | `FMT` into the props scratch's text                                  |
| `s_timers`, `er_timer_add`, `er_app_tick()`       | The arena's timer table, ticked by `erb_tick()`                      |
| `APP_LIMIT`                                       | The runtime's report, once per place                                 |
| `er_app_set_<name>()`                             | Inputs, set by index through `erb_input_set_int()` and its kin       |
| `er_app_fits()`, `ER_AOT_SCREEN_W/H`              | The panel table                                                      |
| `_Static_assert(ERUI_MAX_NODES >= n)`             | The node count, checked at load                                      |
| `assets.generated.c`                              | The asset pack, section 2                                            |
| `ERNode*` statics                                 | Node handles in slots                                                |
| `s_av_*` and `er_anim_value_bind*()`              | Animated-value slots, and animated or interpolated bindings          |
| `er_anim_value_animate()` and its `ERAnimConfig`  | A leaf descriptor, started by `ASTART`                               |
| `er_seqcb_*`, `er_waitcb_*`, `er_donecb_*`        | Animation-done functions; each completion a helper they `CALL`       |
| `s_waits` and `app_wait_*()`                      | Delay leaves, and a parallel group for a joined run                  |
| `er_pan_*` and `er_responder_query_set()`         | Responder handlers and query functions, in `bindings`                |
| `s_svg*_ops`, `s_svg*_paints`, `s_svg*_grads`     | Tapes; a baked one committed by its node's binding                   |
| `build_svg*()`, `updateVector`'s `s_uv*`          | `VTAPE`, `VSET`, `VPAINT` and a commit                               |
| `kbd_cfg`, `er_keyboard_set_config()`             | The keyboard table, handed over at load                              |

## The firmware's side

```c
#include "erb_runtime.h"

static uint32_t s_arena[ERB_ARENA_BYTES / 4];

static bool sensors_read(const int32_t* args, int32_t* result, void* context)
{
    *result = read_sensor(args[0]);
    return true; /* false stops the app, naming the module */
}

static const ErbNative s_natives[] = {
    {"sensors.read", 1, "i:i", sensors_read},
};

void app_start(const void* package, size_t package_bytes)
{
    const ErbConfig config = {
        .arena = s_arena,
        .arena_bytes = sizeof(s_arena),
        .screen_w = PANEL_W,
        .screen_h = PANEL_H,
        .natives = s_natives,
        .native_count = sizeof(s_natives) / sizeof(s_natives[0]),
    };
    ErbError error;
    if (!erb_load(package, package_bytes, &config, &error))
    {
        erb_show_error(&error); /* the error screen, with the reason */
    }
}

/* Each frame, from the loop that polls touch: */
erb_tick(elapsed_ms);
er_commit();
```

A native's signature is its result, a colon, then its parameters, each `i` (int), `b` (bool) or `f`
(float); `v` as the result means none. Firmware with fonts in its packages gives the runtime storage
for their tables (`ErbConfig::fonts`): arrays of fonts, glyphs, and extra glyphs, as many as its apps
need. `on_fault` replaces the error screen when the firmware wants to handle a fault itself, and `log`
receives the reported limits. The error screen is the engine's (`er_error_screen.h`): it shows the
reason, the amounts (`arena: needs 41,210 B, firmware reserves 32,768 B`) and the source line. A
package that fails to load leaves a running app under it until the next good load.

**Limits.** How large an app the firmware accepts is set by CMake options in `runtime/CMakeLists.txt`:
`ERB_EVENT_BUDGET`, `ERB_MAX_FUNCTIONS`, `ERB_MAX_FUNCTION_WORDS`, `ERB_MAX_LOOPS`, `ERB_MAX_IMPORTS`,
`ERB_MAX_KEYBOARD_KEYS`, `ERB_MAX_TEMPLATE_DEPTH` and `ERB_REPORT_PLACES`. The defaults suit most apps,
and a package past one is rejected with the reason. The engine's pool sizes stay the engine's `ERUI_*`
options. The runtime reads them at load time, so each is set once; its image name index follows
`ERUI_IMAGE_REGISTRY_MAX`.

`erb_input_find("steps")` looks an input up once, and `erb_input_set_int()` (or `_bool`, `_float`,
`_string`) sets it after that. `erb_set_wall_clock()` replaces `er_app_set_wall_clock()`.

### Package slots

A board keeps two packages in flash, each in a slot the firmware memory-maps. A slot is a header in a 4 KB erase
sector of its own, then the package, read in place:

| Offset | Type  | Field           | Meaning                          |
| ------ | ----- | --------------- | -------------------------------- |
| 0      | `u32` | `magic`         | `ERBS`                           |
| 4      | `u32` | `sequence`      | One more than the newest slot's  |
| 8      | `u32` | `package_bytes` | The package's length             |
| 12     | `u32` | `crc`           | CRC-32 of the 12 bytes before it |
| 16     |       |                 | `0xFF` to the end of the sector  |

There is no separate record of which slot is which. The valid slot with the newest sequence is **active**, and
the other is **staging**. A new package goes in staging with the next sequence, so writing one never touches the
package that is running. The package is written first and the header last, so a write cut short leaves the slot
with its older sequence or none, and it is still the one to write over.

`erb_package_slots_boot()` runs at boot, in place of `erb_load()`:

- The newest package loads: it is running.
- It fails, for any reason `erb_load()` gives, and the other loads: the other is running, and the failed slot's
  header is programmed to zeros, which takes no erase. The last good package is the newest again, so the next
  package is written over the failed one. The firmware shows the reason on the error screen for a few seconds
  first, saying the last good package is running.
- Neither loads: the error screen shows the newest one's reason. Both are kept, since a later firmware may run
  them.
- Neither slot has a valid header: the error screen says no package is installed, and how to install one.

The firmware gives it the mapped slots and a function that clears a header. `runtime/port/erb_package_slots_esp32.c`
does this on ESP-IDF, and the RP2040 example does it in `main.c`.

`embedded-react flash` reads both headers from the board and writes staging, through `esptool` or `picotool`, then
resets it. That boot clears a package that fails, so the next `flash` doesn't take it for the last good one. On an
ESP32 it finds the slots in the board's partition table, as the partitions labelled `erb_a` and `erb_b`.
`embedded-react pack` writes a slot's image to a file instead, as a UF2 on the RP2040, for a factory line or a
board that isn't attached. On-device hot reload will receive into staging the same way.

A package can still fail after it has loaded. That shows the error screen, and it stays the active package: it
passed verification, and rolling back would hide the fault.

## Left for later

- **Keeping state across a reload** when the slot layout hasn't changed. The `build_id` and a hash of
  the slot table are enough to tell.
- **The ESP32's placement rules.** Its flash cache behaves like the RP2040's, but the dispatch loop only
  moves to IRAM once that is measured.
