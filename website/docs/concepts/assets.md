---
title: 'Assets'
description: 'How images and fonts are baked into an ERPK pack and loaded on the device.'
---

There is no image decoder and no font rasteriser on the device. Everything an app draws that is
not a rectangle or text from the built-in font is prepared on your computer at build time, in the
exact form the engine reads from flash.

## Import-driven

An app imports a file and uses what the import returns. That is the whole API.

```jsx
import logo from './assets/logo.png';   // the baked image's name: "logo"
import Inter from './assets/Inter.ttf';  // the baked font family: "Inter"

<Image source={logo} style={{width: 64, height: 64}} />
<Text style={{fontFamily: Inter, fontSize: 18}}>Hi</Text>
```

The bundler sees each import, bakes the file, and hands the app the asset's name, which is the
file's basename. At runtime `<Image>` and `fontFamily` look the name up in the engine's registry.

**Images** are baked from PNG to the engine's premultiplied ARGB8888. Opaque art, full-screen
backgrounds especially, can opt into an RGB565 bake per image: half the flash, half the memory
traffic on every repaint, and the engine copies it through its opaque fast path with no per-pixel
blending. The baker refuses to bake a non-opaque PNG as RGB565 rather than silently dropping its
transparency.

**Fonts** are baked from TTF or OTF into bitmap glyphs, pre-rasterised at specific pixel sizes.
Because the device cannot rasterise, the baker has to know every `fontSize` the app will use. It
reads them from the source, folding constants, so a type scale like `const TYPE = {body: 14,
title: 22}` bakes what it means, and both arms of a `screen.width` ternary are covered. A size it
cannot see, one computed at runtime, gets a warning naming the expression, because on the device
that text silently snaps to the nearest baked size.

Only the glyphs you ask for are baked: printable ASCII plus a named symbol set, and whatever
characters you add. The build checks the app's text against what was baked and warns about any
character with no glyph, since the engine draws `?` for it and nothing else would tell you.

## The built-in font

Text with no `fontFamily` uses the engine's built-in font, a bake of Inter that ships inside the
engine at seven sizes: **10, 12, 16, 20, 24, 32 and 48** pixels. Any other size snaps to the
nearest of those, and the build says so:

```text
embedded-react: 1 font size(s) the app uses are not baked and will render at the nearest baked size:
  15px → renders at 16px in the built-in font (baked: 10, 12, 16, 20, 24, 32, 48)
```

Use one of those sizes, or import your own font and bake the sizes you need.

## Configuring the bake

Per-app overrides live in `assets.config.js` next to your entry file:

```js
export default {
  fonts: {
    Inter: {
      sizes: [14, 18, 24], // pin the sizes instead of relying on discovery
      bpp: 4, // 1, 2, 4 or 8 bits of anti-aliasing per pixel
      glyphs: 'common', // 'ascii' | 'minimal' | 'common' | 'greek' | 'common-greek' | [codepoints]
      extraGlyphs: '⌘⏻№', // this app's own characters, on top of the set
    },
  },
  images: {
    bg: {format: 'rgb565'}, // 16-bit bake for opaque art; the default is 'argb8888'
  },
};
```

## Where assets go

The same baked bytes ship two ways, and the flow decides which.

**Flow A packs them into the container.** `npx embedded-react build` writes `dist/app.erpkg`,
which holds the app's bytecode and an ERPK asset pack, with a CRC32 and the QuickJS version it was
built for. The firmware loads the container, registers the assets, then mounts the app. Assets
update together with the app and never need a firmware rebuild.

```text
app.erpkg:  "ERCF" | format version | crc32 | QuickJS tag | bytecode | ERPK asset pack
```

**Flow B compiles them in.** `npx embedded-react build --aot` also writes `assets.generated.c`,
which exposes `er_register_assets()`. The firmware calls it once at boot, and every image and font
is registered straight from flash: zero RAM, since the engine reads the bytes in place.

Either way the engine holds each image in a registry slot (`ERUI_IMAGE_REGISTRY_MAX`, 128 by
default, about 80 bytes each). Past the limit, registration is refused and the image simply does not
draw, so a board that shrinks the registry should keep it at or above its asset count.

## SVG

An `.svg` import is baked at build time into a compact vector op-tape the engine's rasteriser
draws directly, with a raster fallback for files that use features it cannot represent. Pass it to
`<Svg source={…}>`. Shapes that change at runtime use the `<Svg>` element's own children
(`<Path>`, `<Circle>`, `<Arc>`) instead, and dials, gauges and progress rings have their own native
`<Dial>` node that is far cheaper than a re-tessellated arc. The [components reference](../api/components.md)
covers all three.
