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
import {
  compilePackage,
  bakeSvgArtifacts,
  readNativeFiles,
} from '../compile.mts';
import {demoFile} from '../demo-file.mts';
import {analyzeFontSizes, findSizeGaps} from '../../assets/font-sizes.mjs';

// Regression guard: the demos must keep compiling end-to-end (no thrown "AOT: …"). This is the cheap counterpart to
// a full compile-and-screenshot harness — it would have caught any compiler change that broke a demo's lowering
// during development.
const demosDirectory = resolve(
  dirname(fileURLToPath(import.meta.url)),
  '../../../../demos',
);
const appPath = demo => demoFile(resolve(demosDirectory, demo), 'App');
const appSource = demo => readFileSync(appPath(demo), 'utf8');

describe('AOT demo compile smoke', () => {
  it('compiles the watch-face demo for the RP2040 240×280 panel', () => {
    const source = appSource('watch-face');
    const result = compilePackage(source, 'watch-face', {
      screen: {width: 240, height: 280},
      filename: appPath('watch-face'),
      nativeFiles: readNativeFiles(
        source,
        resolve(demosDirectory, 'watch-face'),
      ),
    });
    expect(result.program.length).toBeGreaterThan(0);
  });

  it('compiles the thermostat demo with a layout for every panel', async () => {
    const source = appSource('thermostat');
    const result = compilePackage(source, 'thermostat', {
      filename: 'demos/thermostat/App.jsx',
      svgArtifacts: await bakeSvgArtifacts(
        source,
        resolve(demosDirectory, 'thermostat'),
      ),
    });
    expect(result.program.length).toBeGreaterThan(0);
  });
});

// The scaffolder's default starters are a newcomer's first build, in either flow — so they must compile
// ahead of time for a no-PSRAM board and use only the font sizes the built-in font is baked at.
describe('create-embedded-react starters build clean', () => {
  const starters = ['template/App.jsx', 'template-typescript/App.tsx'].map(
    relativePath => [
      relativePath,
      readFileSync(
        resolve(demosDirectory, '../create-embedded-react', relativePath),
        'utf8',
      ),
    ],
  );

  it.each(starters)(
    '%s compiles for a 240×320 panel',
    (relativePath, source) => {
      const result = compilePackage(source, 'starter', {
        screen: {width: 240, height: 320},
        filename: relativePath,
      });
      expect(result.program.length).toBeGreaterThan(0);
    },
  );

  it.each(starters)('%s uses only baked font sizes', (relativePath, source) => {
    const used = analyzeFontSizes(source);
    expect(used.dynamic).toEqual([]);
    expect(findSizeGaps({sizes: used.sizes}).gaps).toEqual([]);
  });
});

describe('embedded-react build --aot', () => {
  it.each(['thermostat', 'watch-face'])('builds %s to a package', demo => {
    const demoDirectory = resolve(demosDirectory, demo);
    const outputDirectory = mkdtempSync(join(tmpdir(), 'er-cli-'));
    try {
      const cliPath = resolve(
        demosDirectory,
        '../packages/embedded-react/cli.mjs',
      );
      const built = spawnSync(
        process.execPath,
        [cliPath, 'build', '--aot', '--out', outputDirectory],
        {cwd: demoDirectory, encoding: 'utf8'},
      );
      expect(built.status).toBe(0);
      expect(existsSync(join(outputDirectory, 'app.erpkg'))).toBe(true);
    } finally {
      rmSync(outputDirectory, {recursive: true, force: true});
    }
  });
});
