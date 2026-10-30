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

import {
  mkdirSync,
  readFileSync,
  readdirSync,
  rmSync,
  writeFileSync,
} from 'node:fs';
import {basename, dirname, join, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';

import {
  bakeSvgArtifacts,
  compilePackage,
  packPackage,
} from '../../aot/compile.mts';
import {bakeAssetPackBytes} from '../../assets/index.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(HERE, '../../../..');
const FIXTURES = join(HERE, '../erb-parity/fixtures');
const OUT_DIR = join(ROOT, 'tools/erb-fuzz/corpus/seeds');

const SMALL_PANEL = [[240, 320]];

// As erb-parity.mjs compiles it to fit the engine's default vector nodes.
const LIST_CAPS = {'svg-rows': 4};

const fixtures = readdirSync(FIXTURES)
  .filter(file => file.endsWith('.jsx'))
  .map(file => basename(file, '.jsx'))
  .map(name => ({
    name,
    file: join(FIXTURES, `${name}.jsx`),
    panels: SMALL_PANEL,
    listCap: LIST_CAPS[name],
  }));

const SEEDS = [
  ...fixtures,
  // A package with no debug table takes another path through the loader and the error screen.
  {
    name: 'counter-release',
    file: join(FIXTURES, 'counter.jsx'),
    panels: SMALL_PANEL,
    release: true,
  },
  {
    name: 'watch-face',
    file: join(ROOT, 'demos/watch-face/App.jsx'),
    panels: [[240, 280]],
  },
  {
    name: 'thermostat',
    file: join(ROOT, 'demos/thermostat/App.jsx'),
    panels: [
      [240, 320],
      [320, 240],
    ],
  },
];

/** Compiles one seed to its package's bytes. */
async function compileSeed(seed) {
  const src = readFileSync(seed.file, 'utf8');
  const sourceDir = dirname(seed.file);
  const size = ([width, height]) => ({width, height});
  const packaged = compilePackage(src, seed.name, {
    filename: seed.file,
    screen: size(seed.panels[0]),
    listCap: seed.listCap,
    svgArtifacts: await bakeSvgArtifacts(src, sourceDir),
    screens: seed.panels.map(size),
    release: seed.release,
  });
  const images = packaged.images.map(image => ({
    name: image.name,
    path: resolve(sourceDir, image.importPath),
  }));
  const assets = images.length ? bakeAssetPackBytes({images}) : undefined;
  return packPackage(packaged, assets).bytes;
}

rmSync(OUT_DIR, {recursive: true, force: true});
mkdirSync(OUT_DIR, {recursive: true});
for (const seed of SEEDS) {
  const bytes = await compileSeed(seed);
  writeFileSync(join(OUT_DIR, `${seed.name}.erpkg`), bytes);
  console.log(`${seed.name}: ${bytes.length} B`);
}
