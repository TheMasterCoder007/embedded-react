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
import {existsSync, mkdtempSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {registerSvgVectorLoader} from '../svg-loader.mjs';

// `assets` (optional) = a Map the loader registers raster-fallback PNGs into (name -> png path), mirroring a
// real bundler's image collection. When omitted, the loader has no raster fallback (vector-only).
async function bundleWithSvg(
  svg,
  {
    entry = `import icon from './icon.svg'; globalThis.__icon = icon;`,
    assets,
  } = {},
) {
  const directory = mkdtempSync(join(tmpdir(), 'svgld-'));
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
        setup: pluginBuild => registerSvgVectorLoader(pluginBuild, addRaster),
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
      '<svg viewBox="0 0 20 20" width="20" height="20"><text x="2" y="10">hi</text></svg>',
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
  });

  it('warns when an unsupported SVG has no raster fallback, and still bakes the vector subset', async () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
    // No `assets` map → no raster fallback available; the <text> is dropped, the <rect> still bakes.
    const artifact = await bundleWithSvg(
      '<svg viewBox="0 0 10 10"><rect width="10" height="10" fill="lime"/><text>x</text></svg>',
    );
    const warned = warn.mock.calls.some(
      call =>
        /unsupported SVG feature/.test(String(call[0])) &&
        /text/.test(String(call[0])),
    );
    warn.mockRestore();
    const art = evalArtifact(artifact);
    expect(art.kind).toBe('vector'); // degrades to the vector subset (the rect)
    expect(warned).toBe(true); // but the dropped <text> is surfaced, not silent
  });
});
