#!/usr/bin/env node
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

// embedded-react — the consumer CLI.
//
//   npx embedded-react dev [entry] [--port 3333]
//
// Runs the WASM simulator against the app in your current project: bundles your JSX, serves the prebuilt
// embedded-react.wasm host page, and hot-reloads on save (useState preserved). No native toolchain — the
// .wasm ships prebuilt in this package. See tools/web-sim/README.md.

import { copyFileSync, existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { basename, dirname, relative, resolve } from 'node:path';
import { tmpdir } from 'node:os';
import { buildApp, runDevServer } from './sim-server.mjs';

const PKG_ROOT = dirname(fileURLToPath(import.meta.url));
const require = createRequire(import.meta.url);

// QuickJS release the bytecode container targets — MUST match bridges/quickjs/CMakeLists.txt and
// ER_QUICKJS_TAG in er_runtime.c (the device loader rejects a mismatch). The wasm that compiles the
// bytecode embeds this same QuickJS.
const QJS_TAG = 'v0.15.0';

function usage() {
  console.log(`embedded-react — React Native for embedded MCUs

Usage:
  embedded-react dev [entry] [--port <n>]      Run the WASM simulator with hot reload
  embedded-react export [entry] [--out <dir>]  Build a self-contained static playground (no server)
  embedded-react build [entry] [--out <dir>]   Build the device artifact:
                                                 default → app.erpkg (Flow A: QuickJS bytecode + assets,
                                                           upload to the device's config region)
                                                 --aot   → app.gen.c/.h + assets.generated.c (Flow B:
                                                           no QuickJS, compiled into firmware)

  entry   dev/export/build use ./index.jsx, ./src/index.jsx, or package.json "main";
          build --aot uses ./App.jsx or ./src/App.jsx. Pass one to override.
`);
}

/** Locate the package's prebuilt simulator assets (the .wasm ships with this package). */
function simDirOrExit() {
  const simDir = resolve(PKG_ROOT, 'sim');
  if (!existsSync(resolve(simDir, 'embedded-react.wasm'))) {
    console.error('The prebuilt simulator (sim/embedded-react.wasm) is missing from this install.');
    console.error('A published embedded-react package ships it; building from source needs the Emscripten SDK.');
    process.exit(1);
  }
  return simDir;
}

const libSrc = () => resolve(PKG_ROOT, 'src/embedded-react/index.js');
const nodePaths = (cwd) => [resolve(PKG_ROOT, 'node_modules'), resolve(cwd, 'node_modules')];

/** Resolve the app entry: an explicit arg, else common conventions, else package.json "main"/"source". */
function resolveEntry(cwd, explicit) {
  if (explicit) {
    const p = resolve(cwd, explicit);
    if (!existsSync(p)) {
      console.error(`Entry not found: ${p}`);
      process.exit(1);
    }
    return p;
  }
  for (const rel of ['index.jsx', 'src/index.jsx', 'index.tsx', 'src/index.tsx', 'App.jsx', 'src/App.jsx']) {
    const p = resolve(cwd, rel);
    if (existsSync(p)) return p;
  }
  const pkgPath = resolve(cwd, 'package.json');
  if (existsSync(pkgPath)) {
    try {
      const pkg = JSON.parse(readFileSync(pkgPath, 'utf8'));
      for (const field of [pkg.source, pkg.main]) {
        if (field) {
          const p = resolve(cwd, field);
          if (existsSync(p)) return p;
        }
      }
    } catch {
      /* ignore */
    }
  }
  console.error('No app entry found. Pass one explicitly:  embedded-react dev <entry.jsx>');
  console.error('(looked for ./index.jsx, ./src/index.jsx, … and package.json "main")');
  process.exit(1);
}

async function dev(args) {
  const cwd = process.cwd();
  const portIdx = args.indexOf('--port');
  const port = portIdx >= 0 ? parseInt(args[portIdx + 1], 10) : 3333;
  const explicit = args.find((a, i) => !a.startsWith('--') && (portIdx < 0 || i !== portIdx + 1));

  const simDir = simDirOrExit();
  const entry = resolveEntry(cwd, explicit);
  const outDir = resolve(tmpdir(), 'embedded-react-sim');
  mkdirSync(outDir, { recursive: true });

  await runDevServer({
    entry,
    projectRoot: cwd,
    libSrc: libSrc(),
    nodePaths: nodePaths(cwd),
    indexHtml: resolve(simDir, 'index.html'),
    simDir,
    outDir,
    port,
    label: entry,
  });
}

async function exportApp(args) {
  const cwd = process.cwd();
  const outIdx = args.indexOf('--out');
  const outDir = resolve(cwd, outIdx >= 0 ? args[outIdx + 1] : 'sim-export');
  const explicit = args.find((a, i) => !a.startsWith('--') && (outIdx < 0 || i !== outIdx + 1));

  const simDir = simDirOrExit();
  const entry = resolveEntry(cwd, explicit);
  const pub = resolve(outDir, 'public');
  mkdirSync(pub, { recursive: true });

  // Bundle the app + bake assets into public/, then drop the prebuilt module + host page alongside.
  await buildApp({ entry, projectRoot: cwd, libSrc: libSrc(), nodePaths: nodePaths(cwd), outDir: pub });
  for (const f of ['embedded-react.js', 'embedded-react.wasm']) copyFileSync(resolve(simDir, f), resolve(pub, f));
  copyFileSync(resolve(simDir, 'index.html'), resolve(outDir, 'index.html'));

  console.log(`✓ exported a static playground → ${relative(cwd, outDir) || '.'}/`);
  console.log(`  serve it over http (e.g. \`npx serve ${relative(cwd, outDir) || '.'}\`) or deploy the folder to any static host.`);
}

/** Resolve the root component file for the AOT compiler (App.jsx), distinct from the registry entry. */
function resolveAppComponent(cwd, explicit) {
  if (explicit) {
    const p = resolve(cwd, explicit);
    if (!existsSync(p)) {
      console.error(`Entry not found: ${p}`);
      process.exit(1);
    }
    return p;
  }
  for (const rel of ['App.jsx', 'src/App.jsx', 'App.tsx', 'src/App.tsx']) {
    const p = resolve(cwd, rel);
    if (existsSync(p)) return p;
  }
  console.error('No App component found. Pass one explicitly:  embedded-react build --aot <App.jsx>');
  process.exit(1);
}

/** Flow B (--aot): compile the App component to C (app.gen.{c,h}) + bake assets, to compile into firmware. */
async function buildAot(cwd, explicit, outDir) {
  const { compileSource } = await import('./aot/compile.mjs');
  const { bakeAssets } = await import('./assets/index.mjs');
  const appPath = resolveAppComponent(cwd, explicit);
  let result;
  try {
    result = compileSource(readFileSync(appPath, 'utf8'), 'app', { filename: appPath });
  } catch (e) {
    console.error(e && e.aotLoc ? e.message : e?.message || String(e));
    process.exit(1);
  }
  writeFileSync(resolve(outDir, 'app.gen.c'), result.c);
  writeFileSync(resolve(outDir, 'app.gen.h'), result.h);

  const appDir = dirname(appPath);
  const imageJobs = result.images.map((im) => ({ name: im.name, path: resolve(appDir, im.importPath) }));
  for (const j of imageJobs) {
    if (!existsSync(j.path)) {
      console.error(`<Image> asset "${j.name}" not found at ${j.path}`);
      process.exit(1);
    }
  }
  const baked = bakeAssets({ images: imageJobs, fonts: [], outDir });
  console.log(`✓ Flow B (AOT) → ${relative(cwd, outDir) || '.'}/app.gen.c (+ app.gen.h, assets.generated.c — ${baked.images} image(s))`);
  console.log('  No QuickJS on the device: compile these into your firmware against the engine (er_scene.h).');
}

/** Flow A (default): bundle → QuickJS bytecode (via the prebuilt wasm) + baked assets → app.erpkg. */
async function buildContainer(cwd, explicit, outDir) {
  const esbuild = require('esbuild');
  const { bakeImage } = await import('./assets/bake-image.mjs');
  const { bakeFont } = await import('./assets/bake-font.mjs');
  const { emitAssetPack } = await import('./assets/emit-pack.mjs');
  const { emitContainer } = await import('./assets/emit-container.mjs');
  const { registerSvgVectorLoader } = await import('./assets/svg-loader.mjs');
  const { compileToBytecode } = await import('./qjsc-wasm.mjs');

  const simDir = simDirOrExit();
  const entry = resolveEntry(cwd, explicit);

  // Bundle the app (IIFE) with import-driven asset discovery (same as the dev/pack paths).
  const images = new Map();
  const fonts = new Map();
  const assetPlugin = {
    name: 'embedded-react-assets',
    setup(b) {
      b.onLoad({ filter: /\.(png|jpe?g|webp|gif|bmp)$/i }, (a) => {
        const n = basename(a.path).replace(/\.[^.]+$/, '');
        images.set(n, a.path);
        return { contents: `module.exports = ${JSON.stringify(n)};`, loader: 'js' };
      });
      b.onLoad({ filter: /\.(ttf|otf)$/i }, (a) => {
        const f = basename(a.path).replace(/\.[^.]+$/, '');
        fonts.set(f, a.path);
        return { contents: `module.exports = ${JSON.stringify(f)};`, loader: 'js' };
      });
      registerSvgVectorLoader(b);
    },
  };
  // The bundle is an intermediate (it becomes bytecode in the .erpkg) — keep it out of the user's outDir
  // so `dist/` ends up holding only app.erpkg.
  const tmp = resolve(tmpdir(), 'embedded-react-build');
  mkdirSync(tmp, { recursive: true });
  const bundlePath = resolve(tmp, 'app.bundle.js');
  await esbuild.build({
    entryPoints: [entry],
    bundle: true,
    format: 'iife',
    outfile: bundlePath,
    platform: 'neutral',
    target: 'es2020',
    jsx: 'automatic',
    alias: { 'embedded-react': libSrc() },
    nodePaths: nodePaths(cwd),
    plugins: [assetPlugin],
    define: { 'process.env.NODE_ENV': '"production"' },
    legalComments: 'none',
    logLevel: 'silent',
  });
  const bundleSrc = readFileSync(bundlePath, 'utf8');

  // Precompile to QuickJS bytecode through the prebuilt sim wasm — no native toolchain.
  const bytecode = await compileToBytecode(bundleSrc, simDir);

  // Bake imported images/fonts into an ERPK pack (font sizes discovered from the bundle).
  const discoveredSizes = [
    ...new Set([...bundleSrc.matchAll(/\bfontSize\s*:\s*(\d+(?:\.\d+)?)/g)].map((m) => Math.round(Number(m[1])))),
  ].sort((a, b) => a - b);
  let cfg = {};
  const cp = resolve(cwd, 'assets.config.js');
  if (existsSync(cp)) cfg = (await import(pathToFileURL(cp).href)).default || {};
  const fontConfig = cfg.fonts || {};
  const fontJobs = [...fonts.entries()].map(([family, path]) => {
    const fc = fontConfig[family] || {};
    return { path, family, sizes: fc.sizes?.length ? fc.sizes : discoveredSizes.length ? discoveredSizes : [16], bpp: fc.bpp ?? 4, glyphs: fc.glyphs ?? 'ascii' };
  });
  const imageJobs = [...images.entries()].map(([name, path]) => ({ path, name }));
  const bakedImages = imageJobs.map(bakeImage);
  const bakedFonts = fontJobs.map(bakeFont);
  const assetPack = bakedImages.length || bakedFonts.length ? emitAssetPack({ images: bakedImages, fonts: bakedFonts }) : null;

  const container = emitContainer({ bytecode, assetPack, qjsTag: QJS_TAG });
  const outPath = resolve(outDir, 'app.erpkg');
  writeFileSync(outPath, container);
  const kb = (n) => `${(n / 1024).toFixed(1)} KB`;
  console.log(
    `✓ Flow A → ${relative(cwd, outPath) || 'app.erpkg'} (${kb(container.length)}; qjs ${QJS_TAG}, bytecode ${kb(bytecode.length)}` +
      (assetPack ? `, assets ${kb(assetPack.length)}` : '') + ')',
  );
  console.log("  Upload app.erpkg to your device's config region (er_runtime_load_container), or run it on the desktop host.");
}

/** `embedded-react build [--aot] [entry] [--out dir]` — produce the device artifact. */
async function build(args) {
  const cwd = process.cwd();
  const aot = args.includes('--aot');
  const outIdx = args.indexOf('--out');
  const outDir = resolve(cwd, outIdx >= 0 ? args[outIdx + 1] : 'dist');
  const explicit = args.find((a, i) => !a.startsWith('--') && (outIdx < 0 || i !== outIdx + 1));
  mkdirSync(outDir, { recursive: true });
  if (aot) await buildAot(cwd, explicit, outDir);
  else await buildContainer(cwd, explicit, outDir);
}

const [cmd, ...rest] = process.argv.slice(2);
if (cmd === 'dev') {
  await dev(rest);
} else if (cmd === 'export') {
  await exportApp(rest);
} else if (cmd === 'build') {
  await build(rest);
} else if (!cmd || cmd === '--help' || cmd === '-h' || cmd === 'help') {
  usage();
} else {
  console.error(`Unknown command: ${cmd}\n`);
  usage();
  process.exit(1);
}
