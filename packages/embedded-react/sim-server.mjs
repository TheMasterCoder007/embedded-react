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

import {createServer} from 'node:http';
import {readFile} from 'node:fs/promises';
import {existsSync, readFileSync, statSync} from 'node:fs';
import {createRequire} from 'node:module';
import {pathToFileURL} from 'node:url';
import {basename, dirname, extname, relative, resolve} from 'node:path';

const HERE = dirname(
  new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'),
);
const require = createRequire(import.meta.url);
const esbuild = require('esbuild');
const {bakeAssetPack} = await import(
  pathToFileURL(resolve(HERE, 'assets/index.mjs')).href
);
const {resolveFontJobs} = await import(
  pathToFileURL(resolve(HERE, 'assets/font-config.mjs')).href
);
const {analyzeFontSizes, fontSizeSignature, warnFontSizes} = await import(
  pathToFileURL(resolve(HERE, 'assets/font-sizes.mjs')).href
);
const {textSignature, warnMissingGlyphs} = await import(
  pathToFileURL(resolve(HERE, 'assets/glyph-coverage.mjs')).href
);
const {registerSvgVectorLoader} = await import(
  pathToFileURL(resolve(HERE, 'assets/svg-loader.mjs')).href
);
const {transformPersist, shouldPersist} = await import(
  pathToFileURL(resolve(HERE, 'persist-transform.mjs')).href
);
const {loadAppFile} = await import('./aot/app-file-loader.mts');

const REACT_PIN = Object.fromEntries(
  ['react', 'react-reconciler', 'scheduler']
    .map(packageName => [
      packageName,
      resolve(HERE, 'node_modules', packageName),
    ])
    .filter(([, pinPath]) => existsSync(pinPath)),
);

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.wasm': 'application/wasm',
  '.pack': 'application/octet-stream',
};
const assetName = filePath => basename(filePath).replace(/\.[^.]+$/, '');
const mtime = filePath => {
  try {
    return statSync(filePath).mtimeMs;
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
function createBundle({
  entry,
  projectRoot,
  libSrc,
  nodePaths,
  outDir,
  persist = true,
  onRebuilt,
}) {
  const bundlePath = resolve(outDir, 'app.js');
  const packPath = resolve(outDir, 'assets.pack');
  const projectRootNormalized = projectRoot.replace(/\\/g, '/');
  const images = new Map();
  const fonts = new Map();
  let lastAssetSignature = null;

  async function bakePack() {
    const bundleSource = readFileSync(bundlePath, 'utf8');
    const usedSizes = analyzeFontSizes(bundleSource);

    let assetsConfig = {};
    const assetsConfigPath = resolve(projectRoot, 'assets.config.js');
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

    // The app's text is an asset input too: new characters and new font sizes must both be
    // re-checked against the bake even when no font or image file changed.
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
    if (!imageJobs.length && !fontJobs.length) {
      // Built-in font only → no pack, but its glyph and size coverage still bind.
      warnMissingGlyphs({source: bundleSource});
      warnFontSizes({used: usedSizes});
      return;
    }

    try {
      const summary = bakeAssetPack({
        images: imageJobs,
        fonts: fontJobs,
        outPath: packPath,
        source: bundleSource,
        usedSizes,
      });
      console.log(
        `  assets → ${summary.images} image(s), ${summary.fonts} font size(s), ${summary.bytes} B`,
      );
    } catch (error) {
      console.error(`  asset bake failed: ${error.message}`);
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
    alias: {'embedded-react': libSrc, ...REACT_PIN},
    nodePaths,
    define: {'process.env.NODE_ENV': '"production"'},
    legalComments: 'none',
    logLevel: 'silent',
    plugins: [
      {
        name: 'embedded-react-websim',
        setup(pluginBuild) {
          pluginBuild.onStart(() => {
            images.clear();
            fonts.clear();
          });
          pluginBuild.onLoad({filter: /\.(jsx?|tsx?)$/}, async loadArgs => {
            // Only the app's own source — never dependencies (see shouldPersist: excluding node_modules is what
            // stops the library's usePersistentState being rewritten to call itself). Native modules get their
            // signatures, and with persist on, useState persists.
            if (!shouldPersist(loadArgs.path, projectRootNormalized))
              return undefined;
            try {
              return await loadAppFile(
                loadArgs.path,
                persist
                  ? source =>
                      transformPersist(
                        source,
                        relative(projectRoot, loadArgs.path).replace(
                          /\\/g,
                          '/',
                        ),
                      )
                  : undefined,
              );
            } catch (error) {
              return {errors: [{text: `persist transform: ${error.message}`}]};
            }
          });
          pluginBuild.onLoad(
            {filter: /\.(png|jpe?g|webp|gif|bmp)$/i},
            loadArgs => {
              images.set(assetName(loadArgs.path), loadArgs.path);
              return {
                contents: `module.exports = ${JSON.stringify(assetName(loadArgs.path))};`,
                loader: 'js',
              };
            },
          );
          pluginBuild.onLoad({filter: /\.(ttf|otf)$/i}, loadArgs => {
            fonts.set(assetName(loadArgs.path), loadArgs.path);
            return {
              contents: `module.exports = ${JSON.stringify(assetName(loadArgs.path))};`,
              loader: 'js',
            };
          });
          registerSvgVectorLoader(pluginBuild, (name, svgPath) =>
            images.set(name, svgPath),
          ); // raster-fallback SVGs join the image pack
          pluginBuild.onEnd(async result => {
            if (result.errors.length) {
              console.error(
                `✗ build failed (${result.errors.length} error(s))`,
              );
              return;
            }
            await bakePack();
            if (onRebuilt) onRebuilt();
          });
        },
      },
    ],
  };

  return {options, bundlePath, packPath};
}

/**
 * One-shot bundle + asset bake into outDir (app.js [+ assets.pack]). For the static export.
 * Persist transform is off (a static page has no hot reload to preserve state across).
 */
export async function buildApp({
  entry,
  projectRoot,
  libSrc,
  nodePaths,
  outDir,
}) {
  const {options} = createBundle({
    entry,
    projectRoot,
    libSrc,
    nodePaths,
    outDir,
    persist: false,
  });
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
export async function runDevServer({
  entry,
  projectRoot,
  libSrc,
  nodePaths,
  indexHtml,
  simDir,
  outDir,
  port,
  label,
}) {
  const clients = new Set();
  const broadcastReload = () => {
    for (const response of clients) response.write('data: reload\n\n');
  };

  const {options} = createBundle({
    entry,
    projectRoot,
    libSrc,
    nodePaths,
    outDir,
    persist: true,
    onRebuilt: () => {
      console.log('↻ rebuilt → reloading');
      broadcastReload();
    },
  });

  const buildContext = await esbuild.context(options);
  await buildContext.rebuild();
  await buildContext.watch();

  // Serve the host page; embedded-react.{js,wasm} from simDir, the freshly bundled app.js/assets.pack from
  // outDir. The page references ./public/<name>. window.__erHot is injected so the page connects to /reload.
  const send = async (response, file) => {
    try {
      const body = await readFile(file);
      response
        .writeHead(200, {
          'content-type': MIME[extname(file)] || 'application/octet-stream',
          'cache-control': 'no-store',
        })
        .end(body);
    } catch {
      response.writeHead(404).end('not found');
    }
  };
  const server = createServer(async (request, response) => {
    const path = decodeURIComponent(
      new URL(request.url, 'http://localhost').pathname,
    );
    if (path === '/reload') {
      response.writeHead(200, {
        'content-type': 'text/event-stream',
        'cache-control': 'no-cache',
        connection: 'keep-alive',
      });
      response.write('retry: 1000\n\n');
      clients.add(response);
      request.on('close', () => clients.delete(response));
      return;
    }
    if (path === '/' || path === '/index.html') {
      try {
        const html = (await readFile(indexHtml, 'utf8')).replace(
          '</head>',
          '<script>window.__erHot=true</script></head>',
        );
        response
          .writeHead(200, {
            'content-type': MIME['.html'],
            'cache-control': 'no-store',
          })
          .end(html);
      } catch {
        response.writeHead(404).end('not found');
      }
      return;
    }
    if (path.startsWith('/public/')) {
      const name = basename(path); // flat filename only
      const dir =
        name === 'embedded-react.js' || name === 'embedded-react.wasm'
          ? simDir
          : outDir;
      return void send(response, resolve(dir, name));
    }
    response.writeHead(404).end('not found');
  });

  await new Promise(onListening => server.listen(port, onListening));
  console.log(`embedded-react WASM sim → http://localhost:${port}/`);
  console.log(`watching ${label} — edit & save to hot-reload. Ctrl-C to quit.`);

  const shutdown = async () => {
    try {
      await buildContext.dispose();
    } catch {}
    server.close();
    process.exit(0);
  };
  process.on('SIGINT', shutdown);
  process.on('SIGTERM', shutdown);
  return {ctx: buildContext, server};
}
