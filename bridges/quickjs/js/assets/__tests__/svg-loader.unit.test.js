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

import { describe, it, expect } from 'vitest';
import { build } from 'esbuild';
import { mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { registerSvgVectorLoader } from '../svg-loader.mjs';

async function bundleWithSvg(svg, entry = `import icon from './icon.svg'; globalThis.__icon = icon;`) {
  const dir = mkdtempSync(join(tmpdir(), 'svgld-'));
  writeFileSync(join(dir, 'icon.svg'), svg);
  writeFileSync(join(dir, 'entry.js'), entry);
  const r = await build({
    entryPoints: [join(dir, 'entry.js')],
    bundle: true,
    write: false,
    format: 'iife',
    logLevel: 'silent',
    plugins: [{ name: 'svg', setup: (b) => registerSvgVectorLoader(b) }],
  });
  return r.outputFiles[0].text;
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
    const out = await bundleWithSvg('<svg viewBox="0 0 10 10"><path d="M0 0 L10 0" fill="red"/></svg>');
    const art = evalArtifact(out);
    expect(art.kind).toBe('vector');
    expect(art.width).toBe(10);
    expect(art.height).toBe(10);
    expect(art.ops).toEqual([0, 0, 1, 0, 0, 2, 10, 0]); // [SHAPE,paint, MOVE,0,0, LINE,10,0]
    expect(art.paints[0]).toBe(0xffff0000); // red fill
  });

  it('surfaces a bake failure as an esbuild error (not a silent miscompile)', async () => {
    await expect(bundleWithSvg('<svg><path d="M0 0"')).rejects.toThrow(); // malformed XML
  });
});
