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

import { copyFileSync, existsSync, mkdirSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, relative, resolve } from 'node:path';
import { tmpdir } from 'node:os';
import { buildApp, runDevServer } from './sim-server.mjs';

const PKG_ROOT = dirname(fileURLToPath(import.meta.url));

function usage() {
  console.log(`embedded-react — React Native for embedded MCUs

Usage:
  embedded-react dev [entry] [--port <n>]      Run the WASM simulator with hot reload
  embedded-react export [entry] [--out <dir>]  Build a self-contained static playground (no server)

  entry   App entry file. Defaults to ./index.jsx, ./src/index.jsx, or package.json "main".
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

const [cmd, ...rest] = process.argv.slice(2);
if (cmd === 'dev') {
  await dev(rest);
} else if (cmd === 'export') {
  await exportApp(rest);
} else if (!cmd || cmd === '--help' || cmd === '-h' || cmd === 'help') {
  usage();
} else {
  console.error(`Unknown command: ${cmd}\n`);
  usage();
  process.exit(1);
}
