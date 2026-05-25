# font-converter

Python CLI that converts a TrueType/OpenType font into bitmap font data for
embedded-react.

It can emit:

- a generated `.c` file containing one `BitmapFont` per requested pixel size
- a single-size `.bin` font blob for runtime loading via `er_font_load`

See `gen_font.py` for the supported options and output formats.
