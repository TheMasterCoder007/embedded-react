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

// build.mjs — compile embedded-react.{js,wasm} via Emscripten + CMake.
//
//   node tools/web-sim/build.mjs            configure (if needed) + build to tools/web-sim/public/
//   node tools/web-sim/build.mjs --debug    Debug build (-O0 -g, assertions)
//   node tools/web-sim/build.mjs --clean    wipe the CMake build dir first
//
// Uses the CMake project in tools/web-sim/CMakeLists.txt, which reuses bridges/quickjs (engine + QuickJS-ng +
// er_runtime + bridge) so the module runs Flow A (QuickJS inside WASM). The .wasm is app-agnostic — built once
// and shipped prebuilt in the npm package (CI does this on release); consumers never need emsdk. See tools/web-sim/README.md.
//
// We pass the Emscripten CMake toolchain file directly (derived from `emcc` on PATH) rather than going through
// `emcmake`, which on some setups doesn't inject the toolchain and falls back to the native compiler. Set
// EMSCRIPTEN_ROOT (= `em-config EMSCRIPTEN_ROOT`) to point at an install we can't derive on our own.

import {execSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {dirname, resolve} from 'node:path';
import {
  mkdirSync,
  copyFileSync,
  rmSync,
  existsSync,
  readFileSync,
  realpathSync,
} from 'node:fs';

const HERE = dirname(fileURLToPath(import.meta.url));
const BUILD_DIR = resolve(HERE, 'build');
const OUT_DIR = resolve(HERE, 'public');
const debug = process.argv.includes('--debug');
const clean = process.argv.includes('--clean');
const buildType = debug ? 'Debug' : 'Release';

const run = cmd => execSync(cmd, {stdio: 'inherit', cwd: HERE});
const has = c => {
  try {
    execSync(process.platform === 'win32' ? `where ${c}` : `command -v ${c}`, {
      stdio: 'ignore',
    });
    return true;
  } catch {
    return false;
  }
};

/** Pick a generator that doesn't need MSVC: Ninja if present, else MinGW Makefiles on Windows, else default. */
function pickGenerator() {
  if (has('ninja')) return 'Ninja';
  if (process.platform === 'win32') return 'MinGW Makefiles';
  return null; // CMake default (Unix Makefiles on Linux/macOS)
}

/** Where an Emscripten install keeps its CMake toolchain file. */
const TOOLCHAIN_REL = 'cmake/Modules/Platform/Emscripten.cmake';

/** Resolve symlinks where we can; a path that doesn't exist comes back unchanged. */
const real = p => {
  try {
    return realpathSync(p);
  } catch {
    return p;
  }
};

/**
 * The toolchain file of the Emscripten install at — or next door to — `dir`, else null.
 *
 * Package managers don't put `emcc` in the install root, and following the symlink isn't enough on its
 * own: Homebrew's `bin/emcc` links to a *wrapper script* in the Cellar's `bin/` that execs the real
 * `libexec/emcc`, so neither the PATH entry nor its realpath is the directory holding `cmake/`. Probe
 * the usual neighbors (emsdk's `upstream/emscripten`, Homebrew's `../libexec`) as well.
 */
function toolchainNear(dir) {
  if (!dir) return null;
  for (const base of new Set([dir, real(dir)])) {
    for (const cand of [
      base,
      resolve(base, 'upstream/emscripten'),
      resolve(base, '../libexec'),
      resolve(base, '..'),
    ]) {
      const p = resolve(cand, TOOLCHAIN_REL);
      if (existsSync(p)) return p;
    }
  }
  return null;
}

/** First non-empty line of `cmd`'s stdout, or null if it fails. */
function firstLine(cmd) {
  try {
    const out = execSync(cmd, {
      encoding: 'utf8',
      stdio: ['ignore', 'pipe', 'ignore'],
    });
    return (out.split(/\r?\n/).find(l => l.trim()) || '').trim() || null;
  } catch {
    return null;
  }
}

/**
 * Locate the Emscripten CMake toolchain file: $EMSCRIPTEN_ROOT (explicit override), then the emsdk
 * layout under $EMSDK, then emcc's own idea of its root, then `emcc` on PATH.
 */
function findToolchain() {
  const forced = process.env.EMSCRIPTEN_ROOT;
  if (forced) {
    const t = toolchainNear(forced);
    if (t) return t;
    console.error(`EMSCRIPTEN_ROOT=${forced} has no ${TOOLCHAIN_REL}.`);
    console.error('Set it to the output of `em-config EMSCRIPTEN_ROOT`.');
    process.exit(1);
  }
  const which = process.platform === 'win32' ? 'where emcc' : 'command -v emcc';
  const emcc = firstLine(which);
  for (const dir of [
    process.env.EMSDK,
    firstLine('em-config EMSCRIPTEN_ROOT'), // emcc telling us where it lives
    emcc && dirname(real(emcc)), // through the PATH symlink, to the install's own bin/
    emcc && dirname(emcc),
  ]) {
    const t = dir && toolchainNear(dir);
    if (t) return t;
  }
  return null;
}

const toolchain = findToolchain();
if (!toolchain) {
  console.error(
    `Could not find the Emscripten CMake toolchain (${TOOLCHAIN_REL}).`,
  );
  console.error(
    'Checked $EMSCRIPTEN_ROOT, $EMSDK, `em-config EMSCRIPTEN_ROOT` and `emcc` on PATH.',
  );
  console.error(
    'Install + activate emsdk: https://emscripten.org/docs/getting_started/downloads.html',
  );
  console.error(
    'Or point at an install you already have:  EMSCRIPTEN_ROOT="$(em-config EMSCRIPTEN_ROOT)" node tools/web-sim/build.mjs',
  );
  process.exit(1);
}

const gen = pickGenerator();
const genArg = gen ? `-G "${gen}" ` : '';

// A cache from a failed/native configure (wrong compiler) or a different generator can't be reconfigured in
// place — wipe it. (Also honor an explicit --clean.)
const cachePath = resolve(BUILD_DIR, 'CMakeCache.txt');
let needWipe = clean;
let toolchainArg = toolchain;
if (!needWipe && existsSync(cachePath)) {
  const cache = readFileSync(cachePath, 'utf8');
  const cachedGen = (cache.match(/^CMAKE_GENERATOR:INTERNAL=(.*)$/m) || [])[1];
  const cachedTc =
    (cache.match(/^CMAKE_TOOLCHAIN_FILE:[^=]*=(.*)$/m) || [])[1] || '';
  const genOk = !gen || cachedGen === gen;
  // Key off the toolchain file, not CMAKE_C_COMPILER: the Emscripten toolchain re-forces the compilers
  // on every configure and never caches that variable, so testing it wiped the cache — and paid for a
  // full rebuild — on every single run. A native/failed cache has no toolchain line at all.
  const tcOk = !!cachedTc && real(cachedTc) === real(toolchain);
  needWipe = !genOk || !tcOk;
  // Same install spelled differently (Homebrew's opt/ symlink vs the versioned Cellar path): keep the
  // cache's spelling so CMake doesn't see the compiler move under it.
  if (!needWipe) toolchainArg = cachedTc;
}
if (needWipe) {
  rmSync(BUILD_DIR, {recursive: true, force: true});
}
mkdirSync(OUT_DIR, {recursive: true});
console.log(
  `cmake (Emscripten, ${buildType}${gen ? `, ${gen}` : ''}) → ${OUT_DIR}`,
);
console.log(`  toolchain: ${toolchainArg}`);
try {
  // Configure once; re-runs are cheap and pick up CMakeLists edits. First configure clones + builds
  // QuickJS-ng (FetchContent) — a few minutes; subsequent builds are incremental.
  run(
    `cmake -S "${HERE}" -B "${BUILD_DIR}" ${genArg}-DCMAKE_TOOLCHAIN_FILE="${toolchainArg}" -DCMAKE_BUILD_TYPE=${buildType}`,
  );
  run(`cmake --build "${BUILD_DIR}" -j`);
} catch (e) {
  console.error('\nbuild failed (see output above).');
  process.exit(e.status || 1);
}

// Stage a .cjs copy alongside the .js: the host page loads the .js as a classic <script>, but the npm
// package is "type": "module", so Node would treat the .js as ESM and the emscripten CommonJS export
// would never run. `embedded-react build` requires the .cjs (forced CommonJS) to compile bytecode in Node.
const stage = dir => {
  copyFileSync(
    resolve(BUILD_DIR, 'embedded-react.js'),
    resolve(dir, 'embedded-react.js'),
  );
  copyFileSync(
    resolve(BUILD_DIR, 'embedded-react.js'),
    resolve(dir, 'embedded-react.cjs'),
  );
  copyFileSync(
    resolve(BUILD_DIR, 'embedded-react.wasm'),
    resolve(dir, 'embedded-react.wasm'),
  );
};
stage(OUT_DIR);

// Stage the prebuilt module + host page into the npm package's sim/ dir so `npx embedded-react dev` ships
// with it (the `files` whitelist includes sim/). This is what CI builds + publishes; not committed.
const PKG_SIM = resolve(HERE, '../../bridges/quickjs/js/sim');
mkdirSync(PKG_SIM, {recursive: true});
stage(PKG_SIM);
copyFileSync(resolve(HERE, 'index.html'), resolve(PKG_SIM, 'index.html'));

console.log(
  '✓ built embedded-react.{js,wasm} → tools/web-sim/public/ + bridges/quickjs/js/sim/',
);
console.log(
  '  repo preview: node tools/web-sim/dev.mjs [demo]   ·   consumer CLI: npx embedded-react dev',
);
