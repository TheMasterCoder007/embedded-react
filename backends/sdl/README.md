# backends/sdl

SDL2 backend: the desktop host and the test target, on Linux, macOS and Windows. `fill_rect` uses SDL
draw calls with blend-mode switching; `copy_rect` and `blend_rect` upload into a streaming ARGB8888
scratch texture and composite with a custom premultiplied-alpha blend mode
(`SDL_ComposeCustomBlendMode`, SDL 2.0.6+), with `blend_rect`'s global alpha applied through the
texture colour and alpha mods.

**Docs:** [Linux and desktop](https://embedded-react.dev/guides/boards/linux).

```c
er_sdl_backend_init(renderer, fb_w, fb_h);   // registers the backend with the engine
er_sdl_present();                            // SDL_RenderPresent; call after er_commit() each frame
er_sdl_backend_destroy();                    // frees the scratch texture
```
