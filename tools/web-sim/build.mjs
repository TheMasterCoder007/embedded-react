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

// build.mjs — compile the engine + software + web backends to embedded-react.wasm via Emscripten.
//
//   node tools/web-sim/build.mjs            build to tools/web-sim/public/
//   node tools/web-sim/build.mjs --debug    -O0 -g, assertions on
//
// W1 scope: NO QuickJS. The module renders a static C scene (er_web_demo_scene) to prove the
// engine -> WASM -> canvas pipeline. Flow A (QuickJS inside WASM, er_web_load_source) lands in W2.
//
// Because it runs Flow A later, the .wasm is app-agnostic and is built once and shipped prebuilt in the
// npm package (CI does this on release) — consumers never need emsdk. See WASM_SIM.md.

import { execSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';
import { mkdirSync } from 'node:fs';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(HERE, '..', '..');
const OUT_DIR = resolve(HERE, 'public');
const debug = process.argv.includes('--debug');

// Engine sources — kept in lockstep with engine/CMakeLists.txt (EMBEDDED_REACT_SOURCES).
const ENGINE_SOURCES = [
  'core/native_renderer.c',
  'scene/compositor.c',
  'scene/hit_test.c',
  'layout/layout_engine.c',
  'rendering/canvas_bindings.c',
  'rendering/gradient.c',
  'rendering/image_registry.c',
  'rendering/image_scaler.c',
  'rendering/perf_overlay.c',
  'rendering/rrect.c',
  'rendering/scratch_pool.c',
  'rendering/shadow.c',
  'rendering/transform.c',
  'rendering/vector.c',
  'text/text_renderer.c',
  'animation/animation.c',
  'animation/layout_anim.c',
  'font/font_bitmap.c',
  'font/font_blob.c',
  'font/font_data.c',
  'font/font_registry.c',
].map((s) => resolve(ROOT, 'engine', s));

const SOURCES = [
  ...ENGINE_SOURCES,
  resolve(ROOT, 'backends/software/renderer_backend.c'),
  resolve(ROOT, 'backends/web/renderer_backend.c'),
];

const INCLUDE_DIRS = [
  'engine/include',
  'engine/core',
  'engine/scene',
  'engine/layout',
  'engine/rendering',
  'engine/text',
  'engine/animation',
  'engine/font',
  'engine/platform',
  'backends/software',
  'backends/web',
].map((d) => `-I${resolve(ROOT, d)}`);

// Engine feature flags — a capable, desktop-class preview (faithful to an ARGB device). Gradient + full
// transforms + bilinear + AA on; shadows off to keep the module small (toggle later if a demo needs them).
// Scratch is sized to cover a full-width node on common boards (<= 480 px) under transforms/opacity.
const DEFINES = [
  'ERUI_BORDER_AA=1',
  'ERUI_GRADIENT=1',
  'ERUI_GRADIENT_RADIAL=1',
  'ERUI_BILINEAR_SCALE=1',
  'ERUI_SHADOWS=0',
  'ERUI_3D_TRANSFORMS=0',
  'ERUI_ONSCREEN_KEYBOARD=0',
  'ERUI_TRANSFORMS_FULL=1',
  'ERUI_FONT_SIZES=7',
  'ERUI_MAX_NODES=512',
  'ERUI_MAX_OPACITY_DEPTH=4',
  'ERUI_SCRATCH_W=480',
  'ERUI_SCRATCH_H=480',
].map((d) => `-D${d}`);

// The flat C ABI the host page calls via cwrap (leading underscore = C symbol name in the .wasm).
const EXPORTS = [
  '_er_web_init',
  '_er_web_demo_scene',
  '_er_web_resize',
  '_er_web_pump',
  '_er_web_touch',
  '_er_web_reset',
  '_er_web_framebuffer',
  '_er_web_fb_width',
  '_er_web_fb_height',
  '_malloc',
  '_free',
];

const EMFLAGS = [
  '-sMODULARIZE=1',
  '-sEXPORT_NAME=createEmbeddedReact',
  '-sALLOW_MEMORY_GROWTH=1',
  '-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,HEAPU8',
  `-sEXPORTED_FUNCTIONS=${EXPORTS.join(',')}`,
  '-sENVIRONMENT=web',
];

const optFlags = debug
  ? ['-O0', '-g', '-sASSERTIONS=1', '-sSAFE_HEAP=1']
  : ['-O2', '-sASSERTIONS=0'];

mkdirSync(OUT_DIR, { recursive: true });

const args = [
  '-std=c99',
  ...optFlags,
  ...DEFINES,
  ...INCLUDE_DIRS,
  ...SOURCES,
  ...EMFLAGS,
  '-o',
  resolve(OUT_DIR, 'embedded-react.js'),
];

// emcc is a shim (.bat / shell script) on most installs; run it through the shell as a single command
// string so PATH lookup + the right extension resolve the same way they do in an interactive terminal.
const quote = (a) => (/[\s]/.test(a) ? `"${a}"` : a);
const cmd = ['emcc', ...args].map(quote).join(' ');
console.log(`emcc -> ${resolve(OUT_DIR, 'embedded-react.{js,wasm}')}${debug ? '  (debug)' : ''}`);
try {
  execSync(cmd, { stdio: 'inherit', cwd: ROOT });
} catch (e) {
  console.error('\nemcc build failed. Is the Emscripten SDK installed and `emcc` on PATH?');
  console.error('Install: https://emscripten.org/docs/getting_started/downloads.html');
  process.exit(e.status || 1);
}
console.log('✓ built embedded-react.wasm — open tools/web-sim/index.html via `node tools/web-sim/serve.mjs`');
