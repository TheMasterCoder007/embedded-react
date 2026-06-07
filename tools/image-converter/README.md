# image-converter

Build-time image bake for embedded-react (IMG-1). Decodes images on the host and emits a C source
file of **premultiplied ARGB8888** pixel arrays plus an `er_register_baked_images()` that hands them to
the engine's image registry (`er_image_load`). The engine references the buffers by pointer — no
runtime decoder, zero RAM copy — so an `<Image imageName="…">` just looks them up by name.

This mirrors the font/bytecode bake pattern: decode on the host, ship raw pixels in flash.

## `gen_image.py` — bake images → C

```
pip install Pillow
python tools/image-converter/gen_image.py \
    --out demos/thermostat/assets/image_data.c \
    name1=path/to/a.png \
    name2=path/to/b.png
```

Each positional arg is `name=path`; `name` is what `<Image imageName="name">` looks up (≤ 63 chars,
`ER_IMAGE_NAME_MAX`). Writes `image_data.c` and a sibling `image_data.h` (declares
`er_register_baked_images`). The generated files are committed artifacts — regenerate and re-commit
when the source images change. The engine u,opdoes scaling at render time, so bake at whatever source
resolution you want to ship (bake at the largest on-screen size to avoid upscaling; enable
`ERUI_BILINEAR_SCALE` for smooth up/down-scaling).

## Wiring a baked set into a target

1. Add the generated `image_data.c` to the target's sources and its directory to the include path
   (see `examples/esp32/esp32-s3/main/CMakeLists.txt` and `examples/linux/CMakeLists.txt`).
2. Call `er_register_baked_images()` once at boot, **after** the render backend is set
   (`embedded_renderer_set_backend()` initializes the image registry).

## Notes / roadmap

- Output is premultiplied ARGB8888 (the registry format). With the RGB565 backend, packing to 565
  happens at blit time.
- IMG-1 is the build-time bake baseline. A compressed path (bake → QOI, decode once in-engine) is the
  planned IMG-2 upgrade for larger assets, since the generated C grows with pixel count.
