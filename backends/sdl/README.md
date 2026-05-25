# backends/sdl

SDL2 backend. Used to exercise the engine end-to-end on Linux / macOS / Windows desktops
without flashing real hardware, and as the host-side target for pixel-exact rendering
regression tests in CI.

This is the first backend planned for a real implementation — it lets us bring up the
React-on-QuickJS toolchain (Flow A) without needing a bring-up board.

**Status:** Stub. Implementation tracks the Flow A milestone.
