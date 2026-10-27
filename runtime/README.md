# runtime

The parts of the device runtime that no JavaScript engine owns, and the Flow B runtime. Plain C99
that links only the engine: no QuickJS, no allocation in the container reader or the Flow B runtime,
no platform headers. The QuickJS bridge links `er-runtime`; Flow B firmware links `erb-runtime`.

| File             | What it does                                                                                                                                                     |
| ---------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `er_container.c` | Reads an ERCF container (`.erpkg`): checks the magic, the format version, every section bound and the CRC, and hands back the tag and sections. Also `er_crc32`. |
| `er_assets.c`    | Registers an ERPK asset pack (images and fonts) with the engine, in place or from a copy.                                                                        |
| `er_hotreload.c` | Parses ERHR hot-reload frames from a byte stream into a staging buffer and checks their CRC.                                                                     |
| `erb_runtime.c`  | Flow B: loads a package (`erb_load`), lays out the arena, builds templates, runs events, timers, effects and animations, and shows the error screen.             |
| `erb_verify.c`   | Flow B: checks a whole package before anything is built (the spec's verifier rules).                                                                             |
| `erb_vm.c`       | Flow B: the register VM, with the C output's arithmetic rules.                                                                                                   |
| `erb_format.h`   | Flow B: the package format and the opcode list the compiler and the runtime share.                                                                               |
| `erb_props.c`    | Flow B: the engine ABI's prop ids, mapped to `ERProps` fields.                                                                                                   |
| `tests/`         | CTests: the container reader, one test per Flow B verifier rule, and the VM.                                                                                     |

Running a container's program is the caller's job: `er_runtime_load_container` in
`bridges/quickjs/er_runtime.h` checks the QuickJS tag and runs the bytecode, and `er_hotreload_apply`
there loads a received frame. A Flow B package (tag `erb`) goes to `erb_load` in `erb_runtime.h`.

```bash
cmake -S runtime -B runtime/build
cmake --build runtime/build
ctest --test-dir runtime/build --output-on-failure
```

**Docs:** [Architecture](https://embedded-react.dev/internals/architecture),
[Flow B packages](https://embedded-react.dev/internals/flow-b-packages) and
[Hot reload](https://embedded-react.dev/guides/hot-reload).
