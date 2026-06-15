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

// sim-server.mjs — the shared WASM-simulator dev server.
//
// esbuild --watch a JSX app → app.js, bake its imported images/fonts → assets.pack, serve the prebuilt
// embedded-react.{js,wasm} host page, and push a Server-Sent "reload" on every rebuild so the open browser
// hot-reloads with no wasm rebuild. useState survives the reload via the Babel persist transform.
//
// Used by both the consumer CLI (cli.mjs → `npx embedded-react dev`) and the repo dev loop
// (tools/web-sim/dev.mjs). The only differences between the two are paths, passed in here.

import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { existsSync, readFileSync, statSync } from 'node:fs';
import { createRequire } from 'node:module';
import { pathToFileURL } from 'node:url';
import { basename, dirname, extname, relative, resolve } from 'node:path';

const HERE = dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const require = createRequire(import.meta.url);
const esbuild = require('esbuild');
const { bakeAssetPack } = await import(pathToFileURL(resolve(HERE, 'assets/index.mjs')).href);
const { transformPersist } = await import(pathToFileURL(resolve(HERE, 'persist-transform.mjs')).href);

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.wasm': 'application/wasm',
  '.pack': 'application/octet-stream',
};
const assetName = (p) => basename(p).replace(/\.[^.]+$/, '');
const mtime = (p) => {
  try {
    return statSync(p).mtimeMs;
  } catch {
    return 0;
  }
};

/**
 * Run the simulator dev server: watch + bundle + bake assets + serve + hot reload.
 *
 * @param {object}   o
 * @param {string}   o.entry        App entry (index.jsx) to bundle.
 * @param {string}   o.projectRoot  Dir whose files get the persist transform (the app's own code).
 * @param {string}   o.libSrc       Path to embedded-react's src index (alias target for `embedded-react`).
 * @param {string[]} o.nodePaths    Extra node_modules dirs for esbuild resolution.
 * @param {string}   o.indexHtml    Host page to serve at `/`.
 * @param {string}   o.simDir       Dir holding embedded-react.{js,wasm}.
 * @param {string}   o.outDir       Dir to write app.js + assets.pack (also served).
 * @param {number}   o.port         HTTP port.
 * @param {string}   o.label        App/demo name for logs.
 */
export async function runDevServer({ entry, projectRoot, libSrc, nodePaths, indexHtml, simDir, outDir, port, label }) {
  const bundlePath = resolve(outDir, 'app.js');
  const packPath = resolve(outDir, 'assets.pack');
  const projNorm = projectRoot.replace(/\\/g, '/');

  // SSE reload channel.
  const clients = new Set();
  const broadcastReload = () => {
    for (const res of clients) res.write('data: reload\n\n');
  };

  // Per-build asset discovery → ERPK pack.
  const images = new Map();
  const fonts = new Map();
  let lastAssetSig = null;
  async function bakePack() {
    const discoveredSizes = [
      ...new Set(
        [...readFileSync(bundlePath, 'utf8').matchAll(/\bfontSize\s*:\s*(\d+(?:\.\d+)?)/g)].map((m) => Math.round(Number(m[1]))),
      ),
    ].sort((a, b) => a - b);

    let cfg = {};
    const cp = resolve(projectRoot, 'assets.config.js');
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
    if (!imageJobs.length && !fontJobs.length) return; // built-in font only → no pack

    try {
      const s = bakeAssetPack({ images: imageJobs, fonts: fontJobs, outPath: packPath });
      console.log(`  assets → ${s.images} image(s), ${s.fonts} font size(s), ${s.bytes} B`);
    } catch (e) {
      console.error(`  asset bake failed: ${e.message}`);
    }
  }

  const ctx = await esbuild.context({
    entryPoints: [entry],
    bundle: true,
    format: 'iife',
    outfile: bundlePath,
    platform: 'neutral',
    target: 'es2020',
    jsx: 'automatic',
    alias: { 'embedded-react': libSrc },
    nodePaths,
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
            if (!a.path.replace(/\\/g, '/').startsWith(projNorm)) return undefined;
            try {
              return { contents: transformPersist(readFileSync(a.path, 'utf8'), relative(projectRoot, a.path).replace(/\\/g, '/')), loader: 'jsx' };
            } catch (e) {
              return { errors: [{ text: `persist transform: ${e.message}` }] };
            }
          });
          b.onLoad({ filter: /\.(png|jpe?g|webp|gif|bmp)$/i }, (a) => {
            images.set(assetName(a.path), a.path);
            return { contents: `module.exports = ${JSON.stringify(assetName(a.path))};`, loader: 'js' };
          });
          b.onLoad({ filter: /\.(ttf|otf)$/i }, (a) => {
            fonts.set(assetName(a.path), a.path);
            return { contents: `module.exports = ${JSON.stringify(assetName(a.path))};`, loader: 'js' };
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

  // Static server. The host page references ./public/<name>; embedded-react.{js,wasm} come from simDir, the
  // freshly bundled app.js/assets.pack from outDir.
  const send = async (res, file) => {
    try {
      res.writeHead(200, { 'content-type': MIME[extname(file)] || 'application/octet-stream', 'cache-control': 'no-store' }).end(await readFile(file));
    } catch {
      res.writeHead(404).end('not found');
    }
  };
  const server = createServer((req, res) => {
    const path = decodeURIComponent(new URL(req.url, 'http://localhost').pathname);
    if (path === '/reload') {
      res.writeHead(200, { 'content-type': 'text/event-stream', 'cache-control': 'no-cache', connection: 'keep-alive' });
      res.write('retry: 1000\n\n');
      clients.add(res);
      req.on('close', () => clients.delete(res));
      return;
    }
    if (path === '/' || path === '/index.html') return void send(res, indexHtml);
    if (path.startsWith('/public/')) {
      const name = basename(path); // flat filename only
      const dir = name === 'embedded-react.js' || name === 'embedded-react.wasm' ? simDir : outDir;
      return void send(res, resolve(dir, name));
    }
    res.writeHead(404).end('not found');
  });

  await new Promise((r) => server.listen(port, r));
  console.log(`embedded-react WASM sim → http://localhost:${port}/`);
  console.log(`watching ${label} — edit & save to hot-reload. Ctrl-C to quit.`);

  const shutdown = async () => {
    try {
      await ctx.dispose();
    } catch {}
    server.close();
    process.exit(0);
  };
  process.on('SIGINT', shutdown);
  process.on('SIGTERM', shutdown);
  return { ctx, server };
}
