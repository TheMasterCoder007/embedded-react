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
 * It builds the linux-aot SDL host (which loads a package), then for each demo: (1) compiles App.jsx →
 * dist/app.erpkg (`node aot/compile.mts`), (2) runs the host headless via ER_AOT_SHOT to render ONE frame to
 * a BMP, and (3) checks the image actually has content (distinct colors above a floor) — i.e., it compiled,
 * loaded, and rendered something rather than crashing or drawing a blank screen. Exits non-zero if any demo
 * fails, so it can gate CI.
 *
 * Prereq: the linux-aot CMake build must be configurable (SDL2 found). It reuses examples/linux-aot/build;
 * if that isn't configured yet, set CMAKE_TOOLCHAIN_FILE (e.g. a vcpkg toolchain) and it will configure it.
 * Needs a display (SDL video); on a headless box run under a virtual framebuffer.
 */
import {execFileSync} from 'node:child_process';
import {existsSync, mkdirSync, readFileSync, rmSync} from 'node:fs';
import {resolve, dirname} from 'node:path';
import {fileURLToPath} from 'node:url';

// Where a BMP's header keeps what the check reads, and the 'BM' it starts with.
const BMP_MAGIC = [0x42, 0x4d];
const BMP_PIXEL_OFFSET_AT = 10;
const BMP_WIDTH_AT = 18;
const BMP_HEIGHT_AT = 22;
const BMP_BITS_PER_PIXEL_AT = 28;

const here = dirname(fileURLToPath(import.meta.url)); // packages/embedded-react/aot
const packageDirectory = resolve(here, '..'); // packages/embedded-react
const repositoryRoot = resolve(here, '../../..');
const exampleDirectory = resolve(repositoryRoot, 'examples/linux-aot');
const buildDirectory = resolve(exampleDirectory, 'build');
const executable = resolve(
  buildDirectory,
  process.platform === 'win32'
    ? 'embedded-react-desktop-aot.exe'
    : 'embedded-react-desktop-aot',
);
const temporaryDirectory = resolve(packageDirectory, 'dist', '.smoke');

/**
 * The demos to smoke-test. `screen` sets ER_AOT_SCREEN_W/H, the panel the package is for and the window's size;
 * `minColors` is the floor of distinct sampled colors that counts as "rendered".
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
  const bitmap = readFileSync(path);
  if (bitmap[0] !== BMP_MAGIC[0] || bitmap[1] !== BMP_MAGIC[1]) {
    throw new Error(`not a BMP: ${path}`);
  }
  const pixelOffset = bitmap.readUInt32LE(BMP_PIXEL_OFFSET_AT);
  const width = bitmap.readInt32LE(BMP_WIDTH_AT);
  const height = Math.abs(bitmap.readInt32LE(BMP_HEIGHT_AT));
  const bitsPerPixel = bitmap.readUInt16LE(BMP_BITS_PER_PIXEL_AT);
  if (bitsPerPixel !== 24 && bitsPerPixel !== 32) {
    throw new Error(`unexpected BMP bpp ${bitsPerPixel}`);
  }
  const bytesPerPixel = bitsPerPixel / 8;
  const rowSize = Math.floor((bitsPerPixel * width + 31) / 32) * 4; // rows padded to 4 bytes
  const colors = new Set();
  for (let y = 0; y < height; y += step) {
    for (let x = 0; x < width; x += step) {
      const index = pixelOffset + y * rowSize + x * bytesPerPixel;
      colors.add(
        (bitmap[index] << 16) | (bitmap[index + 1] << 8) | bitmap[index + 2],
      ); // BGR triplet
    }
  }
  return colors.size;
}

function run(command, commandArguments, options = {}) {
  execFileSync(command, commandArguments, {stdio: 'pipe', ...options});
}

/** Configures the linux-aot CMake build on the first run. */
function ensureConfigured() {
  if (existsSync(resolve(buildDirectory, 'CMakeCache.txt'))) return;
  console.log('• configuring linux-aot build (first run)…');
  const cmakeArguments = ['-S', exampleDirectory, '-B', buildDirectory];
  if (process.env.CMAKE_TOOLCHAIN_FILE) {
    cmakeArguments.push(
      `-DCMAKE_TOOLCHAIN_FILE=${process.env.CMAKE_TOOLCHAIN_FILE}`,
    );
  }
  if (process.platform === 'win32') {
    cmakeArguments.push('-G', 'MinGW Makefiles');
  }
  run('cmake', cmakeArguments);
}

/**
 * Compiles one demo, renders one frame and checks the frame has content. A step that cannot run (the compiler or
 * the host) throws.
 *
 * @param demo  The demo, from DEMOS.
 *
 * @returns Why the demo failed, or null when it rendered.
 */
function smokeTest(demo) {
  const shot = resolve(temporaryDirectory, `${demo.name}.bmp`);
  rmSync(shot, {force: true});

  // Compile at the demo's screen size, then render one frame to the BMP in a window of that size.
  const screenEnvironment = {...process.env};
  if (demo.screen) {
    screenEnvironment.ER_AOT_SCREEN_W = String(demo.screen.w);
    screenEnvironment.ER_AOT_SCREEN_H = String(demo.screen.h);
  }
  run('node', [resolve(here, 'compile.mts'), demo.name], {
    cwd: packageDirectory,
    env: screenEnvironment,
  });
  run(executable, [resolve(packageDirectory, 'dist/app.erpkg')], {
    cwd: buildDirectory,
    env: {...screenEnvironment, ER_NO_HIDPI: '1', ER_AOT_SHOT: shot},
  });

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
mkdirSync(temporaryDirectory, {recursive: true});
ensureConfigured();
run('cmake', ['--build', buildDirectory]);

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
