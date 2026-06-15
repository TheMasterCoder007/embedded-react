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

// bundle-app.mjs — bundle a demo's JSX into a Flow A app bundle for the WASM simulator.
//
//   node tools/web-sim/bundle-app.mjs [demo]      → tools/web-sim/public/app.js   (default: music-player)
//
// One-shot esbuild (the same config tools/simulator's sim.mjs uses): bundles demos/<demo>/index.jsx with
// React + the reconciler + the embedded-react library into a single IIFE the host page fetches and hands to
// er_web_load_source (Flow A). W2 targets asset-free demos (the built-in font renders text); imported
// images/fonts + an asset pack + esbuild --watch hot reload land in W3.

import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';
import { existsSync, mkdirSync } from 'node:fs';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = resolve(HERE, '../..');
const JS = resolve(REPO, 'bridges/quickjs/js'); // where esbuild + react + the library live

// esbuild + react + the reconciler are installed in the JS package, not here — resolve from there.
const require = createRequire(resolve(JS, 'package.json'));
const { build } = require('esbuild');
const demo = process.argv[2] || process.env.DEMO || 'music-player';
const entry = resolve(REPO, 'demos', demo, 'index.jsx');
const outfile = resolve(HERE, 'public', 'app.js');

if (!existsSync(entry)) {
  console.error(`Demo "${demo}" not found (expected ${entry}).`);
  process.exit(1);
}
mkdirSync(resolve(HERE, 'public'), { recursive: true });

try {
  await build({
    entryPoints: [entry],
    bundle: true,
    format: 'iife',
    outfile,
    platform: 'neutral',
    target: 'es2020',
    jsx: 'automatic',
    alias: { 'embedded-react': resolve(JS, 'src/embedded-react/index.js') },
    nodePaths: [resolve(JS, 'node_modules')],
    define: { 'process.env.NODE_ENV': '"production"' },
    legalComments: 'none',
    logLevel: 'info',
  });
  console.log(`✓ bundled demos/${demo} → tools/web-sim/public/app.js`);
} catch (e) {
  console.error(`bundle failed: ${e.message}`);
  process.exit(1);
}
