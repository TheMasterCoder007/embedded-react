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

// dev.mjs — the repo's WASM-simulator dev loop over the bundled demos (demos/<demo>).
//
//   node tools/web-sim/dev.mjs [demo] [--port 3333]
//
// A thin wrapper over the shared dev server (packages/embedded-react/sim-server.mjs) — the same core the shipped
// `npx embedded-react dev` CLI uses — pointed at a repo demo + the locally built module in public/. Build the
// module once first: `node tools/web-sim/build.mjs`. See tools/web-sim/README.md.

import {existsSync} from 'node:fs';
import {fileURLToPath, pathToFileURL} from 'node:url';
import {dirname, resolve} from 'node:path';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = resolve(HERE, '../..');
const PACKAGE_DIR = resolve(REPO, 'packages/embedded-react');
const PUBLIC = resolve(HERE, 'public');
const DEFAULT_PORT = 3333;

const args = process.argv.slice(2);
const portIndex = args.indexOf('--port');
const port = portIndex >= 0 ? parseInt(args[portIndex + 1], 10) : DEFAULT_PORT;
const demo =
  args.find(
    (arg, argIndex) =>
      !arg.startsWith('--') && (portIndex < 0 || argIndex !== portIndex + 1),
  ) ||
  process.env.DEMO ||
  'watch-face';

const demoDir = resolve(REPO, 'demos', demo);
const {demoFile} = await import(
  pathToFileURL(resolve(PACKAGE_DIR, 'aot/demo-file.mts')).href
);
const entry = demoFile(demoDir, 'index');
if (!existsSync(entry)) {
  console.error(`Demo "${demo}" not found (expected ${entry}).`);
  process.exit(1);
}
if (!existsSync(resolve(PUBLIC, 'embedded-react.wasm'))) {
  console.error('No built module — run `node tools/web-sim/build.mjs` first.');
  process.exit(1);
}

const {runDevServer} = await import(
  pathToFileURL(resolve(PACKAGE_DIR, 'sim-server.mjs')).href
);

await runDevServer({
  entry,
  projectRoot: demoDir,
  libSrc: resolve(PACKAGE_DIR, 'src/embedded-react/index.js'),
  nodePaths: [resolve(PACKAGE_DIR, 'node_modules')],
  indexHtml: resolve(HERE, 'index.html'),
  simDir: PUBLIC, // embedded-react.{js,wasm}
  outDir: PUBLIC, // app.js + assets.pack
  port,
  label: `demos/${demo}`,
});
