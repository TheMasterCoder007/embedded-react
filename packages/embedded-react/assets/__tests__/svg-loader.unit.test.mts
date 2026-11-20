/*
 * Copyright 2026 Cory Lamming
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import {describe, it, expect, vi} from 'vitest';
import {build} from 'esbuild';
import {
  copyFileSync,
  existsSync,
  mkdirSync,
  mkdtempSync,
  writeFileSync,
} from 'node:fs';
import {tmpdir} from 'node:os';
import {dirname, join, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {PNG} from 'pngjs';
import {imageAssetName, registerSvgVectorLoader} from '../svg-loader.mts';

// `assets` (optional) = a Map the loader registers raster-fallback PNGs into (name -> png path), mirroring a
// real bundler's image collection. When omitted, the loader has no raster fallback (vector-only).
const FONTS = resolve(
  dirname(fileURLToPath(import.meta.url)),
  '../../../../assets/fonts',
);

// `fonts` (optional) = font files copied in before the build: path in the app → file in assets/fonts.
async function bundleWithSvg(
  svg,
  {
    entry = `import icon from './icon.svg'; globalThis.__icon = icon;`,
    assets,
    fonts = {},
  } = {},
) {
  const directory = mkdtempSync(join(tmpdir(), 'svgld-'));
  for (const [path, file] of Object.entries(fonts)) {
    mkdirSync(dirname(join(directory, path)), {recursive: true});
    copyFileSync(join(FONTS, file), join(directory, path));
  }
  writeFileSync(join(directory, 'icon.svg'), svg);
  writeFileSync(join(directory, 'entry.js'), entry);
  const addRaster = assets ? (name, path) => assets.set(name, path) : undefined;
  const result = await build({
    entryPoints: [join(directory, 'entry.js')],
    bundle: true,
    write: false,
    format: 'iife',
    logLevel: 'silent',
    plugins: [
      {
        name: 'svg',
        setup: pluginBuild => {
          registerSvgVectorLoader(pluginBuild, addRaster);
          pluginBuild.onLoad({filter: /\.ttf$/}, () => ({
            contents: 'module.exports = "font";',
            loader: 'js',
          }));
        },
      },
    ],
  });
  return result.outputFiles[0].text;
}

// Evaluate the bundled IIFE (which assigns the import to globalThis.__icon) and return the artifact.
function evalArtifact(bundle) {
  delete globalThis.__icon;
  // eslint-disable-next-line no-new-func
  new Function(bundle)();
  return globalThis.__icon;
}

describe('svg-loader (esbuild .svg → inline vector artifact)', () => {
  it('inlines an imported .svg as a {kind:vector} op-tape artifact', async () => {
    const artifact = await bundleWithSvg(
      '<svg viewBox="0 0 10 10"><path d="M0 0 L10 0" fill="red"/></svg>',
    );
    const art = evalArtifact(artifact);
    expect(art.kind).toBe('vector');
    expect(art.width).toBe(10);
    expect(art.height).toBe(10);
    expect(art.ops).toEqual([0, 0, 1, 0, 0, 2, 10, 0]); // [SHAPE,paint, MOVE,0,0, LINE,10,0]
    expect(art.paints[0]).toBe(0xffff0000); // red fill
  });

  it('surfaces a bake failure as an esbuild error (not a silent miscompile)', async () => {
    await expect(bundleWithSvg('<svg><path d="M0 0"')).rejects.toThrow(); // malformed XML
  });

  it('falls back to a raster image artifact when the SVG uses unsupported features', async () => {
    const assets = new Map();
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
    const artifact = await bundleWithSvg(
      '<svg viewBox="0 0 20 20" width="20" height="20"><rect width="9" height="9" mask="url(#m)"/></svg>',
      {assets},
    );
    warn.mockRestore();
    const art = evalArtifact(artifact);
    expect(art.kind).toBe('raster');
    expect(art.name).toBe('icon'); // basename of icon.svg
    expect(art.width).toBe(20);
    expect(art.height).toBe(20);
    // The rasterized PNG was registered into the image collection and actually written to disk.
    expect(assets.has('icon')).toBe(true);
    expect(assets.get('icon')).toMatch(/\.png$/);
    expect(existsSync(assets.get('icon'))).toBe(true);
  }, 30000);

  it('warns when an unsupported SVG has no raster fallback, and still bakes the vector subset', async () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
    // No `assets` map → no raster fallback available; the <foreignObject> is dropped, the <rect> still bakes.
    const artifact = await bundleWithSvg(
      '<svg viewBox="0 0 10 10"><rect width="10" height="10" fill="lime"/><foreignObject/></svg>',
    );
    const warned = warn.mock.calls.some(
      call =>
        /unsupported SVG feature/.test(String(call[0])) &&
        /foreignObject/.test(String(call[0])),
    );
    warn.mockRestore();
    const art = evalArtifact(artifact);
    expect(art.kind).toBe('vector'); // degrades to the vector subset (the rect)
    expect(warned).toBe(true); // but the dropped <foreignObject> is surfaced, not silent
  });

  it('registers the images an SVG embeds, and keeps their files out of the bundle', async () => {
    const assets = new Map();
    const png = new PNG({width: 1, height: 1});
    png.data.set([0, 0, 255, 255]);
    const url = `data:image/png;base64,${PNG.sync.write(png).toString('base64')}`;
    const artifact = evalArtifact(
      await bundleWithSvg(
        `<svg viewBox="0 0 10 10"><image href="${url}" width="10" height="10"/></svg>`,
        {assets},
      ),
    );
    expect(artifact.kind).toBe('vector');
    expect(artifact.images).toBeUndefined();
    const [[name, path]] = [...assets];
    expect(name).toMatch(/^svg-image-[0-9a-f]{6}$/);
    expect(existsSync(path)).toBe(true);
    expect(artifact.ops[1]).toBe(parseInt(name.slice(-6), 16));
  });

  it("names an imported image by its file, and keeps the embedded images' prefix for them", () => {
    expect(imageAssetName('/app/assets/wx_sun.png')).toBe('wx_sun');
    expect(() => imageAssetName('/app/assets/svg-image-00ab12.png')).toThrow(
      /svg-image-00ab12\.png starts with "svg-image-", which is kept for images embedded in \.svg files/,
    );
  });

  it('fails the build on a reference outside the file', async () => {
    await expect(
      bundleWithSvg(
        '<svg viewBox="0 0 10 10"><image href="photo.png" width="10" height="10"/></svg>',
      ),
    ).rejects.toThrow(/icon\.svg: it references "photo\.png"/);
  });

  const LABEL =
    '<svg viewBox="0 0 40 20"><text x="2" y="14" font-family="Inter" font-size="12">N</text></svg>';

  it('bakes text from a font file in the fonts folder beside the .svg', async () => {
    const artifact = evalArtifact(
      await bundleWithSvg(LABEL, {
        fonts: {'fonts/Inter.ttf': 'Inter-Regular.ttf'},
      }),
    );
    expect(artifact.kind).toBe('vector');
    expect(artifact.ops.length).toBeGreaterThan(2);
  });

  it('bakes text from a font the importing file imports', async () => {
    const artifact = evalArtifact(
      await bundleWithSvg(LABEL, {
        fonts: {'type/Inter.ttf': 'Inter-Regular.ttf'},
        entry: `import Inter from './type/Inter.ttf'; import icon from './icon.svg'; globalThis.__icon = icon;`,
      }),
    );
    expect(artifact.kind).toBe('vector');
    expect(artifact.ops.length).toBeGreaterThan(2);
  });

  it('fails the build with the family when its font is missing', async () => {
    await expect(bundleWithSvg(LABEL)).rejects.toThrow(
      /no font for font-family "Inter"/,
    );
  });
});
