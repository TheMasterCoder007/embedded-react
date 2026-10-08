# backends/software

Pure CPU compositor into an ARGB8888 framebuffer in RAM (`0xAARRGGBB`, persisting between commits
like panel GRAM): `fill_rect` (straight-alpha source-over), `copy_rect` (premultiplied source-over)
and `blend_rect` (premultiplied, scaled by a global alpha) as clipped scanline loops. It runs the
identical primitive code a device runs, so its output is pixel-accurate to a hardware ARGB target,
which makes it the reference compositor: the browser simulator presents it through `../web`.
Built for a target with AVX2 (`-mavx2`, `-march=x86-64-v3`, ...), the translucent fill and the copy and
blend rows take eight pixels per step with the same integer arithmetic, so the output is bit-identical.

**Docs:** [Engine and backends](https://embedded-react.dev/concepts/engine-and-backends).

`software_backend.h`: `er_software_backend_init` / `_destroy` / `_clear` and the framebuffer accessors.
