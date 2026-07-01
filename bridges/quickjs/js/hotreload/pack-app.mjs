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

// pack-app — build a Flow A config container (.erpkg) for an app entry, in memory.
//
// The shared core behind both `embedded-react build` (writes dist/app.erpkg) and the on-device
// hot-reload dev loop (streams the bytes straight to the device). Bundles the JSX with esbuild (same
// import-driven asset discovery as the dev/build paths), precompiles to QuickJS bytecode through the
// prebuilt sim wasm, bakes images/fonts into an ERPK pack, and serializes the ERCF container — all
// without touching disk, so the dev loop can rebuild on every save and hand the Buffer to the uploader.

import {createRequire} from 'node:module';
import {pathToFileURL} from 'node:url';
import {basename, resolve} from 'node:path';
import {existsSync, mkdirSync, readFileSync} from 'node:fs';
import {tmpdir} from 'node:os';

const require = createRequire(import.meta.url);

// QuickJS release the bytecode container targets — MUST match cli.mjs's QJS_TAG and the device loader.
const QJS_TAG = 'v0.15.0';

/**
 * Bundle + compile + bake an app into an ERCF container Buffer.
 *
 * @param {object}   o
 * @param {string}   o.entry        App entry (index.jsx/tsx).
 * @param {string}   o.projectRoot  Project cwd (for assets.config.js + persist scoping).
 * @param {string}   o.libSrc       Path to the embedded-react library entry (aliased as 'embedded-react').
 * @param {string[]} o.nodePaths    esbuild nodePaths (package + project node_modules).
 * @param {string}   o.simDir       Dir holding the prebuilt sim wasm (used to compile bytecode).
 * @param {boolean}  [o.persist]    Apply the useState→persist transform (dev hot reload preserves state).
 * @returns {Promise<{container: Buffer, bytecodeLen: number, assetsLen: number}>}
 */
export async function packAppContainer({
  entry,
  projectRoot,
  libSrc,
  nodePaths,
  simDir,
  persist = false,
}) {
  const esbuild = require('esbuild');
  const {bakeImage} = await import('../assets/bake-image.mjs');
  const {bakeFont} = await import('../assets/bake-font.mjs');
  const {emitAssetPack} = await import('../assets/emit-pack.mjs');
  const {emitContainer} = await import('../assets/emit-container.mjs');
  const {registerSvgVectorLoader} = await import('../assets/svg-loader.mjs');
  const {compileToBytecode} = await import('../qjsc-wasm.mjs');

  const assetName = p => basename(p).replace(/\.[^.]+$/, '');
  const images = new Map();
  const fonts = new Map();

  // Optionally apply the persist transform to the app's own source (not deps) so plain useState survives
  // a live reload on-device, matching the simulator. The device must have install_persist=true.
  let persistHooks = null;
  if (persist) {
    const {transformPersist, shouldPersist} =
      await import('../persist-transform.mjs');
    const {relative} = await import('node:path');
    const projNorm = projectRoot.replace(/\\/g, '/');
    persistHooks = {transformPersist, shouldPersist, relative, projNorm};
  }

  const assetPlugin = {
    name: 'embedded-react-assets',
    setup(b) {
      if (persistHooks) {
        b.onLoad({filter: /\.(jsx?|tsx?)$/}, a => {
          const {transformPersist, shouldPersist, relative, projNorm} =
            persistHooks;
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

  const tmp = resolve(tmpdir(), 'embedded-react-build');
  mkdirSync(tmp, {recursive: true});
  const bundlePath = resolve(tmp, 'app.bundle.js');
  await esbuild.build({
    entryPoints: [entry],
    bundle: true,
    format: 'iife',
    outfile: bundlePath,
    platform: 'neutral',
    target: 'es2020',
    jsx: 'automatic',
    alias: {'embedded-react': libSrc},
    nodePaths,
    plugins: [assetPlugin],
    define: {'process.env.NODE_ENV': '"production"'},
    legalComments: 'none',
    logLevel: 'silent',
  });
  const bundleSrc = readFileSync(bundlePath, 'utf8');

  const bytecode = await compileToBytecode(bundleSrc, simDir);

  const discoveredSizes = [
    ...new Set(
      [...bundleSrc.matchAll(/\bfontSize\s*:\s*(\d+(?:\.\d+)?)/g)].map(m =>
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
  const assetPack =
    bakedImages.length || bakedFonts.length
      ? emitAssetPack({images: bakedImages, fonts: bakedFonts})
      : null;

  const container = emitContainer({bytecode, assetPack, qjsTag: QJS_TAG});
  return {
    container,
    bytecodeLen: bytecode.length,
    assetsLen: assetPack ? assetPack.length : 0,
  };
}
