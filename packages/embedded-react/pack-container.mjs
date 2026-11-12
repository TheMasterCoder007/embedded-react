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
// upload it to a device's config region. See packages/embedded-react/assets/emit-container.mjs for the
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
import {loadAppFile, loadDependencyFile} from './aot/app-file-loader.mts';
import {demoFile} from './aot/demo-file.mts';

// QuickJS release the bytecode targets — MUST match the FetchContent pin in
// bridges/quickjs/CMakeLists.txt and ER_QUICKJS_TAG in er_runtime.c. The loader rejects a mismatch.
const QUICKJS_TAG = 'v0.15.0';
const BYTES_PER_KILOBYTE = 1024;

const here = dirname(fileURLToPath(import.meta.url)); // packages/embedded-react
const repoRoot = resolve(here, '../..');
const demosDir = resolve(repoRoot, 'demos');
const libEntry = resolve(here, 'src/embedded-react/index.js');
const nodeModules = resolve(here, 'node_modules');
const distDir = resolve(here, 'dist');

const demo = process.argv[2] || process.env.DEMO || 'thermostat';
const demoDir = resolve(demosDir, demo);
const entry = demoFile(demoDir, 'index');
if (!existsSync(entry)) {
  const available = existsSync(demosDir)
    ? readdirSync(demosDir, {withFileTypes: true})
        .filter(entry => entry.isDirectory())
        .map(entry => entry.name)
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
  setup(pluginBuild) {
    // The demo's own files give its native modules their signatures, and so do a package's.
    pluginBuild.onLoad({filter: /\.(jsx?|tsx?)$/}, args =>
      args.path.startsWith(demoDir)
        ? loadAppFile(args.path)
        : loadDependencyFile(args.path),
    );
    pluginBuild.onLoad({filter: /\.(png|jpe?g|webp|gif|bmp)$/i}, args => {
      const name = basename(args.path).replace(/\.[^.]+$/, '');
      images.set(name, args.path);
      return {
        contents: `module.exports = ${JSON.stringify(name)};`,
        loader: 'js',
      };
    });
    pluginBuild.onLoad({filter: /\.(ttf|otf)$/i}, args => {
      const family = basename(args.path).replace(/\.[^.]+$/, '');
      fonts.set(family, args.path);
      return {
        contents: `module.exports = ${JSON.stringify(family)};`,
        loader: 'js',
      };
    });
    registerSvgVectorLoader(pluginBuild, (name, svgPath) =>
      images.set(name, svgPath),
    ); // raster-fallback SVGs join the image pack
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
const bytecodePath = resolve(distDir, 'app.bundle.qbc');
const compileResult = spawnSync(compileBin, [bundlePath, bytecodePath], {
  stdio: 'inherit',
});
if (compileResult.status !== 0) {
  console.error('Bytecode compile failed.');
  process.exit(1);
}
const bytecode = readFileSync(bytecodePath);

// --- Bake imported assets into an ERPK pack (same sizing rules as build.mjs / sim.mjs) ----------
const bundleSource = readFileSync(bundlePath, 'utf8');
const usedSizes = analyzeFontSizes(bundleSource);

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

const bakedImages = imageJobs.map(imageJob => bakeImage(imageJob));
const bakedFonts = fontJobs.map(fontJob => bakeFont(fontJob));
warnMissingGlyphs({source: bundleSource, fonts: bakedFonts});
warnFontSizes({used: usedSizes, fonts: bakedFonts});
const assetPack =
  bakedImages.length || bakedFonts.length
    ? emitAssetPack({images: bakedImages, fonts: bakedFonts})
    : null;
const fontSizeCount = bakedFonts.reduce(
  (count, font) => count + font.sizes.length,
  0,
);

// --- Wrap into the ERCF container ---------------------------------------------------------------
const container = emitContainer({bytecode, assetPack, qjsTag: QUICKJS_TAG});
const outPath = resolve(distDir, 'app.erpkg');
writeFileSync(outPath, container);

const kb = byteCount => `${(byteCount / BYTES_PER_KILOBYTE).toFixed(1)} KB`;
console.log(
  `Packed config -> dist/app.erpkg (${kb(container.length)})\n` +
    `  qjs ${QUICKJS_TAG} · bytecode ${kb(bytecode.length)}` +
    (assetPack
      ? ` · assets ${kb(assetPack.length)} (${bakedImages.length} image(s), ${fontSizeCount} font size(s))`
      : ' · no assets'),
);
