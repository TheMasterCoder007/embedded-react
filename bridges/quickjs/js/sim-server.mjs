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

// sim-server.mjs — the shared WASM-simulator bundler + dev server.
//
// esbuild-bundles a JSX app → app.js, bakes its imported images/fonts → assets.pack, and either (a) serves
// the prebuilt embedded-react.{js,wasm} host page with live hot reload (runDevServer), or (b) produces those
// files once for a static export (buildApp). useState survives a hot reload via the Babel persist transform.
//
// Used by the consumer CLI (cli.mjs → `npx embedded-react dev` / `export`) and the repo dev loop
// (tools/web-sim/dev.mjs). The only differences are paths, passed in here.

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
 * Build the esbuild config + asset baker for one app bundle. Owns its own asset-discovery state, so it is
 * safe to use for either a one-shot build or a watching context.
 *
 * @param {object}        o
 * @param {string}        o.entry, o.projectRoot, o.libSrc, o.outDir
 * @param {string[]}      o.nodePaths
 * @param {boolean}       [o.persist]    Apply the useState→persist transform (dev hot reload). Default true.
 * @param {() => void}    [o.onRebuilt]  Called after each successful build + asset bake (e.g. broadcast reload).
 * @returns {{ options: object, bundlePath: string, packPath: string }}
 */
function createBundle({ entry, projectRoot, libSrc, nodePaths, outDir, persist = true, onRebuilt }) {
  const bundlePath = resolve(outDir, 'app.js');
  const packPath = resolve(outDir, 'assets.pack');
  const projNorm = projectRoot.replace(/\\/g, '/');
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

  const options = {
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
          b.onLoad({ filter: /\.(jsx?|tsx?)$/ }, (a) => {
            if (!persist || !a.path.replace(/\\/g, '/').startsWith(projNorm)) return undefined;
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
              console.error(`✗ build failed (${r.errors.length} error(s))`);
              return;
            }
            await bakePack();
            if (onRebuilt) onRebuilt();
          });
        },
      },
    ],
  };

  return { options, bundlePath, packPath };
}

/**
 * One-shot bundle + asset bake into outDir (app.js [+ assets.pack]). For the static export.
 * Persist transform is off (a static page has no hot reload to preserve state across).
 */
export async function buildApp({ entry, projectRoot, libSrc, nodePaths, outDir }) {
  const { options } = createBundle({ entry, projectRoot, libSrc, nodePaths, outDir, persist: false });
  await esbuild.build(options);
}

/**
 * Run the simulator dev server: watch + bundle + bake + serve + hot reload.
 *
 * @param {object}   o
 * @param {string}   o.entry, o.projectRoot, o.libSrc, o.indexHtml, o.simDir, o.outDir, o.label
 * @param {string[]} o.nodePaths
 * @param {number}   o.port
 */
export async function runDevServer({ entry, projectRoot, libSrc, nodePaths, indexHtml, simDir, outDir, port, label }) {
  const clients = new Set();
  const broadcastReload = () => {
    for (const res of clients) res.write('data: reload\n\n');
  };

  const { options } = createBundle({ entry, projectRoot, libSrc, nodePaths, outDir, persist: true, onRebuilt: () => {
    console.log('↻ rebuilt → reloading');
    broadcastReload();
  } });

  const ctx = await esbuild.context(options);
  await ctx.rebuild();
  await ctx.watch();

  // Serve the host page; embedded-react.{js,wasm} from simDir, the freshly bundled app.js/assets.pack from
  // outDir. The page references ./public/<name>. window.__erHot is injected so the page connects to /reload.
  const send = async (res, file) => {
    try {
      res.writeHead(200, { 'content-type': MIME[extname(file)] || 'application/octet-stream', 'cache-control': 'no-store' }).end(await readFile(file));
    } catch {
      res.writeHead(404).end('not found');
    }
  };
  const server = createServer(async (req, res) => {
    const path = decodeURIComponent(new URL(req.url, 'http://localhost').pathname);
    if (path === '/reload') {
      res.writeHead(200, { 'content-type': 'text/event-stream', 'cache-control': 'no-cache', connection: 'keep-alive' });
      res.write('retry: 1000\n\n');
      clients.add(res);
      req.on('close', () => clients.delete(res));
      return;
    }
    if (path === '/' || path === '/index.html') {
      try {
        const html = (await readFile(indexHtml, 'utf8')).replace('</head>', '<script>window.__erHot=true</script></head>');
        res.writeHead(200, { 'content-type': MIME['.html'], 'cache-control': 'no-store' }).end(html);
      } catch {
        res.writeHead(404).end('not found');
      }
      return;
    }
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
