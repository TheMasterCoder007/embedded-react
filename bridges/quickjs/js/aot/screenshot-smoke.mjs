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

// `npm run aot:smoke` — automated compile-and-screenshot smoke test for the Flow B (AOT) demos.
//
// For each demo it: (1) compiles App.jsx → dist/app.gen.{c,h} (`node aot/compile.mjs`), (2) rebuilds the
// linux-aot SDL host (which links the generated C), (3) runs it headless via ER_AOT_SHOT to render ONE
// frame to a BMP, and (4) checks the image actually has content (distinct colours above a floor) — i.e. it
// compiled, linked, and rendered something rather than crashing or drawing a blank screen. Exits non-zero
// if any demo fails, so it can gate CI.
//
// Prereq: the linux-aot CMake build must be configurable (SDL2 found). It reuses examples/linux-aot/build;
// if that isn't configured yet, set CMAKE_TOOLCHAIN_FILE (e.g. a vcpkg toolchain) and it will configure it.
// Needs a display (SDL video); on a headless box run under a virtual framebuffer.
import { execFileSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, rmSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url)); // bridges/quickjs/js/aot
const jsDir = resolve(here, '..'); // bridges/quickjs/js
const repoRoot = resolve(here, '../../../..');
const exampleDir = resolve(repoRoot, 'examples/linux-aot');
const buildDir = resolve(exampleDir, 'build');
const exe = resolve(buildDir, process.platform === 'win32' ? 'embedded-react-desktop-aot.exe' : 'embedded-react-desktop-aot');
const tmpDir = resolve(jsDir, 'dist', '.smoke');

// Demos to smoke-test. `screen` (optional) sets ER_AOT_SCREEN_W/H so the responsive thermostat folds to its
// compact (AOT-compilable) branch. `minColors` is the floor of distinct sampled colours for "rendered".
const DEMOS = [
  { name: 'music-player', minColors: 40 },
  { name: 'thermostat', screen: { w: 240, h: 320 }, minColors: 40 },
];

/** Counts distinct colours in an uncompressed 24/32-bpp BMP (sampled) — a quick "is anything drawn?" signal. */
function bmpDistinctColors(path, step = 4) {
  const b = readFileSync(path);
  if (b[0] !== 0x42 || b[1] !== 0x4d) throw new Error(`not a BMP: ${path}`);
  const off = b.readUInt32LE(10);
  const w = b.readInt32LE(18);
  const h = Math.abs(b.readInt32LE(22));
  const bpp = b.readUInt16LE(28);
  if (bpp !== 24 && bpp !== 32) throw new Error(`unexpected BMP bpp ${bpp}`);
  const bytesPP = bpp / 8;
  const rowSize = Math.floor((bpp * w + 31) / 32) * 4; // rows padded to 4 bytes
  const colors = new Set();
  for (let y = 0; y < h; y += step) {
    for (let x = 0; x < w; x += step) {
      const p = off + y * rowSize + x * bytesPP;
      colors.add((b[p] << 16) | (b[p + 1] << 8) | b[p + 2]); // BGR triplet
    }
  }
  return colors.size;
}

function run(cmd, args, opts = {}) {
  execFileSync(cmd, args, { stdio: 'pipe', ...opts });
}

function ensureConfigured() {
  if (existsSync(resolve(buildDir, 'CMakeCache.txt'))) return;
  console.log('• configuring linux-aot build (first run)…');
  const args = ['-S', exampleDir, '-B', buildDir];
  if (process.env.CMAKE_TOOLCHAIN_FILE) args.push(`-DCMAKE_TOOLCHAIN_FILE=${process.env.CMAKE_TOOLCHAIN_FILE}`);
  if (process.platform === 'win32') args.push('-G', 'MinGW Makefiles');
  run('cmake', args);
}

let failures = 0;
mkdirSync(tmpDir, { recursive: true });
ensureConfigured();

for (const demo of DEMOS) {
  const shot = resolve(tmpDir, `${demo.name}.bmp`);
  try {
    rmSync(shot, { force: true });
    const genEnv = { ...process.env };
    if (demo.screen) {
      genEnv.ER_AOT_SCREEN_W = String(demo.screen.w);
      genEnv.ER_AOT_SCREEN_H = String(demo.screen.h);
    }
    run('node', [resolve(here, 'compile.mjs'), demo.name], { cwd: jsDir, env: genEnv }); // → dist/app.gen.{c,h}
    run('cmake', ['--build', buildDir]); // relink the generated C
    run(exe, [], { cwd: buildDir, env: { ...process.env, ER_AOT_SHOT: shot } }); // render one frame → BMP

    if (!existsSync(shot)) throw new Error('no screenshot written (host crashed before present?)');
    const colors = bmpDistinctColors(shot);
    if (colors < demo.minColors) throw new Error(`screenshot looks blank (${colors} distinct colours < ${demo.minColors})`);
    console.log(`✓ ${demo.name}: rendered (${colors} distinct colours)`);
  } catch (e) {
    failures++;
    console.error(`✗ ${demo.name}: ${e.message?.split('\n')[0] || e}`);
  }
}

console.log(failures ? `\n${failures} demo(s) failed the smoke test.` : `\nAll ${DEMOS.length} demos compiled, built, and rendered.`);
process.exit(failures ? 1 : 0);
