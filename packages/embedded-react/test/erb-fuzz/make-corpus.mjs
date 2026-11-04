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
const FIXTURES = join(HERE, '../erb-frames/fixtures');
const OUT_DIR = join(ROOT, 'tools/erb-fuzz/corpus/seeds');

// As erb-frames.mts compiles it to fit the engine's default vector nodes.
const LIST_CAPS = {'svg-rows': 4};

const fixtures = readdirSync(FIXTURES)
  .filter(file => file.endsWith('.tsx') && !file.endsWith('.d.ts'))
  .map(file => basename(file, '.tsx'))
  .map(name => ({
    name,
    file: join(FIXTURES, `${name}.tsx`),
    listCap: LIST_CAPS[name],
  }));

const SEEDS = [
  ...fixtures,
  // A package with no debug table takes another path through the loader and the error screen.
  {
    name: 'counter-release',
    file: join(FIXTURES, 'counter.tsx'),
    release: true,
  },
  {
    name: 'watch-face',
    file: join(ROOT, 'demos/watch-face/App.jsx'),
  },
  {
    name: 'thermostat',
    file: join(ROOT, 'demos/thermostat/App.jsx'),
  },
];

/** Compiles one seed to its package's bytes. */
async function compileSeed(seed) {
  const source = readFileSync(seed.file, 'utf8');
  const sourceDirectory = dirname(seed.file);
  const packaged = compilePackage(source, seed.name, {
    filename: seed.file,
    listCap: seed.listCap,
    svgArtifacts: await bakeSvgArtifacts(source, sourceDirectory),
    release: seed.release,
  });
  const images = packaged.images.map(image => ({
    name: image.name,
    path: resolve(sourceDirectory, image.importPath),
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
