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

// `npm run sim [demo]` — the embedded-react simulator launcher (see tools/simulator/README.md).
//
// Runs esbuild in watch mode (rebundling demos/<demo> → dist/app.bundle.js on every save), bakes the
// demo's imported assets into a binary pack (dist/assets.pack) the simulator loads at runtime, and
// launches the simulator window. Edit your JSX (or an image/font), save, and the window live-reloads
// — JS via the bundle, images/fonts via the pack. The React Native inner loop on the desktop.
//
// One-time setup: build the simulator binary with CMake (tools/simulator). This script tells you how
// if it's missing.
import {context} from 'esbuild';
import {spawn} from 'node:child_process';
import {fileURLToPath, pathToFileURL} from 'node:url';
import {dirname, resolve, basename, relative} from 'node:path';
import {existsSync, readFileSync, statSync} from 'node:fs';
import {bakeAssetPack} from './assets/index.mjs';
import {resolveFontJobs} from './assets/font-config.mjs';
import {analyzeFontSizes, fontSizeSignature} from './assets/font-sizes.mjs';
import {textSignature} from './assets/glyph-coverage.mjs';
import {registerSvgVectorLoader} from './assets/svg-loader.mjs';
import {transformPersist} from './persist-transform.mjs';
import {loadAppFile, loadDependencyFile} from './aot/app-file-loader.mts';
import {demoFile} from './aot/demo-file.mts';

const here = dirname(fileURLToPath(import.meta.url)); // packages/embedded-react
const repoRoot = resolve(here, '../..');
const demosDir = resolve(repoRoot, 'demos');
const libEntry = resolve(here, 'src/embedded-react/index.js');
const nodeModules = resolve(here, 'node_modules');
const distDir = resolve(here, 'dist');
const bundlePath = resolve(distDir, 'app.bundle.js');
const packPath = resolve(distDir, 'assets.pack');

const demo = process.argv[2] || process.env.DEMO || 'thermostat';
const demoDir = resolve(demosDir, demo);
const entry = demoFile(demoDir, 'index');
if (!existsSync(entry)) {
  console.error(`Demo "${demo}" not found (expected ${entry}).`);
  process.exit(1);
}

// Locate the built simulator binary (override with ER_SIM_BIN).
const exeSuffix = process.platform === 'win32' ? '.exe' : '';
const simulatorBinary =
  process.env.ER_SIM_BIN ||
  resolve(
    repoRoot,
    'tools/simulator/build',
    `embedded-react-simulator${exeSuffix}`,
  );
if (!existsSync(simulatorBinary)) {
  console.error(`Simulator binary not found at:\n  ${simulatorBinary}\n`);
  console.error('Build it once (then re-run `npm run sim`):');
  console.error(
    '  cmake -S tools/simulator -B tools/simulator/build [-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake]',
  );
  console.error('  cmake --build tools/simulator/build');
  console.error('(or set ER_SIM_BIN to the binary path)');
  process.exit(1);
}

const assetName = filePath => basename(filePath).replace(/\.[^.]+$/, '');
const mtime = filePath => {
  try {
    return statSync(filePath).mtimeMs;
  } catch {
    return 0;
  }
};

// Asset discovery: imports are recorded per build (cleared on onStart), then baked into the pack in
// onEnd. The bundle still references the baked names (the import resolves to the file basename).
const images = new Map(); // name -> path
const fonts = new Map(); // family -> path
let lastAssetSignature = null; // skip re-baking the pack on pure-JS saves (font rasterization is the cost)

async function bakePack() {
  // Font sizes: bake exactly the sizes the bundle uses, constants folded (engine has no rasterizer).
  const bundleSource = readFileSync(bundlePath, 'utf8');
  const usedSizes = analyzeFontSizes(bundleSource);

  let assetsConfig = {};
  const assetsConfigPath = resolve(demoDir, 'assets.config.js');
  if (existsSync(assetsConfigPath))
    assetsConfig =
      (
        await import(
          `${pathToFileURL(assetsConfigPath).href}?t=${mtime(assetsConfigPath)}`
        )
      ).default || {};
  const fontConfig = assetsConfig.fonts || {};
  const imageConfig = assetsConfig.images || {};

  const fontJobs = resolveFontJobs(fonts, fontConfig, usedSizes.sizes);
  const imageJobs = [...images.entries()].map(([name, path]) => ({
    path,
    name,
    format: imageConfig[name]?.format,
  }));

  // Only re-bake when the asset inputs actually changed (avoids re-rasterizing fonts on every save).
  // The app's text counts as an input: new characters and new font sizes must both be re-checked
  // against the bake, even when no font or image file changed.
  const assetSignature = JSON.stringify({
    text: textSignature(bundleSource),
    fontSizes: fontSizeSignature(usedSizes),
    images: imageJobs
      .map(job => [job.name, job.path, mtime(job.path), job.format])
      .sort(),
    fonts: fontJobs
      .map(job => [
        job.family,
        job.path,
        mtime(job.path),
        job.sizes,
        job.bpp,
        job.glyphs,
        job.extraGlyphs,
      ])
      .sort(),
  });
  if (assetSignature === lastAssetSignature) return;
  lastAssetSignature = assetSignature;

  try {
    const summary = bakeAssetPack({
      images: imageJobs,
      fonts: fontJobs,
      outPath: packPath,
      source: bundleSource,
      usedSizes,
    });
    console.log(
      `  assets → ${summary.images} image(s), ${summary.fonts} font size(s), ${summary.bytes} B → dist/assets.pack`,
    );
  } catch (error) {
    console.error(`  asset bake failed: ${error.message}`);
  }
}

const buildContext = await context({
  entryPoints: [entry],
  bundle: true,
  format: 'iife',
  outfile: bundlePath,
  platform: 'neutral',
  target: 'es2020',
  jsx: 'automatic',
  alias: {'embedded-react': libEntry},
  nodePaths: [nodeModules],
  plugins: [
    {
      name: 'embedded-react-sim',
      setup(pluginBuild) {
        pluginBuild.onStart(() => {
          images.clear();
          fonts.clear();
        });
        // App files only (not the library/React): rewrite useState → a persisting helper so state
        // survives reload transparently, and run native modules on their mocks. Library/node_modules files fall
        // through to esbuild untouched, but a package's native modules run on their mocks too.
        pluginBuild.onLoad({filter: /\.(jsx?|tsx?)$/}, async args => {
          const normalizedPath = args.path.replace(/\\/g, '/');
          if (!normalizedPath.startsWith(demoDir.replace(/\\/g, '/')))
            return loadDependencyFile(args.path, {mocks: true}); // not app code
          try {
            return await loadAppFile(
              args.path,
              source =>
                transformPersist(
                  source,
                  relative(demoDir, args.path).replace(/\\/g, '/'),
                ),
              {mocks: true},
            );
          } catch (error) {
            return {errors: [{text: `persist transform: ${error.message}`}]};
          }
        });
        pluginBuild.onLoad({filter: /\.(png|jpe?g|webp|gif|bmp)$/i}, args => {
          const name = assetName(args.path);
          images.set(name, args.path);
          return {
            contents: `module.exports = ${JSON.stringify(name)};`,
            loader: 'js',
          };
        });
        pluginBuild.onLoad({filter: /\.(ttf|otf)$/i}, args => {
          const family = assetName(args.path);
          fonts.set(family, args.path);
          return {
            contents: `module.exports = ${JSON.stringify(family)};`,
            loader: 'js',
          };
        });
        registerSvgVectorLoader(pluginBuild, (name, svgPath) =>
          images.set(name, svgPath),
        ); // raster-fallback SVGs join the image pack
        pluginBuild.onEnd(async result => {
          if (result.errors.length) {
            console.error(
              `✗ build failed (${result.errors.length} error(s)) — fix and save to retry`,
            );
            return;
          }
          console.log('↻ rebuilt → dist/app.bundle.js');
          await bakePack();
        });
      },
    },
  ],
  define: {'process.env.NODE_ENV': '"production"'},
  legalComments: 'none',
  logLevel: 'silent',
});

await buildContext.rebuild(); // initial build → bundle + pack
await buildContext.watch();
console.log(
  `Building demo "${demo}" — watching for changes. Close the window or Ctrl-C to quit.`,
);

// Launch the simulator pointed at the watched bundle + asset pack.
const simulator = spawn(simulatorBinary, [bundlePath, packPath], {
  stdio: 'inherit',
});
const shutdown = async () => {
  try {
    await buildContext.dispose();
  } catch {}
  try {
    simulator.kill();
  } catch {}
  process.exit(0);
};
simulator.on('exit', shutdown);
process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
