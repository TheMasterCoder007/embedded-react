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

// `npm run sim [demo]` — the embedded-react simulator launcher (see /SIMULATOR.md).
//
// Runs esbuild in watch mode (rebundling demos/<demo> → dist/app.bundle.js on every save), bakes the
// demo's imported assets into a binary pack (dist/assets.pack) the simulator loads at runtime, and
// launches the simulator window. Edit your JSX (or an image/font), save, and the window live-reloads
// — JS via the bundle, images/fonts via the pack. The React Native inner loop on the desktop.
//
// One-time setup: build the simulator binary with CMake (tools/simulator). This script tells you how
// if it's missing.
import { context } from 'esbuild';
import { spawn } from 'node:child_process';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { dirname, resolve, basename, relative } from 'node:path';
import { existsSync, readFileSync, statSync } from 'node:fs';
import { bakeAssetPack } from './assets/index.mjs';
import { transformPersist } from './persist-transform.mjs';

const here = dirname(fileURLToPath(import.meta.url)); // bridges/quickjs/js
const repoRoot = resolve(here, '../../..');
const demosDir = resolve(repoRoot, 'demos');
const libEntry = resolve(here, 'src/embedded-react/index.js');
const nodeModules = resolve(here, 'node_modules');
const distDir = resolve(here, 'dist');
const bundlePath = resolve(distDir, 'app.bundle.js');
const packPath = resolve(distDir, 'assets.pack');

const demo = process.argv[2] || process.env.DEMO || 'thermostat';
const demoDir = resolve(demosDir, demo);
const entry = resolve(demoDir, 'index.jsx');
if (!existsSync(entry)) {
  console.error(`Demo "${demo}" not found (expected ${entry}).`);
  process.exit(1);
}

// Locate the built simulator binary (override with ER_SIM_BIN).
const exeSuffix = process.platform === 'win32' ? '.exe' : '';
const simBin =
  process.env.ER_SIM_BIN || resolve(repoRoot, 'tools/simulator/build', `embedded-react-simulator${exeSuffix}`);
if (!existsSync(simBin)) {
  console.error(`Simulator binary not found at:\n  ${simBin}\n`);
  console.error('Build it once (then re-run `npm run sim`):');
  console.error('  cmake -S tools/simulator -B tools/simulator/build [-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake]');
  console.error('  cmake --build tools/simulator/build');
  console.error('(or set ER_SIM_BIN to the binary path)');
  process.exit(1);
}

const assetName = (p) => basename(p).replace(/\.[^.]+$/, '');
const mtime = (p) => {
  try {
    return statSync(p).mtimeMs;
  } catch {
    return 0;
  }
};

// Asset discovery: imports are recorded per build (cleared on onStart), then baked into the pack in
// onEnd. The bundle still references the baked names (the import resolves to the file basename).
const images = new Map(); // name -> path
const fonts = new Map(); // family -> path
let lastAssetSig = null; // skip re-baking the pack on pure-JS saves (font rasterization is the cost)

async function bakePack() {
  // Font sizes: bake exactly the literal fontSizes the bundle uses (engine has no runtime rasterizer).
  const bundleSrc = readFileSync(bundlePath, 'utf8');
  const discoveredSizes = [
    ...new Set([...bundleSrc.matchAll(/\bfontSize\s*:\s*(\d+(?:\.\d+)?)/g)].map((m) => Math.round(Number(m[1])))),
  ].sort((a, b) => a - b);

  let cfg = {};
  const cp = resolve(demoDir, 'assets.config.js');
  if (existsSync(cp)) cfg = (await import(`${pathToFileURL(cp).href}?t=${mtime(cp)}`)).default || {};
  const fontConfig = cfg.fonts || {};

  const fontJobs = [...fonts.entries()].map(([family, path]) => {
    const fc = fontConfig[family] || {};
    const sizes = fc.sizes && fc.sizes.length ? fc.sizes : discoveredSizes.length ? discoveredSizes : [16];
    return { path, family, sizes, bpp: fc.bpp ?? 4, glyphs: fc.glyphs ?? 'ascii' };
  });
  const imageJobs = [...images.entries()].map(([name, path]) => ({ path, name }));

  // Only re-bake when the asset inputs actually changed (avoids re-rasterizing fonts on every save).
  const sig = JSON.stringify({
    i: imageJobs.map((j) => [j.name, j.path, mtime(j.path)]).sort(),
    f: fontJobs.map((j) => [j.family, j.path, mtime(j.path), j.sizes, j.bpp, j.glyphs]).sort(),
  });
  if (sig === lastAssetSig) return;
  lastAssetSig = sig;

  try {
    const s = bakeAssetPack({ images: imageJobs, fonts: fontJobs, outPath: packPath });
    console.log(`  assets → ${s.images} image(s), ${s.fonts} font size(s), ${s.bytes} B → dist/assets.pack`);
  } catch (e) {
    console.error(`  asset bake failed: ${e.message}`);
  }
}

const ctx = await context({
  entryPoints: [entry],
  bundle: true,
  format: 'iife',
  outfile: bundlePath,
  platform: 'neutral',
  target: 'es2020',
  jsx: 'automatic',
  alias: { 'embedded-react': libEntry },
  nodePaths: [nodeModules],
  plugins: [
    {
      name: 'embedded-react-sim',
      setup(b) {
        b.onStart(() => {
          images.clear();
          fonts.clear();
        });
        // App files only (not the library/React): rewrite useState → a persisting helper so state
        // survives reload transparently. Library/node_modules files fall through to esbuild untouched.
        b.onLoad({ filter: /\.(jsx?|tsx?)$/ }, (args) => {
          const norm = args.path.replace(/\\/g, '/');
          if (!norm.startsWith(demoDir.replace(/\\/g, '/'))) return undefined; // not app code
          try {
            const code = transformPersist(readFileSync(args.path, 'utf8'), relative(demoDir, args.path).replace(/\\/g, '/'));
            return { contents: code, loader: 'jsx' };
          } catch (e) {
            return { errors: [{ text: `persist transform: ${e.message}` }] };
          }
        });
        b.onLoad({ filter: /\.(png|jpe?g|webp|gif|bmp)$/i }, (args) => {
          const name = assetName(args.path);
          images.set(name, args.path);
          return { contents: `module.exports = ${JSON.stringify(name)};`, loader: 'js' };
        });
        b.onLoad({ filter: /\.(ttf|otf)$/i }, (args) => {
          const family = assetName(args.path);
          fonts.set(family, args.path);
          return { contents: `module.exports = ${JSON.stringify(family)};`, loader: 'js' };
        });
        b.onEnd(async (r) => {
          if (r.errors.length) {
            console.error(`✗ build failed (${r.errors.length} error(s)) — fix and save to retry`);
            return;
          }
          console.log('↻ rebuilt → dist/app.bundle.js');
          await bakePack();
        });
      },
    },
  ],
  define: { 'process.env.NODE_ENV': '"production"' },
  legalComments: 'none',
  logLevel: 'silent',
});

await ctx.rebuild(); // initial build → bundle + pack
await ctx.watch();
console.log(`Building demo "${demo}" — watching for changes. Close the window or Ctrl-C to quit.`);

// Launch the simulator pointed at the watched bundle + asset pack.
const sim = spawn(simBin, [bundlePath, packPath], { stdio: 'inherit' });
const shutdown = async () => {
  try {
    await ctx.dispose();
  } catch {}
  try {
    sim.kill();
  } catch {}
  process.exit(0);
};
sim.on('exit', shutdown);
process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
