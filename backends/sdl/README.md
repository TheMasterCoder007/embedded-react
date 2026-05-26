# backends/sdl

SDL2 backend. Used to exercise the engine end-to-end on Linux / macOS / Windows desktops
without flashing real hardware, and as the host-side target for pixel-exact rendering
regression tests in CI.

This is the first backend planned for a real implementation — it lets us bring up the
React-on-QuickJS toolchain (Flow A) without needing a bring-up board.

**Status:** Implemented. `fill_rect` uses SDL draw calls with blend-mode switching (opaque vs straight-alpha). `copy_rect` and `blend_rect` upload source pixels into a streaming ARGB8888 scratch texture and composite using a custom premultiplied-alpha blend mode (`SDL_ComposeCustomBlendMode`, requires SDL 2.0.6+). Global alpha for `blend_rect` is applied by scaling both the color and alpha channels uniformly via `SDL_SetTextureColorMod` / `SDL_SetTextureAlphaMod`.

Public API (`sdl_backend.h`):
- `er_sdl_backend_init(renderer, fb_w, fb_h)` — registers the backend with the engine
- `er_sdl_present()` — wraps `SDL_RenderPresent`; call after `er_commit()` each frame
- `er_sdl_backend_destroy()` — frees the scratch texture
