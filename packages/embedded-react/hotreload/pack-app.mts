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
import type {Plugin} from 'esbuild';
import type {AssetsConfig} from '../assets/font-config.mts';
import type {PackAppOptions} from './pack-split.mts';

const require = createRequire(import.meta.url);

// QuickJS release the bytecode container targets — MUST match cli.mts's QJS_TAG and the device loader.
const QJS_TAG = 'v0.15.0';

/**
 * Bundle + compile + bake an app into an ERCF container Buffer.
 */
export async function packAppContainer({
  entry,
  projectRoot,
  libSrc,
  nodePaths,
  simDir,
  persist = false,
  strip = false,
}: PackAppOptions): Promise<{
  container: Buffer;
  bytecodeLen: number;
  assetsLen: number;
}> {
  const esbuild: typeof import('esbuild') = require('esbuild');
  const {bakeImage} = await import('../assets/bake-image.mts');
  const {bakeFont} = await import('../assets/bake-font.mts');
  const {resolveFontJobs} = await import('../assets/font-config.mts');
  const {analyzeFontSizes, warnFontSizes} =
    await import('../assets/font-sizes.mts');
  const {warnMissingGlyphs} = await import('../assets/glyph-coverage.mts');
  const {emitAssetPack} = await import('../assets/emit-pack.mts');
  const {emitContainer} = await import('../assets/emit-container.mts');
  const {imageAssetName, registerSvgVectorLoader} =
    await import('../assets/svg-loader.mts');
  const {compileToBytecode} = await import('../qjsc-wasm.mts');

  const assetName = (filePath: string) =>
    basename(filePath).replace(/\.[^.]+$/, '');
  const images = new Map<string, string>();
  const fonts = new Map<string, string>();

  // Optionally apply the persist transform to the app's own source (not deps) so plain useState survives
  // a live reload on-device, matching the simulator. The device must have install_persist=true.
  let persistHooks: {
    transformPersist: (code: string, moduleId: string) => string;
    shouldPersist: (path: string, projectRoot: string) => boolean;
    relative: (from: string, to: string) => string;
    normalizedProjectRoot: string;
  } | null = null;
  if (persist) {
    const {transformPersist, shouldPersist} =
      await import('../persist-transform.mts');
    const {relative} = await import('node:path');
    const normalizedProjectRoot = projectRoot.replace(/\\/g, '/');
    persistHooks = {
      transformPersist,
      shouldPersist,
      relative,
      normalizedProjectRoot,
    };
  }

  const assetPlugin: Plugin = {
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
          } catch (error: any) {
            return {errors: [{text: `persist transform: ${error.message}`}]};
          }
        });
      }
      build.onLoad({filter: /\.(png|jpe?g|webp|gif|bmp)$/i}, args => {
        const name = imageAssetName(args.path);
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
  let config: AssetsConfig = {};
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
