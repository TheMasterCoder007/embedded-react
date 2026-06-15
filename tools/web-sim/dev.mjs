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

// dev.mjs — the WASM simulator dev server (phase W3): esbuild --watch + asset baking + hot reload.
//
//   node tools/web-sim/dev.mjs [demo] [--port 3333]
//
// Rebundles demos/<demo> → public/app.js on every save, bakes its imported images/fonts → public/assets.pack,
// and pushes a reload signal (Server-Sent Events) so the open page re-loads the new bundle/pack with NO wasm
// rebuild — the React Native inner loop in a browser. useState survives the reload via the same Babel persist
// transform the SDL simulator uses. Requires a prebuilt module (run `node tools/web-sim/build.mjs` once).
//
// This is the precursor to the consumer CLI `npx embedded-react dev` (W4). Reload uses SSE rather than a
//  WebSocket, so the dev server needs no extra dependency.

import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { existsSync, readFileSync, statSync } from 'node:fs';
import { createRequire } from 'node:module';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { basename, dirname, extname, normalize, relative, resolve } from 'node:path';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = resolve(HERE, '../..');
const JS = resolve(REPO, 'bridges/quickjs/js');
const PUBLIC = resolve(HERE, 'public');

const args = process.argv.slice(2);
const portIdx = args.indexOf('--port');
const PORT = portIdx >= 0 ? parseInt(args[portIdx + 1], 10) : 3333;
const demo = args.find((a) => !a.startsWith('--') && a !== String(PORT)) || process.env.DEMO || 'music-player';

const demoDir = resolve(REPO, 'demos', demo);
const entry = resolve(demoDir, 'index.jsx');
if (!existsSync(entry)) {
  console.error(`Demo "${demo}" not found (expected ${entry}).`);
  process.exit(1);
}
if (!existsSync(resolve(PUBLIC, 'embedded-react.wasm'))) {
  console.error('No built module — run `node tools/web-sim/build.mjs` first.');
  process.exit(1);
}

// esbuild + the asset baker + the persist transform live in the JS package.
const require = createRequire(resolve(JS, 'package.json'));
const esbuild = require('esbuild');
const { bakeAssetPack } = await import(pathToFileURL(resolve(JS, 'assets/index.mjs')).href);
const { transformPersist } = await import(pathToFileURL(resolve(JS, 'persist-transform.mjs')).href);

const bundlePath = resolve(PUBLIC, 'app.js');
const packPath = resolve(PUBLIC, 'assets.pack');
const assetName = (p) => basename(p).replace(/\.[^.]+$/, '');
const mtime = (p) => {
  try {
    return statSync(p).mtimeMs;
  } catch {
    return 0;
  }
};

/*----------------------------------------------------------------------------------------------------------------------
 - SSE reload channel
 ---------------------------------------------------------------------------------------------------------------------*/

const clients = new Set();
function broadcastReload() {
  for (const res of clients) res.write('data: reload\n\n');
}

/*----------------------------------------------------------------------------------------------------------------------
 - Asset baking (per build) — mirrors tools/simulator's sim.mjs
 ---------------------------------------------------------------------------------------------------------------------*/

const images = new Map(); // name -> path
const fonts = new Map(); // family -> path
let lastAssetSig = null; // skip re-baking on pure-JS saves (font rasterization is the cost)

async function bakePack() {
  // Bake exactly the literal fontSizes the bundle uses (the engine has no runtime rasterizer).
  const discoveredSizes = [
    ...new Set([...readFileSync(bundlePath, 'utf8').matchAll(/\bfontSize\s*:\s*(\d+(?:\.\d+)?)/g)].map((m) => Math.round(Number(m[1])))),
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

  const sig = JSON.stringify({
    i: imageJobs.map((j) => [j.name, j.path, mtime(j.path)]).sort(),
    f: fontJobs.map((j) => [j.family, j.path, mtime(j.path), j.sizes, j.bpp, j.glyphs]).sort(),
  });
  if (sig === lastAssetSig) return;
  lastAssetSig = sig;

  if (!imageJobs.length && !fontJobs.length) return; // no assets → no pack (built-in font only)
  try {
    const s = bakeAssetPack({ images: imageJobs, fonts: fontJobs, outPath: packPath });
    console.log(`  assets → ${s.images} image(s), ${s.fonts} font size(s), ${s.bytes} B → public/assets.pack`);
  } catch (e) {
    console.error(`  asset bake failed: ${e.message}`);
  }
}

/*----------------------------------------------------------------------------------------------------------------------
 - esbuild watch
 ---------------------------------------------------------------------------------------------------------------------*/

const ctx = await esbuild.context({
  entryPoints: [entry],
  bundle: true,
  format: 'iife',
  outfile: bundlePath,
  platform: 'neutral',
  target: 'es2020',
  jsx: 'automatic',
  alias: { 'embedded-react': resolve(JS, 'src/embedded-react/index.js') },
  nodePaths: [resolve(JS, 'node_modules')],
  define: { 'process.env.NODE_ENV': '"production"' },
  legalComments: 'none',
  logLevel: 'silent',
  plugins: [
    {
      name: 'embedded-react-websim',
      setup(b) {
        b.onStart(() => {
          images.clear();
          fonts.clear();
        });
        // App files (not the library/React): rewrite useState → a persisting helper so state survives reload.
        b.onLoad({ filter: /\.(jsx?|tsx?)$/ }, (a) => {
          const norm = a.path.replace(/\\/g, '/');
          if (!norm.startsWith(demoDir.replace(/\\/g, '/'))) return undefined;
          try {
            return { contents: transformPersist(readFileSync(a.path, 'utf8'), relative(demoDir, a.path).replace(/\\/g, '/')), loader: 'jsx' };
          } catch (e) {
            return { errors: [{ text: `persist transform: ${e.message}` }] };
          }
        });
        b.onLoad({ filter: /\.(png|jpe?g|webp|gif|bmp)$/i }, (a) => {
          const name = assetName(a.path);
          images.set(name, a.path);
          return { contents: `module.exports = ${JSON.stringify(name)};`, loader: 'js' };
        });
        b.onLoad({ filter: /\.(ttf|otf)$/i }, (a) => {
          const family = assetName(a.path);
          fonts.set(family, a.path);
          return { contents: `module.exports = ${JSON.stringify(family)};`, loader: 'js' };
        });
        b.onEnd(async (r) => {
          if (r.errors.length) {
            console.error(`✗ build failed (${r.errors.length} error(s)) — fix and save to retry`);
            return;
          }
          await bakePack();
          console.log('↻ rebuilt → reloading');
          broadcastReload();
        });
      },
    },
  ],
});

await ctx.rebuild();
await ctx.watch();

/*----------------------------------------------------------------------------------------------------------------------
 - Static file server (+ SSE)
 ---------------------------------------------------------------------------------------------------------------------*/

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.wasm': 'application/wasm',
  '.pack': 'application/octet-stream',
  '.json': 'application/json',
  '.css': 'text/css; charset=utf-8',
};

const server = createServer(async (req, res) => {
  const urlPath = decodeURIComponent(new URL(req.url, 'http://localhost').pathname);

  if (urlPath === '/reload') {
    res.writeHead(200, {
      'content-type': 'text/event-stream',
      'cache-control': 'no-cache',
      connection: 'keep-alive',
    });
    res.write('retry: 1000\n\n');
    clients.add(res);
    req.on('close', () => clients.delete(res));
    return;
  }

  try {
    const rel = urlPath === '/' ? 'index.html' : urlPath.replace(/^\/+/, '');
    const file = normalize(resolve(HERE, rel));
    if (!file.startsWith(HERE)) {
      res.writeHead(403).end('forbidden');
      return;
    }
    const body = await readFile(file);
    res.writeHead(200, {
      'content-type': MIME[extname(file)] || 'application/octet-stream',
      'cache-control': 'no-store',
    }).end(body);
  } catch {
    res.writeHead(404).end('not found');
  }
});

server.listen(PORT, () => {
  console.log(`embedded-react WASM sim (dev) → http://localhost:${PORT}/`);
  console.log(`watching demos/${demo} — edit & save to hot-reload. Ctrl-C to quit.`);
});

const shutdown = async () => {
  try {
    await ctx.dispose();
  } catch {}
  server.close();
  process.exit(0);
};
process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
