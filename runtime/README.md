# runtime

The parts of the device runtime that no JavaScript engine owns. Plain C99 that links only the
engine: no QuickJS, no allocation in the container reader, no platform headers. The QuickJS bridge
links it today, and the Flow B runtime will too.

| File             | What it does                                                                                                                                                     |
| ---------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `er_container.c` | Reads an ERCF container (`.erpkg`): checks the magic, the format version, every section bound and the CRC, and hands back the tag and sections. Also `er_crc32`. |
| `er_assets.c`    | Registers an ERPK asset pack (images and fonts) with the engine, in place or from a copy.                                                                        |
| `er_hotreload.c` | Parses ERHR hot-reload frames from a byte stream into a staging buffer and checks their CRC.                                                                     |
| `tests/`         | The container reader's CTest.                                                                                                                                    |

Running a container's program is the caller's job: `er_runtime_load_container` in
`bridges/quickjs/er_runtime.h` checks the QuickJS tag and runs the bytecode, and `er_hotreload_apply`
there loads a received frame.

```bash
cmake -S runtime -B runtime/build
cmake --build runtime/build
ctest --test-dir runtime/build --output-on-failure
```

**Docs:** [Architecture](https://embedded-react.dev/internals/architecture) and
[Hot reload](https://embedded-react.dev/guides/hot-reload).
