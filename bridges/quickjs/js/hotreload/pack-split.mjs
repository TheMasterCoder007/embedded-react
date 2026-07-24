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

// pack-split — build the vendor/app SPLIT of a Flow A app, for incremental hot reload.
//
// The monolithic packer (pack-app.mjs) inlines react + react-reconciler + the embedded-react library
// together with the user's source into one ~1 MB bytecode blob. But the framework half never changes
// when you edit your app, so re-shipping and re-parsing it on every save is pure waste. pack-split cuts
// the bundle along the one seam that matters:
//
//   • vendor chunk — react, react/jsx-runtime, and the embedded-react library. Built once. It registers
//     those modules in `globalThis.__er_modules` and installs a `globalThis.require(id)` shim that the
//     app chunk resolves its imports through. ~94% of the bytecode, and stable across edits.
//   • app chunk — the user's own source, with react / react/jsx-runtime / embedded-react marked EXTERNAL
//     so esbuild emits `require(...)` calls instead of inlining them. ~6% of the bytecode; the only thing
//     that changes on a save.
//
// The flashed boot container carries BOTH (vendor section + app section); the device runs vendor first,
// then the app. A hot-reload frame carries only the app chunk — the device keeps the resident vendor and
// swaps just the app (a "soft reset"). Old monolithic containers still boot unchanged (emit-container's
// vendor section is optional), so this is backwards-compatible, and production behavior is identical.

import {createRequire} from 'node:module';
import {pathToFileURL} from 'node:url';
import {basename, dirname, resolve, relative} from 'node:path';
import {existsSync, readFileSync} from 'node:fs';

const require = createRequire(import.meta.url);

// QuickJS release the bytecode container targets — MUST match cli.mjs's QJS_TAG, pack-app.mjs, and the
// device loader.
const QJS_TAG = 'v0.15.0';

// The shared deps the app chunk resolve through the vendor registry instead of inlining.
export const VENDOR_EXTERNALS = [
  'react',
  'react/jsx-runtime',
  'embedded-react',
];

const esbuildCommon = nodePaths => ({
  bundle: true,
  format: 'iife',
  platform: 'neutral',
  target: 'es2020',
  jsx: 'automatic',
  nodePaths,
  define: {'process.env.NODE_ENV': '"production"'},
  legalComments: 'none',
  logLevel: 'silent',
  write: false,
});

/**
 * The vendor chunk's synthetic entry source: import the shared deps, register them in a runtime module
 * registry, and install the `require()` shim the app chunk resolves through. esbuild's `__require` helper
 * (emitted for the app chunk's external imports) finds this global `require` at runtime.
 */
function vendorEntrySource() {
  return `import * as React from 'react';
import * as JsxRuntime from 'react/jsx-runtime';
import * as EmbeddedReact from 'embedded-react';
const reg = (globalThis.__er_modules = globalThis.__er_modules || {});
reg['react'] = React;
reg['react/jsx-runtime'] = JsxRuntime;
reg['embedded-react'] = EmbeddedReact;
globalThis.require = function (id) {
  const m = reg[id];
  if (!m) throw new Error('embedded-react: module not in vendor registry: ' + id);
  return m;
};
`;
}

/**
 * Bundle the vendor chunk (react + reconciler + embedded-react lib) to a self-contained IIFE source.
 *
 * @param {object}   o
 * @param {string}   o.libSrc     embedded-react library entry (aliased as 'embedded-react').
 * @param {string[]} o.nodePaths  esbuild nodePaths (package + project node_modules).
 * @returns {Promise<string>} The bundled JS source.
 */
export async function bundleVendorSource({libSrc, nodePaths}) {
  const esbuild = require('esbuild');
  const out = await esbuild.build({
    ...esbuildCommon(nodePaths),
    stdin: {
      contents: vendorEntrySource(),
      resolveDir: dirname(libSrc),
      sourcefile: 'vendor-entry.js',
      loader: 'js',
    },
    alias: {'embedded-react': libSrc},
  });
  return out.outputFiles[0].text;
}

/**
 * Build an asset-discovering esbuild plugin shared with the monolithic packer's behavior: image/font/svg
 * imports become bare string names (the bytes live in the ERPK pack), and — when persist is on — the app's
 * own source gets the useState→persist transform so state survives a live reload.
 */
function makeAssetPlugin({images, fonts, persistHooks, projectRoot}) {
  const assetName = p => basename(p).replace(/\.[^.]+$/, '');
  return {
    name: 'embedded-react-assets',
    async setup(b) {
      const {registerSvgVectorLoader} = await import(
        '../assets/svg-loader.mjs'
      );
      if (persistHooks) {
        b.onLoad({filter: /\.(jsx?|tsx?)$/}, a => {
          const {transformPersist, shouldPersist, projNorm} = persistHooks;
          if (!shouldPersist(a.path, projNorm)) return undefined;
          try {
            const loader = a.path.endsWith('.tsx')
              ? 'tsx'
              : a.path.endsWith('.ts')
                ? 'ts'
                : 'jsx';
            return {
              contents: transformPersist(
                readFileSync(a.path, 'utf8'),
                relative(projectRoot, a.path).replace(/\\/g, '/'),
              ),
              loader,
            };
          } catch (e) {
            return {errors: [{text: `persist transform: ${e.message}`}]};
          }
        });
      }
      b.onLoad({filter: /\.(png|jpe?g|webp|gif|bmp)$/i}, a => {
        const n = assetName(a.path);
        images.set(n, a.path);
        return {
          contents: `module.exports = ${JSON.stringify(n)};`,
          loader: 'js',
        };
      });
      b.onLoad({filter: /\.(ttf|otf)$/i}, a => {
        const f = assetName(a.path);
        fonts.set(f, a.path);
        return {
          contents: `module.exports = ${JSON.stringify(f)};`,
          loader: 'js',
        };
      });
      registerSvgVectorLoader(b, (name, p) => images.set(name, p));
    },
  };
}

/**
 * Bundle the app chunk: the user's entry with the shared deps marked external (resolved at runtime through
 * the vendor registry). Returns the bundled source plus the discovered image/font maps for baking.
 */
export async function bundleAppSource({
  entry,
  projectRoot,
  nodePaths,
  persist = false,
}) {
  const esbuild = require('esbuild');
  const images = new Map();
  const fonts = new Map();

  let persistHooks = null;
  if (persist) {
    const {transformPersist, shouldPersist} = await import(
      '../persist-transform.mjs'
    );
    persistHooks = {
      transformPersist,
      shouldPersist,
      projNorm: projectRoot.replace(/\\/g, '/'),
    };
  }

  const out = await esbuild.build({
    ...esbuildCommon(nodePaths),
    entryPoints: [entry],
    external: VENDOR_EXTERNALS,
    plugins: [makeAssetPlugin({images, fonts, persistHooks, projectRoot})],
  });
  return {source: out.outputFiles[0].text, images, fonts};
}

/** Bake the discovered images/fonts into an ERPK asset pack (or null if there are none). */
async function bakeAssets({images, fonts, source, projectRoot}) {
  const {bakeImage} = await import('../assets/bake-image.mjs');
  const {bakeFont} = await import('../assets/bake-font.mjs');
  const {emitAssetPack} = await import('../assets/emit-pack.mjs');

  const discoveredSizes = [
    ...new Set(
      [...source.matchAll(/\bfontSize\s*:\s*(\d+(?:\.\d+)?)/g)].map(m =>
        Math.round(Number(m[1])),
      ),
    ),
  ].sort((a, b) => a - b);

  let cfg = {};
  const cp = resolve(projectRoot, 'assets.config.js');
  if (existsSync(cp))
    cfg = (await import(pathToFileURL(cp).href)).default || {};
  const fontConfig = cfg.fonts || {};

  const fontJobs = [...fonts.entries()].map(([family, path]) => {
    const fc = fontConfig[family] || {};
    return {
      path,
      family,
      sizes: fc.sizes?.length
        ? fc.sizes
        : discoveredSizes.length
          ? discoveredSizes
          : [16],
      bpp: fc.bpp ?? 4,
      glyphs: fc.glyphs ?? 'ascii',
    };
  });
  const imageJobs = [...images.entries()].map(([name, path]) => ({path, name}));
  const bakedImages = imageJobs.map(bakeImage);
  const bakedFonts = fontJobs.map(bakeFont);
  return bakedImages.length || bakedFonts.length
    ? emitAssetPack({images: bakedImages, fonts: bakedFonts})
    : null;
}

/**
 * Compile the vendor chunk to a QuickJS bytecode blob. Built once per dev session (it changes only when a
 * dependency changes), so the per-edit reload never re-ships it.
 *
 * @returns {Promise<{bytecode: Buffer, bytecodeLen: number}>}
 */
export async function packVendor({libSrc, nodePaths, simDir, strip = false}) {
  const {compileToBytecode} = await import('../qjsc-wasm.mjs');
  const source = await bundleVendorSource({libSrc, nodePaths});
  const bytecode = await compileToBytecode(source, simDir, {strip});
  return {bytecode, bytecodeLen: bytecode.length};
}

/**
 * Compile the app chunk to a QuickJS bytecode blob and bake its assets. This is the only thing rebuilt on
 * each save — and the only thing a hot-reload frame carries.
 *
 * @returns {Promise<{bytecode: Buffer, assetPack: Buffer|null, bytecodeLen: number, assetsLen: number}>}
 */
export async function packApp({
  entry,
  projectRoot,
  libSrc,
  nodePaths,
  simDir,
  persist = false,
  strip = false,
}) {
  const {compileToBytecode} = await import('../qjsc-wasm.mjs');
  const {source, images, fonts} = await bundleAppSource({
    entry,
    projectRoot,
    nodePaths,
    persist,
  });
  const bytecode = await compileToBytecode(source, simDir, {strip});
  const assetPack = await bakeAssets({images, fonts, source, projectRoot});
  return {
    bytecode,
    assetPack,
    bytecodeLen: bytecode.length,
    assetsLen: assetPack ? assetPack.length : 0,
  };
}

/**
 * Assemble the flashed BOOT container: vendor + app + assets in one ERCF. The device runs vendor first
 * (installs the registry + require shim), registers assets, then runs the app. Used by `embedded-react
 * build` so the same firmware/artifact serves both production and hot reload.
 */
export function emitBootContainer({vendorBytecode, appBytecode, assetPack}) {
  return emit({bytecode: appBytecode, vendorBytecode, assetPack});
}

/**
 * Assemble a hot-reload APP frame: the app chunk + its assets, NO vendor. The device keeps the resident
 * vendor and swaps just the app (soft reset). This is the ~64 KB that travels on each save.
 */
export function emitAppFrame({appBytecode, assetPack}) {
  return emit({bytecode: appBytecode, assetPack});
}

// emitBootContainer/emitAppFrame are async (emit() dynamically imports the emitter) — callers await them.
async function emit(opts) {
  const {emitContainer} = await import('../assets/emit-container.mjs');
  return emitContainer({...opts, qjsTag: QJS_TAG});
}

export {QJS_TAG};
