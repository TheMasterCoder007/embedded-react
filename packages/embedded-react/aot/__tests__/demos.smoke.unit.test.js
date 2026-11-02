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

import {describe, it, expect} from 'vitest';
import {existsSync, readFileSync, mkdtempSync, rmSync} from 'node:fs';
import {resolve, dirname, join} from 'node:path';
import {tmpdir} from 'node:os';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {compilePackage, bakeSvgArtifacts} from '../compile.mts';
import {analyzeFontSizes, findSizeGaps} from '../../assets/font-sizes.mjs';

// Regression guard: the demos must keep compiling end-to-end (no thrown "AOT: …"). This is the cheap counterpart to
// a full compile-and-screenshot harness — it would have caught any compiler change that broke a demo's lowering
// during development.
const demosDir = resolve(
  dirname(fileURLToPath(import.meta.url)),
  '../../../../demos',
);
const appSrc = demo => readFileSync(resolve(demosDir, demo, 'App.jsx'), 'utf8');

describe('AOT demo compile smoke', () => {
  it('compiles the watch-face demo for the RP2040 240×280 panel', () => {
    const result = compilePackage(appSrc('watch-face'), 'watch-face', {
      screen: {width: 240, height: 280},
      filename: 'demos/watch-face/App.jsx',
    });
    expect(result.program.length).toBeGreaterThan(0);
  });

  it('compiles the thermostat demo with a layout for every panel', async () => {
    const src = appSrc('thermostat');
    const result = compilePackage(src, 'thermostat', {
      filename: 'demos/thermostat/App.jsx',
      svgArtifacts: await bakeSvgArtifacts(
        src,
        resolve(demosDir, 'thermostat'),
      ),
    });
    expect(result.program.length).toBeGreaterThan(0);
  });
});

// The scaffolder's default starters are a newcomer's first build, in either flow — so they must compile
// ahead of time for a no-PSRAM board and use only the font sizes the built-in font is baked at.
describe('create-embedded-react starters build clean', () => {
  const starters = ['template/App.jsx', 'template-typescript/App.tsx'].map(
    rel => [
      rel,
      readFileSync(resolve(demosDir, '../create-embedded-react', rel), 'utf8'),
    ],
  );

  it.each(starters)('%s compiles for a 240×320 panel', (rel, src) => {
    const result = compilePackage(src, 'starter', {
      screen: {width: 240, height: 320},
      filename: rel,
    });
    expect(result.program.length).toBeGreaterThan(0);
  });

  it.each(starters)('%s uses only baked font sizes', (rel, src) => {
    const used = analyzeFontSizes(src);
    expect(used.dynamic).toEqual([]);
    expect(findSizeGaps({sizes: used.sizes}).gaps).toEqual([]);
  });
});

describe('embedded-react build --aot', () => {
  it.each(['thermostat', 'watch-face'])('builds %s to a package', demo => {
    const cwd = resolve(demosDir, demo);
    const out = mkdtempSync(join(tmpdir(), 'er-cli-'));
    try {
      const cli = resolve(demosDir, '../packages/embedded-react/cli.mjs');
      const built = spawnSync(
        process.execPath,
        [cli, 'build', '--aot', '--out', out],
        {cwd, encoding: 'utf8'},
      );
      expect(built.status).toBe(0);
      expect(existsSync(join(out, 'app.erpkg'))).toBe(true);
    } finally {
      rmSync(out, {recursive: true, force: true});
    }
  });
});
