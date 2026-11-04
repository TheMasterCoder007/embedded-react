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
 * @param {object}   options
 * @param {string}   options.entry        App entry (index.jsx/tsx).
 * @param {string}   options.projectRoot  Project cwd (for assets.config.js + persist scoping).
 * @param {string}   options.libSrc       Path to the embedded-react library entry (aliased as 'embedded-react').
 * @param {string[]} options.nodePaths    esbuild nodePaths (package + project node_modules).
 * @param {string}   options.simDir       Dir holding the prebuilt sim wasm (used to compile bytecode).
 * @param {boolean}  [options.persist]    Apply the useState→persist transform (dev hot reload preserves state).
 * @returns {Promise<{container: Buffer, bytecodeLen: number, assetsLen: number}>}
 */
export async function packAppContainer({
  entry,
  projectRoot,
  libSrc,
  nodePaths,
  simDir,
  persist = false,
  strip = false,
}) {
  const esbuild = require('esbuild');
  const {bakeImage} = await import('../assets/bake-image.mjs');
  const {bakeFont} = await import('../assets/bake-font.mjs');
  const {resolveFontJobs} = await import('../assets/font-config.mjs');
  const {analyzeFontSizes, warnFontSizes} =
    await import('../assets/font-sizes.mjs');
  const {warnMissingGlyphs} = await import('../assets/glyph-coverage.mjs');
  const {emitAssetPack} = await import('../assets/emit-pack.mjs');
  const {emitContainer} = await import('../assets/emit-container.mjs');
  const {registerSvgVectorLoader} = await import('../assets/svg-loader.mjs');
  const {compileToBytecode} = await import('../qjsc-wasm.mjs');

  const assetName = filePath => basename(filePath).replace(/\.[^.]+$/, '');
  const images = new Map();
  const fonts = new Map();

  // Optionally apply the persist transform to the app's own source (not deps) so plain useState survives
  // a live reload on-device, matching the simulator. The device must have install_persist=true.
  let persistHooks = null;
  if (persist) {
    const {transformPersist, shouldPersist} =
      await import('../persist-transform.mjs');
    const {relative} = await import('node:path');
    const normalizedProjectRoot = projectRoot.replace(/\\/g, '/');
    persistHooks = {
      transformPersist,
      shouldPersist,
      relative,
      normalizedProjectRoot,
    };
  }

  const assetPlugin = {
    name: 'embedded-react-assets',
    setup(build) {
      if (persistHooks) {
        build.onLoad({filter: /\.(jsx?|tsx?)$/}, args => {
          const {
            transformPersist,
            shouldPersist,
            relative,
            normalizedProjectRoot,
          } = persistHooks;
          if (!shouldPersist(args.path, normalizedProjectRoot))
            return undefined;
          try {
            const loader = args.path.endsWith('.tsx')
              ? 'tsx'
              : args.path.endsWith('.ts')
                ? 'ts'
                : 'jsx';
            return {
              contents: transformPersist(
                readFileSync(args.path, 'utf8'),
                relative(projectRoot, args.path).replace(/\\/g, '/'),
              ),
              loader,
            };
          } catch (error) {
            return {errors: [{text: `persist transform: ${error.message}`}]};
          }
        });
      }
      build.onLoad({filter: /\.(png|jpe?g|webp|gif|bmp)$/i}, args => {
        const name = assetName(args.path);
        images.set(name, args.path);
        return {
          contents: `module.exports = ${JSON.stringify(name)};`,
          loader: 'js',
        };
      });
      build.onLoad({filter: /\.(ttf|otf)$/i}, args => {
        const family = assetName(args.path);
        fonts.set(family, args.path);
        return {
          contents: `module.exports = ${JSON.stringify(family)};`,
          loader: 'js',
        };
      });
      registerSvgVectorLoader(build, (name, pngPath) =>
        images.set(name, pngPath),
      );
    },
  };

  const buildDirectory = resolve(tmpdir(), 'embedded-react-build');
  mkdirSync(buildDirectory, {recursive: true});
  const bundlePath = resolve(buildDirectory, 'app.bundle.js');
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
  const bundleSource = readFileSync(bundlePath, 'utf8');

  const bytecode = await compileToBytecode(bundleSource, simDir, {strip});

  const usedSizes = analyzeFontSizes(bundleSource);
  let config = {};
  const configPath = resolve(projectRoot, 'assets.config.js');
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
  const bakedImages = imageJobs.map(bakeImage);
  const bakedFonts = fontJobs.map(bakeFont);
  warnMissingGlyphs({source: bundleSource, fonts: bakedFonts});
  warnFontSizes({used: usedSizes, fonts: bakedFonts});
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
