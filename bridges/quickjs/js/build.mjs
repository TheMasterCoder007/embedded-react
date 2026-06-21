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

// Bundles a demo (React app + reconciler + host config) into a single classic (IIFE) script that
// QuickJS can run with a plain JS_Eval, and bakes the images/fonts the demo imports into a generated
// C translation unit (dist/assets.generated.c) exposing er_register_assets(). Globals the bundle
// expects at runtime (NativeUI, screen, console, timer shims) are provided by the C host (e.g.,
// examples/linux/main_js.c); the example firmware compiles the generated assets and calls
// er_register_assets() at boot.
//
// Demos live in the top-level demos/ folder, one folder per demo. Pick one with:
//   npm run build                 # default demo (thermostat)
//   npm run build -- marine-dash  # a specific demo by folder name
// Outputs are always dist/app.bundle.js + dist/assets.generated.{c,h} — the single "active" app the
// example hosts pick up.
import { build } from 'esbuild';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { dirname, resolve, basename } from 'node:path';
import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { bakeAssets } from './assets/index.mjs';
import { registerSvgVectorLoader } from './assets/svg-loader.mjs';

const here = dirname(fileURLToPath(import.meta.url)); // bridges/quickjs/js
const repoRoot = resolve(here, '../../..');
const demosDir = resolve(repoRoot, 'demos');
const libEntry = resolve(here, 'src/embedded-react/index.js');
const nodeModules = resolve(here, 'node_modules');
const distDir = resolve(here, 'dist');

const DEFAULT_DEMO = 'thermostat';
const demo = process.argv[2] || process.env.DEMO || DEFAULT_DEMO;
const demoDir = resolve(demosDir, demo);
const entry = resolve(demoDir, 'index.jsx');

if (!existsSync(entry)) {
  const available = existsSync(demosDir)
    ? readdirSync(demosDir, { withFileTypes: true })
        .filter((d) => d.isDirectory())
        .map((d) => d.name)
    : [];
  console.error(`Demo "${demo}" not found (expected ${entry}).`);
  console.error(`Available demos: ${available.join(', ') || '(none)'}`);
  process.exit(1);
}

// Asset discovery is import-driven: an `import x from './x.png'` (image) or `import F from './F.ttf'`
// (font) is intercepted here. The import resolves to the asset's NAME (its file basename) — the
// string an <Image source>/imageName looks up, or the family a fontFamily uses — and the path is
// recorded, so it gets baked below. Only what the app imports is baked.
const images = new Map(); // name -> path
const fonts = new Map(); // family -> path
const assetPlugin = {
  name: 'embedded-react-assets',
  setup(build) {
    build.onLoad({ filter: /\.(png|jpe?g|webp|gif|bmp)$/i }, (args) => {
      const name = basename(args.path).replace(/\.[^.]+$/, '');
      images.set(name, args.path);
      return { contents: `module.exports = ${JSON.stringify(name)};`, loader: 'js' };
    });
    build.onLoad({ filter: /\.(ttf|otf)$/i }, (args) => {
      const family = basename(args.path).replace(/\.[^.]+$/, '');
      fonts.set(family, args.path);
      return { contents: `module.exports = ${JSON.stringify(family)};`, loader: 'js' };
    });
    // .svg imports bake to an inline vector op-tape artifact (no asset-pack entry — see svg-loader.mjs).
    registerSvgVectorLoader(build);
  },
};

const bundlePath = resolve(distDir, 'app.bundle.js');
await build({
  entryPoints: [entry],
  bundle: true,
  format: 'iife',
  outfile: bundlePath,
  platform: 'neutral',
  target: 'es2020',
  jsx: 'automatic',
  // Demos live outside the embedded-react package, so the package self-reference doesn't resolve for
  // them: map the bare `embedded-react` import to the library source and let the demo's bare deps
  // (react, react-reconciler) resolve from this package's node_modules. (The library's own internal
  // imports still resolve relatively / from node_modules as before.)
  alias: { 'embedded-react': libEntry },
  nodePaths: [nodeModules],
  plugins: [assetPlugin],
  // Production React: smaller and avoids dev-only warning machinery that needs more shims.
  define: { 'process.env.NODE_ENV': '"production"' },
  legalComments: 'none',
  logLevel: 'info',
});

console.log(`Bundled demo "${demo}" -> dist/app.bundle.js`);

// --- Bake imported assets ---------------------------------------------------------------------
// Fonts are pre-rasterized at fixed sizes (the engine has no runtime rasterizer), so bake exactly
// the literal fontSize values the bundle uses. Computed/dynamic sizes can't be discovered statically
// and will snap to the nearest baked size at runtime; pin them via assets.config.js if needed.
const bundleSrc = readFileSync(bundlePath, 'utf8');
const discoveredSizes = [
  ...new Set([...bundleSrc.matchAll(/\bfontSize\s*:\s*(\d+(?:\.\d+)?)/g)].map((m) => Math.round(Number(m[1])))),
].sort((a, b) => a - b);

// Optional per-demo overrides: demos/<demo>/assets.config.js
//   export default { fonts: { 'Family': { sizes: [..], bpp: 4, glyphs: 'ascii'|'common'|[cps] } } }
let config = {};
const configPath = resolve(demoDir, 'assets.config.js');
if (existsSync(configPath)) {
  config = (await import(pathToFileURL(configPath).href)).default || {};
}
const fontConfig = config.fonts || {};

const fontJobs = [...fonts.entries()].map(([family, path]) => {
  const fc = fontConfig[family] || {};
  const sizes = fc.sizes && fc.sizes.length ? fc.sizes : discoveredSizes.length ? discoveredSizes : [16];
  return { path, family, sizes, bpp: fc.bpp ?? 4, glyphs: fc.glyphs ?? 'ascii' };
});
const imageJobs = [...images.entries()].map(([name, path]) => ({ path, name }));

const summary = bakeAssets({ images: imageJobs, fonts: fontJobs, outDir: distDir });
const fontDesc = fontJobs.length
  ? fontJobs.map((f) => `${f.family}@[${f.sizes.join(',')}]x${f.bpp}bpp`).join(', ')
  : 'none';
console.log(
  `Baked ${summary.images} image(s), ${summary.fonts} font size(s) -> dist/assets.generated.c\n` +
    `  fonts: ${fontDesc}`,
);
