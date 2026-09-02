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

// `npm run pack [demo]` — builds a deployable ERCF config container (dist/app.erpkg).
//
// The full release path for Flow A: bundle the app (esbuild → IIFE), precompile it to QuickJS
// bytecode (so the device never ships the parser or source text), bake the imported images/fonts into
// an ERPK pack, and wrap both — with a QuickJS version stamp and an integrity CRC — into a single
// .erpkg. That one file is "the config": load it with er_runtime_load_container() on the desktop, or
// upload it to a device's config region. See bridges/quickjs/js/assets/emit-container.mjs for the
// format, and er_runtime.h for the loader.
//
//   npm run pack                 # default demo (thermostat)
//   npm run pack -- marine-dash  # a specific demo by folder name
//
// Prereq: the bytecode precompiler (er-bridge-quickjs-compile) must be built. This script searches the
// usual build dirs and tells you how to build it if it's missing (override with ER_COMPILE_BIN).
import {build} from 'esbuild';
import {spawnSync} from 'node:child_process';
import {fileURLToPath, pathToFileURL} from 'node:url';
import {dirname, resolve, basename} from 'node:path';
import {
  existsSync,
  readdirSync,
  readFileSync,
  writeFileSync,
  mkdirSync,
} from 'node:fs';
import {bakeImage} from './assets/bake-image.mjs';
import {bakeFont} from './assets/bake-font.mjs';
import {resolveFontJobs} from './assets/font-config.mjs';
import {analyzeFontSizes, warnFontSizes} from './assets/font-sizes.mjs';
import {warnMissingGlyphs} from './assets/glyph-coverage.mjs';
import {emitAssetPack} from './assets/emit-pack.mjs';
import {emitContainer} from './assets/emit-container.mjs';
import {registerSvgVectorLoader} from './assets/svg-loader.mjs';
import {findCompileBin, compileBinHelp} from './compile-bin.mjs';

// QuickJS release the bytecode targets — MUST match the FetchContent pin in
// bridges/quickjs/CMakeLists.txt and ER_QUICKJS_TAG in er_runtime.c. The loader rejects a mismatch.
const QJS_TAG = 'v0.15.0';

const here = dirname(fileURLToPath(import.meta.url)); // bridges/quickjs/js
const repoRoot = resolve(here, '../../..');
const demosDir = resolve(repoRoot, 'demos');
const libEntry = resolve(here, 'src/embedded-react/index.js');
const nodeModules = resolve(here, 'node_modules');
const distDir = resolve(here, 'dist');

const demo = process.argv[2] || process.env.DEMO || 'thermostat';
const demoDir = resolve(demosDir, demo);
const entry = resolve(demoDir, 'index.jsx');
if (!existsSync(entry)) {
  const available = existsSync(demosDir)
    ? readdirSync(demosDir, {withFileTypes: true})
        .filter(d => d.isDirectory())
        .map(d => d.name)
    : [];
  console.error(`Demo "${demo}" not found (expected ${entry}).`);
  console.error(`Available demos: ${available.join(', ') || '(none)'}`);
  process.exit(1);
}

// --- Locate the bytecode precompiler -----------------------------------------------------------
const compileBin = findCompileBin();
if (!compileBin) {
  console.error(compileBinHelp());
  process.exit(1);
}

// --- Bundle the app (same import-driven asset discovery as build.mjs) ---------------------------
const images = new Map(); // name -> path
const fonts = new Map(); // family -> path
const assetPlugin = {
  name: 'embedded-react-assets',
  setup(b) {
    b.onLoad({filter: /\.(png|jpe?g|webp|gif|bmp)$/i}, args => {
      const name = basename(args.path).replace(/\.[^.]+$/, '');
      images.set(name, args.path);
      return {
        contents: `module.exports = ${JSON.stringify(name)};`,
        loader: 'js',
      };
    });
    b.onLoad({filter: /\.(ttf|otf)$/i}, args => {
      const family = basename(args.path).replace(/\.[^.]+$/, '');
      fonts.set(family, args.path);
      return {
        contents: `module.exports = ${JSON.stringify(family)};`,
        loader: 'js',
      };
    });
    registerSvgVectorLoader(b, (name, p) => images.set(name, p)); // raster-fallback SVGs join the image pack
  },
};

mkdirSync(distDir, {recursive: true});
const bundlePath = resolve(distDir, 'app.bundle.js');
await build({
  entryPoints: [entry],
  bundle: true,
  format: 'iife',
  outfile: bundlePath,
  platform: 'neutral',
  target: 'es2020',
  jsx: 'automatic',
  // Pin React to THIS package's copy: a demo/consumer project may carry its own node_modules
  // (e.g., after `npm install` for device-dev deps), and esbuild resolving `react` from there while
  // the library resolves its copy from here yields two React instances — the hook dispatcher is
  // null and every component throws. One copy, always.
  alias: {
    'embedded-react': libEntry,
    react: resolve(nodeModules, 'react'),
    'react-reconciler': resolve(nodeModules, 'react-reconciler'),
    scheduler: resolve(nodeModules, 'scheduler'),
  },
  nodePaths: [nodeModules],
  plugins: [assetPlugin],
  define: {'process.env.NODE_ENV': '"production"'},
  legalComments: 'none',
  logLevel: 'info',
});
console.log(`Bundled demo "${demo}" -> dist/app.bundle.js`);

// --- Precompile to QuickJS bytecode -------------------------------------------------------------
const qbcPath = resolve(distDir, 'app.bundle.qbc');
const cc = spawnSync(compileBin, [bundlePath, qbcPath], {stdio: 'inherit'});
if (cc.status !== 0) {
  console.error('Bytecode compile failed.');
  process.exit(1);
}
const bytecode = readFileSync(qbcPath);

// --- Bake imported assets into an ERPK pack (same sizing rules as build.mjs / sim.mjs) ----------
const bundleSrc = readFileSync(bundlePath, 'utf8');
const usedSizes = analyzeFontSizes(bundleSrc);

let config = {};
const configPath = resolve(demoDir, 'assets.config.js');
if (existsSync(configPath))
  config = (await import(pathToFileURL(configPath).href)).default || {};
const fontConfig = config.fonts || {};
const imageConfig = config.images || {};

const fontJobs = resolveFontJobs(fonts, fontConfig, usedSizes.sizes);
const imageJobs = [...images.entries()].map(([name, path]) => ({
  path,
  name,
  format: imageConfig[name]?.format,
}));

const bakedImages = imageJobs.map(i => bakeImage(i));
const bakedFonts = fontJobs.map(f => bakeFont(f));
warnMissingGlyphs({source: bundleSrc, fonts: bakedFonts});
warnFontSizes({used: usedSizes, fonts: bakedFonts});
const assetPack =
  bakedImages.length || bakedFonts.length
    ? emitAssetPack({images: bakedImages, fonts: bakedFonts})
    : null;
const fontSizeCount = bakedFonts.reduce((n, f) => n + f.sizes.length, 0);

// --- Wrap into the ERCF container ---------------------------------------------------------------
const container = emitContainer({bytecode, assetPack, qjsTag: QJS_TAG});
const outPath = resolve(distDir, 'app.erpkg');
writeFileSync(outPath, container);

const kb = n => `${(n / 1024).toFixed(1)} KB`;
console.log(
  `Packed config -> dist/app.erpkg (${kb(container.length)})\n` +
    `  qjs ${QJS_TAG} · bytecode ${kb(bytecode.length)}` +
    (assetPack
      ? ` · assets ${kb(assetPack.length)} (${bakedImages.length} image(s), ${fontSizeCount} font size(s))`
      : ' · no assets'),
);
