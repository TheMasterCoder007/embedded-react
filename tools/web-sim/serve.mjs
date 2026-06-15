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

// serve.mjs — minimal static server for the WASM simulator host page.
//
//   node tools/web-sim/serve.mjs [--port 3333]
//
// Serves tools/web-sim/ with the correct application/wasm MIME so the browser streams + compiles the
// module. W3 grows this into the dev server (esbuild watch + websocket hot-reload). For now it is just a
// file server so `index.html` + `public/embedded-react.{js,wasm}` load over http (file:// blocks fetch).

import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { dirname, extname, normalize, resolve } from 'node:path';

const HERE = dirname(fileURLToPath(import.meta.url));
const portIdx = process.argv.indexOf('--port');
// 3333 by default — a memorable repdigit port that's clear of the common dev-server ports (Vite 5173,
// CRA/Next 3000, Astro 4321, webpack 8080) and not browser-blocked. Override with --port.
const PORT = portIdx >= 0 ? parseInt(process.argv[portIdx + 1], 10) : 3333;

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.wasm': 'application/wasm',
  '.json': 'application/json',
  '.css': 'text/css; charset=utf-8',
};

const server = createServer(async (req, res) => {
  try {
    const urlPath = decodeURIComponent(new URL(req.url, 'http://localhost').pathname);
    const rel = urlPath === '/' ? 'index.html' : urlPath.replace(/^\/+/, '');
    // Confine to HERE — reject any path that escapes the served directory.
    const file = normalize(resolve(HERE, rel));
    if (!file.startsWith(HERE)) {
      res.writeHead(403).end('forbidden');
      return;
    }
    const body = await readFile(file);
    res.writeHead(200, { 'content-type': MIME[extname(file)] || 'application/octet-stream' }).end(body);
  } catch {
    res.writeHead(404).end('not found');
  }
});

server.listen(PORT, () => {
  console.log(`embedded-react WASM sim → http://localhost:${PORT}/`);
  console.log('(build first with: node tools/web-sim/build.mjs)');
});
