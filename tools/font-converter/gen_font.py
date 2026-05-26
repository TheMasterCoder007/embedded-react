#!/usr/bin/env python3
"""
gen_font.py — Convert a TrueType/OpenType font to a proportional bitmap font
for embedded-react.

Output format
-------------
For each requested pixel size the font is rendered into a packed glyph atlas
plus a per-glyph metrics table (width, height, x_offset, y_offset, advance).
Bitmaps are row-major at the requested BPP (--bpp 1|2|4|8):
  1-bit: MSB = leftmost pixel, each row is ceil(width / 8) bytes.
  2-bit: MSB pair = leftmost pixel, each row is ceil(width / 4) bytes.
  4-bit: high nibble = left pixel of pair, each row is ceil(width / 2) bytes.
  8-bit: raw grayscale coverage, each row is width bytes.

ASCII glyphs (default 0x20..0x7E) form a dense range, indexed by
`(codepoint - first)`. Optional special symbols (--symbol-set / --extras)
are emitted as a sorted `ExtraGlyph` table that the runtime binary-searches
after the dense lookup misses.

A single output .c file declares one `BitmapFont` per requested size and a
`g_inter_sizes[]` list that the runtime walks to pick the closest match for a
given pixel height.

Usage
-----
    python3 tools/font-converter/gen_font.py \\
        --font tools/font-converter/Inter-Regular.ttf \\
        --sizes 10,12,16,20,24,32,48 \\
        --symbol-set common \\
        --symbol inter \\
        --out src/font_data.c

A single-size .bin blob (for runtime upload via er_font_load) carries only the
dense ASCII range — wire format v2 (includes BPP).

Dependencies
    pip install Pillow
"""

import argparse
import datetime
import os
import sys
from typing import List, Tuple, Dict

FIRST_CHAR_DEFAULT = 0x20
LAST_CHAR_DEFAULT  = 0x7E


# ---------------------------------------------------------------------------
# Predefined symbol sets
# ---------------------------------------------------------------------------
#
# Each entry: (codepoint, display character for the C comment).
# Kept sorted by codepoint so the emitted ExtraGlyph table is already sorted
# for binary search.

SYMBOLS_COMMON: List[int] = [
    0x00A2,  # ¢ cent
    0x00A3,  # £ pound
    0x00A5,  # ¥ yen
    0x00A7,  # § section
    0x00A9,  # © copyright
    0x00AE,  # ® registered
    0x00B0,  # ° degree
    0x00B1,  # ± plus-minus
    0x00B5,  # µ micro
    0x00D7,  # × multiply
    0x00F7,  # ÷ divide
    0x2013,  # – en dash
    0x2014,  # — em dash
    0x2018,  # ' left single quote
    0x2019,  # ' right single quote
    0x201C,  # " left double quote
    0x201D,  # " right double quote
    0x2020,  # † dagger
    0x2021,  # ‡ double dagger
    0x2022,  # • bullet
    0x2026,  # … ellipsis
    0x2030,  # ‰ per mille
    0x20AC,  # € euro
    0x2122,  # ™ trade mark
    0x2190,  # ← left arrow
    0x2191,  # ↑ up arrow
    0x2192,  # → right arrow
    0x2193,  # ↓ down arrow
    0x2194,  # ↔ left-right arrow
    0x21B5,  # ↵ return
    0x2202,  # ∂ partial diff
    0x2206,  # ∆ increment (delta)
    0x221A,  # √ square root
    0x221E,  # ∞ infinity
    0x2211,  # ∑ summation
    0x2212,  # − minus
    0x2248,  # ≈ almost equal
    0x2260,  # ≠ not equal
    0x2264,  # ≤ less or equal
    0x2265,  # ≥ greater or equal
    0x25A0,  # ■ filled square
    0x25CF,  # ● filled circle
    0x25C6,  # ◆ filled diamond
    0x2605,  # ★ star
    0x2606,  # ☆ outline star
    0x2713,  # ✓ check
    0x2717,  # ✗ ballot x
]

SYMBOLS_MINIMAL: List[int] = [
    0x00B0, 0x00B1, 0x00B5, 0x00D7, 0x00F7,
    0x2013, 0x2014, 0x2022, 0x2026,
    0x2190, 0x2191, 0x2192, 0x2193,
    0x2713, 0x2717,
]

SYMBOLS_GREEK: List[int] = [
    0x0391, 0x0392, 0x0393, 0x0394, 0x0395, 0x0396, 0x0397, 0x0398,
    0x0399, 0x039A, 0x039B, 0x039C, 0x039D, 0x039E, 0x039F, 0x03A0,
    0x03A1, 0x03A3, 0x03A4, 0x03A5, 0x03A6, 0x03A7, 0x03A8, 0x03A9,
    0x03B1, 0x03B2, 0x03B3, 0x03B4, 0x03B5, 0x03B6, 0x03B7, 0x03B8,
    0x03B9, 0x03BA, 0x03BB, 0x03BC, 0x03BD, 0x03BE, 0x03BF, 0x03C0,
    0x03C1, 0x03C2, 0x03C3, 0x03C4, 0x03C5, 0x03C6, 0x03C7, 0x03C8, 0x03C9,
]

SYMBOL_SETS: Dict[str, List[int]] = {
    "none":         [],
    "minimal":      SYMBOLS_MINIMAL,
    "common":       SYMBOLS_COMMON,
    "common-greek": sorted(set(SYMBOLS_COMMON + SYMBOLS_GREEK)),
    "greek":        SYMBOLS_GREEK,
}


# ---------------------------------------------------------------------------
# Glyph rasterization
# ---------------------------------------------------------------------------

def rasterize_glyph(pil_font, ch: str, bpp: int = 1) -> dict | None:
    """Render one glyph.

    Returns dict with offset (=0), width, height, x_offset, y_offset, advance,
    bitmap_bytes — or None if the font has no glyph for `ch`.

    bpp=1  — packed 1-bit-per-pixel, MSB = leftmost pixel; row stride = ceil(w/8) bytes.
    bpp=8  — 8-bit grayscale, 1 byte per pixel; row stride = w bytes.
    """
    from PIL import Image, ImageDraw  # type: ignore

    bbox = pil_font.getbbox(ch)
    advance = int(round(pil_font.getlength(ch)))

    if bbox is None or bbox[2] <= bbox[0] or bbox[3] <= bbox[1]:
        # Empty / missing glyph — advance only.
        return {
            "width":    0,
            "height":   0,
            "x_offset": 0,
            "y_offset": 0,
            "advance":  advance,
            "bitmap":   b"",
        }

    x0, y0, x1, y1 = bbox
    w = x1 - x0
    h = y1 - y0
    if w > 255 or h > 255:
        raise ValueError(
            f"glyph U+{ord(ch):04X} '{ch}' size {w}x{h} exceeds uint8 limit")

    img = Image.new("L", (w, h), 0)
    draw = ImageDraw.Draw(img)
    draw.text((-x0, -y0), ch, font=pil_font, fill=255)
    pixels = list(img.getdata())

    if bpp == 8:
        # 8-bit grayscale: 1 byte per pixel, raw coverage 0-255.
        out = bytearray()
        for row in range(h):
            for col in range(w):
                out.append(pixels[row * w + col])
    elif bpp == 4:
        # 4-bit grayscale: 2 pixels per byte, high nibble = left pixel.
        row_bytes = (w + 1) // 2
        out = bytearray()
        for row in range(h):
            for col_byte in range(row_bytes):
                lo_col = col_byte * 2
                hi_col = lo_col + 1
                lo_px = pixels[row * w + lo_col] >> 4 if lo_col < w else 0
                hi_px = pixels[row * w + hi_col] >> 4 if hi_col < w else 0
                out.append((lo_px << 4) | hi_px)
    elif bpp == 2:
        # 2-bit grayscale: 4 pixels per byte, bits 7-6 = leftmost pixel.
        row_bytes = (w + 3) // 4
        out = bytearray()
        for row in range(h):
            for col_byte in range(row_bytes):
                byte = 0
                for pair in range(4):
                    col = col_byte * 4 + pair
                    if col < w:
                        byte |= (pixels[row * w + col] >> 6) << (6 - pair * 2)
                out.append(byte)
    else:
        # 1-bit packed: 8 pixels per byte, MSB = leftmost pixel.
        row_bytes = (w + 7) // 8
        out = bytearray()
        for row in range(h):
            for byte_idx in range(row_bytes):
                byte = 0
                for bit in range(8):
                    col = byte_idx * 8 + bit
                    if col < w and pixels[row * w + col] >= 128:
                        byte |= (0x80 >> bit)
                out.append(byte)

    return {
        "width":    w,
        "height":   h,
        "x_offset": x0,
        "y_offset": y0,
        "advance":  advance,
        "bitmap":   bytes(out),
    }


def render_size(font_path: str, pixel_size: int,
                first: int, last: int,
                symbols: List[int],
                bpp: int = 1) -> Tuple[List[dict], List[Tuple[int, dict]], bytes, int, int]:
    """Render dense range [first, last] plus optional sparse `symbols`.

    Returns (dense_glyphs, extras, bitmap_blob, line_height, baseline)
    where bitmap_blob concatenates dense glyph bitmaps followed by extras'
    bitmaps. Each glyph dict in the result has bitmap_offset set.
    """
    from PIL import ImageFont  # type: ignore
    pil_font = ImageFont.truetype(font_path, pixel_size)
    ascent, descent = pil_font.getmetrics()
    line_height = ascent + descent
    baseline    = ascent

    bitmap_blob = bytearray()
    dense: List[dict] = []

    for code in range(first, last + 1):
        ch = chr(code)
        g = rasterize_glyph(pil_font, ch, bpp=bpp)
        if g is None:
            # Should never happen for ASCII, but guard anyway.
            g = {"width": 0, "height": 0, "x_offset": 0, "y_offset": 0,
                 "advance": pixel_size // 2, "bitmap": b""}
        offset = len(bitmap_blob) if g["width"] > 0 else 0
        bitmap_blob.extend(g["bitmap"])
        dense.append({
            "offset":   offset,
            "width":    g["width"],
            "height":   g["height"],
            "x_offset": g["x_offset"],
            "y_offset": g["y_offset"],
            "advance":  g["advance"],
        })

    extras: List[Tuple[int, dict]] = []
    for code in symbols:
        if first <= code <= last:
            continue  # already covered by dense range
        ch = chr(code)
        g = rasterize_glyph(pil_font, ch, bpp=bpp)
        if g is None or (g["width"] == 0 and g["advance"] == 0):
            print(f"  warn: font has no glyph for U+{code:04X}, skipping",
                  file=sys.stderr)
            continue
        offset = len(bitmap_blob) if g["width"] > 0 else 0
        bitmap_blob.extend(g["bitmap"])
        extras.append((code, {
            "offset":   offset,
            "width":    g["width"],
            "height":   g["height"],
            "x_offset": g["x_offset"],
            "y_offset": g["y_offset"],
            "advance":  g["advance"],
        }))

    extras.sort(key=lambda kv: kv[0])
    return dense, extras, bytes(bitmap_blob), line_height, baseline


# ---------------------------------------------------------------------------
# .c emitter
# ---------------------------------------------------------------------------

def _safe_comment_char(code: int) -> str:
    """ASCII-printable form for the trailing /* 'x' */ glyph comment."""
    if 0x20 <= code <= 0x7E and code not in (ord("'"), ord("\\")):
        return chr(code)
    return f"\\u{code:04X}" if code > 0xFF else f"\\x{code:02X}"


def emit_c(out_path: str, font_path: str, symbol: str,
           sizes_data, first: int, last: int,
           symbols: List[int], bpp: int = 1) -> None:
    num_glyphs = last - first + 1
    lines: List[str] = []

    sizes_str = ",".join(str(s[0]) for s in sizes_data)
    cmd_symbols = ""
    if symbols:
        cmd_symbols = " --extras " + ",".join(f"0x{c:04X}" for c in symbols)
    cmd_bpp = f" --bpp {bpp}" if bpp != 1 else ""

    lines.append("/* AUTO-GENERATED — do not edit by hand.")
    lines.append(f" * Source : {os.path.basename(font_path)}")
    lines.append(f" * Command: python3 tools/font-converter/gen_font.py "
                 f"--font {os.path.basename(font_path)} --sizes {sizes_str} "
                 f"--symbol {symbol}{cmd_symbols}{cmd_bpp} --out {out_path}")
    lines.append(f" * Date   : {datetime.datetime.now().strftime('%Y-%m-%d')}")
    lines.append(" */")
    lines.append('#include "font_bitmap.h"')
    lines.append("#include <stddef.h>")
    lines.append("")

    for pixel_size, dense, extras, blob, line_h, baseline in sizes_data:
        prefix = f"s_{symbol}_{pixel_size}"

        lines.append(f"static const uint8_t {prefix}_bitmap[{len(blob)}] = {{")
        for i in range(0, len(blob), 16):
            chunk = blob[i:i+16]
            hex_vals = ", ".join(f"0x{b:02X}" for b in chunk)
            lines.append(f"    {hex_vals},")
        lines.append("};")
        lines.append("")

        lines.append(f"static const GlyphInfo {prefix}_glyphs[{num_glyphs}] = {{")
        for i, g in enumerate(dense):
            code = first + i
            ch = _safe_comment_char(code)
            lines.append(
                f"    {{ .bitmap_offset = {g['offset']}, .width = {g['width']}, "
                f".height = {g['height']}, .x_offset = {g['x_offset']}, "
                f".y_offset = {g['y_offset']}, .advance = {g['advance']} }},"
                f" /* 0x{code:02X} '{ch}' */"
            )
        lines.append("};")
        lines.append("")

        if extras:
            lines.append(f"static const ExtraGlyph {prefix}_extras[{len(extras)}] = {{")
            for code, g in extras:
                ch = _safe_comment_char(code)
                lines.append(
                    f"    {{ .codepoint = 0x{code:04X}, .info = {{ "
                    f".bitmap_offset = {g['offset']}, .width = {g['width']}, "
                    f".height = {g['height']}, .x_offset = {g['x_offset']}, "
                    f".y_offset = {g['y_offset']}, .advance = {g['advance']} }} }},"
                    f" /* U+{code:04X} '{ch}' */"
                )
            lines.append("};")
            lines.append("")

        font_sym = f"g_font_{symbol}_{pixel_size}"
        lines.append(f"const BitmapFont {font_sym} = {{")
        lines.append(f"    .bitmap       = {prefix}_bitmap,")
        lines.append(f"    .glyphs       = {prefix}_glyphs,")
        if extras:
            lines.append(f"    .extras       = {prefix}_extras,")
            lines.append(f"    .extras_count = {len(extras)},")
        else:
            lines.append(f"    .extras       = NULL,")
            lines.append(f"    .extras_count = 0,")
        lines.append(f"    .first        = 0x{first:04X},")
        lines.append(f"    .last         = 0x{last:04X},")
        lines.append(f"    .pixel_size   = {pixel_size},")
        lines.append(f"    .line_height  = {line_h},")
        lines.append(f"    .baseline     = {baseline},")
        lines.append(f"    .format       = {bpp},")
        lines.append("};")
        lines.append("")

    list_sym = f"g_{symbol}_sizes"
    lines.append(f"const BitmapFont *const {list_sym}[] = {{")
    for pixel_size, *_ in sizes_data:
        lines.append(f"    &g_font_{symbol}_{pixel_size},")
    lines.append("};")
    lines.append(f"const size_t {list_sym}_count = "
                 f"sizeof({list_sym}) / sizeof({list_sym}[0]);")
    lines.append("")

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    total_bytes = sum(len(blob) for _, _, _, blob, _, _ in sizes_data)
    total_extras = sum(len(extras) for _, _, extras, _, _, _ in sizes_data)
    print(f"Wrote {len(sizes_data)} sizes ({sizes_str}), "
          f"{total_bytes} bytes bitmap, {total_extras} extra-glyph entries -> {out_path}")


# ---------------------------------------------------------------------------
# .bin emitter — wire format v1 (ASCII dense only)
# ---------------------------------------------------------------------------

FONT_BLOB_MAGIC   = b"FONT"
FONT_BLOB_VERSION = 2


def build_blob(pixel_size: int, dense: List[dict], bitmap: bytes,
               line_height: int, baseline: int, first: int, last: int,
               bpp: int = 1) -> bytes:
    """Pack one font size into the er_font_load wire format v2 (ASCII only).

    v2 layout (17-byte header):
        0..3   magic 'F','O','N','T'
        4      version = 2
        5      first  (ASCII byte)
        6      last   (ASCII byte)
        7      pixel_size
        8      line_height
        9      baseline
        10     format / BPP (1, 2, 4, or 8)
        11..12 glyph_count (uint16 LE)
        13..16 bitmap_size (uint32 LE)
        17..   glyph_count * 8 bytes — GlyphInfo entries
        ...    bitmap_size bytes — packed glyph atlas
    """
    import struct
    if last < first:
        raise ValueError("last < first")
    if first > 0xFF or last > 0xFF:
        raise ValueError("wire format only supports 8-bit codepoints")
    glyph_count = last - first + 1
    if len(dense) != glyph_count:
        raise ValueError(f"glyph count mismatch: have {len(dense)}, want {glyph_count}")

    header = struct.pack(
        "<4sBBBBBBBHI",
        FONT_BLOB_MAGIC,
        FONT_BLOB_VERSION,
        first, last, pixel_size,
        line_height, baseline,
        bpp,
        glyph_count,
        len(bitmap),
    )
    glyph_table = bytearray()
    for g in dense:
        glyph_table += struct.pack(
            "<HBBbbBB",
            g["offset"], g["width"], g["height"],
            g["x_offset"], g["y_offset"], g["advance"],
            0,
        )
    return bytes(header) + bytes(glyph_table) + bitmap


def emit_bin(out_path: str, pixel_size: int, dense: List[dict], bitmap: bytes,
             line_height: int, baseline: int, first: int, last: int,
             bpp: int = 1) -> None:
    blob = build_blob(pixel_size, dense, bitmap, line_height, baseline, first, last, bpp=bpp)
    with open(out_path, "wb") as f:
        f.write(blob)
    print(f"Wrote {len(blob)} byte font blob -> {out_path}")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def parse_extras(arg: str) -> List[int]:
    out = []
    for tok in arg.split(","):
        tok = tok.strip()
        if not tok:
            continue
        out.append(int(tok, 0))
    return out


def main() -> None:
    parser = argparse.ArgumentParser(description="Convert TTF/OTF -> variable-width bitmap font data")
    parser.add_argument("--font",   required=True, help="Path to TrueType/OpenType font file")
    parser.add_argument("--sizes",  required=True,
                        help="Comma-separated pixel sizes to bake, e.g. 10,12,16,20,24,32,48")
    parser.add_argument("--out",    default=None,
                        help="Output .c file path (multi-size build)")
    parser.add_argument("--bin-out", dest="bin_out", default=None,
                        help="Output single-size .bin font blob (requires --sizes to be a single value; "
                             "extras are ignored — wire format v1 is ASCII only)")
    parser.add_argument("--symbol", default="inter",
                        help="C symbol prefix for --out (default 'inter')")
    parser.add_argument("--first",  default="0x20", help="First dense character (default 0x20)")
    parser.add_argument("--last",   default="0x7E", help="Last dense character  (default 0x7E)")
    parser.add_argument("--symbol-set", dest="symbol_set", default="none",
                        choices=sorted(SYMBOL_SETS.keys()),
                        help="Predefined extra-symbol set to include (default 'none')")
    parser.add_argument("--extras", default="",
                        help="Comma-separated extra codepoints (hex/dec), merged with --symbol-set")
    parser.add_argument("--bpp", type=int, default=1, choices=[1, 2, 4, 8],
                        help="Bits per pixel: 1 (packed, default), 2, 4, or 8 (grayscale AA)")
    args = parser.parse_args()

    first = int(args.first, 0)
    last  = int(args.last,  0)
    sizes = [int(s.strip()) for s in args.sizes.split(",") if s.strip()]

    symbols = list(SYMBOL_SETS.get(args.symbol_set, []))
    symbols += parse_extras(args.extras)
    symbols = sorted(set(symbols))
    bpp = args.bpp

    if not (args.out or args.bin_out):
        print("Error: specify at least one of --out / --bin-out", file=sys.stderr)
        sys.exit(1)
    if args.bin_out and len(sizes) != 1:
        print("Error: --bin-out requires exactly one size in --sizes", file=sys.stderr)
        sys.exit(1)

    if not os.path.isfile(args.font):
        print(f"Error: font file not found: {args.font}", file=sys.stderr)
        sys.exit(1)

    try:
        from PIL import ImageFont  # noqa: F401
    except ImportError:
        print("Error: install Pillow: pip install Pillow", file=sys.stderr)
        sys.exit(1)

    sizes_data = []
    for px in sizes:
        print(f"  rendering size {px} px...")
        dense, extras, blob, line_h, baseline = render_size(args.font, px, first, last, symbols, bpp=bpp)
        sizes_data.append((px, dense, extras, blob, line_h, baseline))

    if args.out:
        emit_c(args.out, args.font, args.symbol, sizes_data, first, last, symbols, bpp=bpp)

    if args.bin_out:
        px, dense, _extras, blob, line_h, baseline = sizes_data[0]
        emit_bin(args.bin_out, px, dense, blob, line_h, baseline, first, last, bpp=bpp)


if __name__ == "__main__":
    main()
