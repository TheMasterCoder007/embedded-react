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
// and shipped prebuilt in the npm package (CI does this on release); consumers never need emsdk. See WASM_SIM.md.
//
// We pass the Emscripten CMake toolchain file directly (derived from `emcc` on PATH) rather than going through
// `emcmake`, which on some setups doesn't inject the toolchain and falls back to the native compiler.

import { execSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';
import { mkdirSync, copyFileSync, rmSync, existsSync, readFileSync } from 'node:fs';

const HERE = dirname(fileURLToPath(import.meta.url));
const BUILD_DIR = resolve(HERE, 'build');
const OUT_DIR = resolve(HERE, 'public');
const debug = process.argv.includes('--debug');
const clean = process.argv.includes('--clean');
const buildType = debug ? 'Debug' : 'Release';

const run = (cmd) => execSync(cmd, { stdio: 'inherit', cwd: HERE });
const has = (c) => {
  try {
    execSync(process.platform === 'win32' ? `where ${c}` : `command -v ${c}`, { stdio: 'ignore' });
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

/** Locate the Emscripten install dir (holds emcc + the CMake toolchain) from $EMSDK or `emcc` on PATH. */
function emscriptenDir() {
  if (process.env.EMSDK) {
    const p = resolve(process.env.EMSDK, 'upstream/emscripten');
    if (existsSync(p)) return p;
  }
  try {
    const which = process.platform === 'win32' ? 'where emcc' : 'command -v emcc';
    const first = execSync(which, { encoding: 'utf8' }).split(/\r?\n/).find(Boolean);
    if (first) return dirname(first.trim());
  } catch {
    /* fall through */
  }
  return null;
}

const emDir = emscriptenDir();
const toolchain = emDir && resolve(emDir, 'cmake/Modules/Platform/Emscripten.cmake');
if (!toolchain || !existsSync(toolchain)) {
  console.error('Could not find the Emscripten SDK (emcc / the CMake toolchain) on PATH.');
  console.error('Install + activate emsdk: https://emscripten.org/docs/getting_started/downloads.html');
  process.exit(1);
}

const gen = pickGenerator();
const genArg = gen ? `-G "${gen}" ` : '';

// A cache from a failed/native configure (wrong compiler) or a different generator can't be reconfigured in
// place — wipe it. (Also honor an explicit --clean.)
const cachePath = resolve(BUILD_DIR, 'CMakeCache.txt');
let needWipe = clean;
if (!needWipe && existsSync(cachePath)) {
  const cache = readFileSync(cachePath, 'utf8');
  const cachedGen = (cache.match(/^CMAKE_GENERATOR:INTERNAL=(.*)$/m) || [])[1];
  const cachedCC = (cache.match(/^CMAKE_C_COMPILER:[^=]*=(.*)$/m) || [])[1] || '';
  const genOk = !gen || cachedGen === gen;
  const ccOk = /emcc/i.test(cachedCC);
  needWipe = !genOk || !ccOk;
}
if (needWipe) {
  rmSync(BUILD_DIR, { recursive: true, force: true });
}
mkdirSync(OUT_DIR, { recursive: true });
console.log(`cmake (Emscripten, ${buildType}${gen ? `, ${gen}` : ''}) → ${OUT_DIR}`);
try {
  // Configure once; re-runs are cheap and pick up CMakeLists edits. First configure clones + builds
  // QuickJS-ng (FetchContent) — a few minutes; subsequent builds are incremental.
  run(`cmake -S "${HERE}" -B "${BUILD_DIR}" ${genArg}-DCMAKE_TOOLCHAIN_FILE="${toolchain}" -DCMAKE_BUILD_TYPE=${buildType}`);
  run(`cmake --build "${BUILD_DIR}" -j`);
} catch (e) {
  console.error('\nbuild failed (see output above).');
  process.exit(e.status || 1);
}

for (const f of ['embedded-react.js', 'embedded-react.wasm']) {
  copyFileSync(resolve(BUILD_DIR, f), resolve(OUT_DIR, f));
}
console.log('✓ built embedded-react.{js,wasm} — serve with: node tools/web-sim/serve.mjs');
