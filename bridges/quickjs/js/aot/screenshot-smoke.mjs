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

/*
 * `npm run aot:smoke` — automated compile-and-screenshot smoke test for the Flow B (AOT) demos.
 *
 * For each demo it: (1) compiles App.jsx → dist/app.gen.{c,h} (`node aot/compile.mts`), (2) rebuilds the
 * linux-aot SDL host (which links the generated C), (3) runs it headless via ER_AOT_SHOT to render ONE
 * frame to a BMP, and (4) checks the image actually has content (distinct colors above a floor) — i.e., it
 * compiled, linked, and rendered something rather than crashing or drawing a blank screen. Exits non-zero
 * if any demo fails, so it can gate CI.
 *
 * Prereq: the linux-aot CMake build must be configurable (SDL2 found). It reuses examples/linux-aot/build;
 * if that isn't configured yet, set CMAKE_TOOLCHAIN_FILE (e.g. a vcpkg toolchain) and it will configure it.
 * Needs a display (SDL video); on a headless box run under a virtual framebuffer.
 */
import {execFileSync} from 'node:child_process';
import {existsSync, mkdirSync, readFileSync, rmSync} from 'node:fs';
import {resolve, dirname} from 'node:path';
import {fileURLToPath} from 'node:url';

const here = dirname(fileURLToPath(import.meta.url)); // bridges/quickjs/js/aot
const jsDir = resolve(here, '..'); // bridges/quickjs/js
const repoRoot = resolve(here, '../../../..');
const exampleDir = resolve(repoRoot, 'examples/linux-aot');
const buildDir = resolve(exampleDir, 'build');
const exe = resolve(
  buildDir,
  process.platform === 'win32'
    ? 'embedded-react-desktop-aot.exe'
    : 'embedded-react-desktop-aot',
);
const tmpDir = resolve(jsDir, 'dist', '.smoke');

/**
 * The demos to smoke-test. `screen` sets ER_AOT_SCREEN_W/H so the responsive thermostat folds to its `solo`
 * (AOT-compilable) branch; `minColors` is the floor of distinct sampled colors that counts as "rendered".
 */
const DEMOS = [
  {name: 'watch-face', screen: {w: 240, h: 280}, minColors: 40},
  {name: 'thermostat', screen: {w: 240, h: 320}, minColors: 40},
];

/**
 * Counts the distinct colors in an uncompressed 24/32-bpp BMP, sampling every `step` pixels — a quick
 * "is anything drawn?" signal.
 *
 * @param path  The BMP file.
 * @param step  The sampling stride, in pixels, along both axes.
 *
 * @returns The number of distinct colors sampled.
 */
function bmpDistinctColors(path, step = 4) {
  const bmp = readFileSync(path);
  if (bmp[0] !== 0x42 || bmp[1] !== 0x4d) {
    throw new Error(`not a BMP: ${path}`);
  }
  const pixelOffset = bmp.readUInt32LE(10);
  const width = bmp.readInt32LE(18);
  const height = Math.abs(bmp.readInt32LE(22));
  const bitsPerPixel = bmp.readUInt16LE(28);
  if (bitsPerPixel !== 24 && bitsPerPixel !== 32) {
    throw new Error(`unexpected BMP bpp ${bitsPerPixel}`);
  }
  const bytesPerPixel = bitsPerPixel / 8;
  const rowSize = Math.floor((bitsPerPixel * width + 31) / 32) * 4; // rows padded to 4 bytes
  const colors = new Set();
  for (let y = 0; y < height; y += step) {
    for (let x = 0; x < width; x += step) {
      const index = pixelOffset + y * rowSize + x * bytesPerPixel;
      colors.add((bmp[index] << 16) | (bmp[index + 1] << 8) | bmp[index + 2]); // BGR triplet
    }
  }
  return colors.size;
}

function run(command, args, options = {}) {
  execFileSync(command, args, {stdio: 'pipe', ...options});
}

/** Configures the linux-aot CMake build on the first run. */
function ensureConfigured() {
  if (existsSync(resolve(buildDir, 'CMakeCache.txt'))) return;
  console.log('• configuring linux-aot build (first run)…');
  const args = ['-S', exampleDir, '-B', buildDir];
  if (process.env.CMAKE_TOOLCHAIN_FILE) {
    args.push(`-DCMAKE_TOOLCHAIN_FILE=${process.env.CMAKE_TOOLCHAIN_FILE}`);
  }
  if (process.platform === 'win32') {
    args.push('-G', 'MinGW Makefiles');
  }
  run('cmake', args);
}

/**
 * Compiles one demo, relinks the host, renders one frame and checks the frame has content. A step that cannot
 * run (the compiler, the build or the host) throws.
 *
 * @param demo  The demo, from DEMOS.
 *
 * @returns Why the demo failed, or null when it rendered.
 */
function smokeTest(demo) {
  const shot = resolve(tmpDir, `${demo.name}.bmp`);
  rmSync(shot, {force: true});

  // Compile with the demo's screen size, relink the host, then render one frame to the BMP.
  const compileEnv = {...process.env};
  if (demo.screen) {
    compileEnv.ER_AOT_SCREEN_W = String(demo.screen.w);
    compileEnv.ER_AOT_SCREEN_H = String(demo.screen.h);
  }
  run('node', [resolve(here, 'compile.mts'), demo.name], {
    cwd: jsDir,
    env: compileEnv,
  });
  run('cmake', ['--build', buildDir]);
  run(exe, [], {cwd: buildDir, env: {...process.env, ER_AOT_SHOT: shot}});

  // The frame must exist and show more than a blank screen.
  if (!existsSync(shot)) {
    return 'no screenshot written (host crashed before present?)';
  }
  const colors = bmpDistinctColors(shot);
  if (colors < demo.minColors) {
    return `screenshot looks blank (${colors} distinct colors < ${demo.minColors})`;
  }
  console.log(`✓ ${demo.name}: rendered (${colors} distinct colors)`);
  return null;
}

let failures = 0;
mkdirSync(tmpDir, {recursive: true});
ensureConfigured();

for (const demo of DEMOS) {
  let failure;
  try {
    failure = smokeTest(demo);
  } catch (error) {
    failure = error.message?.split('\n')[0] || String(error);
  }
  if (failure) {
    failures++;
    console.error(`✗ ${demo.name}: ${failure}`);
  }
}

console.log(
  failures
    ? `\n${failures} demo(s) failed the smoke test.`
    : `\nAll ${DEMOS.length} demos compiled, built, and rendered.`,
);
process.exit(failures ? 1 : 0);
