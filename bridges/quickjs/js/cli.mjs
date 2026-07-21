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

import {
  copyFileSync,
  existsSync,
  mkdirSync,
  readFileSync,
  writeFileSync,
} from 'node:fs';
import {fileURLToPath} from 'node:url';
import {dirname, relative, resolve} from 'node:path';
import {tmpdir} from 'node:os';
import {buildApp, runDevServer} from './sim-server.mjs';
import {
  packVendor,
  packApp,
  emitBootContainer,
} from './hotreload/pack-split.mjs';
import {runDeviceDevServer} from './hotreload/dev-device.mjs';
import {detectDevicePorts, preferCalloutPath} from './hotreload/uploader.mjs';

const PKG_ROOT = dirname(fileURLToPath(import.meta.url));

// QuickJS release the bytecode container targets — MUST match bridges/quickjs/CMakeLists.txt and
// ER_QUICKJS_TAG in er_runtime.c (the device loader rejects a mismatch). The wasm that compiles the
// bytecode embeds this same QuickJS.
const QJS_TAG = 'v0.15.0';

function usage() {
  console.log(`embedded-react — React Native for embedded MCUs

Usage:
  embedded-react dev [entry] [--port <n>]      Run the WASM simulator with hot reload
  embedded-react dev [entry] --device [port]   Hot-reload on a real board over USB (Flow A): re-pack
                                                 the app and stream it on every save. Omit <port> (or
                                                 pass 'auto') to auto-detect the ESP32 USB-Serial-JTAG.
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
    console.error(
      'The prebuilt simulator (sim/embedded-react.wasm) is missing from this install.',
    );
    console.error(
      'A published embedded-react package ships it; building from source needs the Emscripten SDK.',
    );
    process.exit(1);
  }
  return simDir;
}

const libSrc = () => resolve(PKG_ROOT, 'src/embedded-react/index.js');
const nodePaths = cwd => [
  resolve(PKG_ROOT, 'node_modules'),
  resolve(cwd, 'node_modules'),
];

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
  for (const rel of [
    'index.jsx',
    'src/index.jsx',
    'index.tsx',
    'src/index.tsx',
    'App.jsx',
    'src/App.jsx',
  ]) {
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
  console.error(
    'No app entry found. Pass one explicitly:  embedded-react dev <entry.jsx>',
  );
  console.error(
    '(looked for ./index.jsx, ./src/index.jsx, … and package.json "main")',
  );
  process.exit(1);
}

function optValue(args, flag) {
  const idx = args.indexOf(flag);
  if (idx < 0) return null;
  const value = args[idx + 1];
  if (!value || value.startsWith('--')) {
    console.error(`Missing value for ${flag}\n`);
    usage();
    process.exit(1);
  }
  return {value, idx, valueIdx: idx + 1};
}

/** Like optValue, but the value is OPTIONAL: `--flag` alone → {value: null}; `--flag x` → {value: 'x'}. */
function optFlagMaybeValue(args, flag) {
  const idx = args.indexOf(flag);
  if (idx < 0) return null;
  const next = args[idx + 1];
  const hasValue = next != null && !next.startsWith('--');
  return {
    value: hasValue ? next : null,
    idx,
    valueIdx: hasValue ? idx + 1 : -1,
  };
}

/** A token that names a serial port rather than an app entry file (so `--device App.jsx` isn't a port). */
const looksLikeDevicePath = s =>
  /^(\/dev\/|com\d|\\\\\.\\)/i.test(s) ||
  /(tty|usbmodem|usbserial|cu\.)/i.test(s);

/**
 * Resolve the `--device` port. An explicit path is the universal, board-agnostic mechanism (any serial
 * port, any board). With no path we auto-detect — but only the ESP32-S3/C3 built-in USB-Serial-JTAG has a
 * fixed USB id to match on, so auto-detect is ESP32-only (at least for now); other boards fall through to the
 * explicit path. Prints an actionable message and exits when the serialport is missing, nothing matches, or
 * it's ambiguous.
 */
async function resolveDevicePortOrExit(explicit) {
  if (explicit && explicit !== 'auto') return preferCalloutPath(explicit);

  const {matches, all, serialportMissing} = await detectDevicePorts();
  if (serialportMissing) {
    console.error(
      "✗ on-device hot reload needs the 'serialport' package.\n" +
        '  Install it in your project:  npm i serialport',
    );
    process.exit(1);
  }
  if (matches.length === 1) {
    const p = matches[0];
    const path = preferCalloutPath(p.path);
    console.log(
      `✓ auto-detected ESP32 USB-Serial-JTAG at ${path}` +
        (p.serialNumber ? `  (SN ${p.serialNumber})` : ''),
    );
    return path;
  }
  if (matches.length === 0) {
    console.error("✗ couldn't auto-detect a board to hot-reload.");
    console.error(
      '  Auto-detect only covers the ESP32-S3/C3 built-in USB-Serial-JTAG (USB 303a:1001) — the one',
    );
    console.error(
      '  board with a fixed USB id. For any other board (STM32, RP2040, …) pass the port explicitly:',
    );
    console.error('      embedded-react dev --device <port>');
    if (all.length) {
      console.error('\n  Serial ports seen (pick yours):');
      for (const p of all)
        console.error(
          `   ${p.path}` +
            (p.vendorId && p.productId
              ? `  [${p.vendorId}:${p.productId}]`
              : '') +
            (p.manufacturer ? `  ${p.manufacturer}` : ''),
        );
    } else {
      console.error(
        '\n  (no serial ports detected at all — is anything connected? is the cable data-capable?)',
      );
    }
    console.error(
      '\n  On an ESP32, if auto-detect should have worked: use its NATIVE USB port (not the flashing UART),',
    );
    console.error(
      '  and make sure the firmware has on-device hot reload enabled.',
    );
    process.exit(1);
  }
  console.error(
    `✗ found ${matches.length} ESP32 USB-Serial-JTAG ports; pass one explicitly with --device <port>:`,
  );
  for (const p of matches)
    console.error(
      `   ${preferCalloutPath(p.path)}${p.serialNumber ? `  (SN ${p.serialNumber})` : ''}`,
    );
  process.exit(1);
}

async function dev(args) {
  const cwd = process.cwd();
  const deviceOpt = optFlagMaybeValue(args, '--device');

  // --device [port] → stream the app to a real board over USB on every save (on-device hot reload).
  // With no port (or `auto`), the ESP32 USB-Serial-JTAG is auto-detected. Otherwise → the WASM simulator.
  if (deviceOpt) {
    // The token-after --device is normally the port. But `--device App.jsx` means "auto-detect + this
    // entry": if the value names an existing file (and isn't a device path), treat it as the entry.
    let deviceValue = deviceOpt.value;
    let entryFromDeviceSlot = null;
    if (
      deviceValue &&
      deviceValue !== 'auto' &&
      !looksLikeDevicePath(deviceValue) &&
      existsSync(resolve(cwd, deviceValue))
    ) {
      entryFromDeviceSlot = deviceValue;
      deviceValue = null; // fall through to auto-detect
    }
    const explicit =
      entryFromDeviceSlot ??
      args.find(
        (a, i) =>
          !a.startsWith('--') &&
          i !== deviceOpt.idx &&
          i !== deviceOpt.valueIdx,
      );

    const simDir = simDirOrExit();
    const entry = resolveEntry(cwd, explicit);
    const device = await resolveDevicePortOrExit(deviceValue);

    await runDeviceDevServer({
      entry,
      projectRoot: cwd,
      libSrc: libSrc(),
      nodePaths: nodePaths(cwd),
      simDir,
      device,
      label: entry,
    });
    return;
  }

  const portOpt = optValue(args, '--port');
  const consumed = new Set(portOpt ? [portOpt.idx, portOpt.valueIdx] : []);
  const explicit = args.find((a, i) => !a.startsWith('--') && !consumed.has(i));

  const simDir = simDirOrExit();
  const entry = resolveEntry(cwd, explicit);

  const port = portOpt ? parseInt(portOpt.value, 10) : 3333;
  const outDir = resolve(tmpdir(), 'embedded-react-sim');
  mkdirSync(outDir, {recursive: true});

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
  const explicit = args.find(
    (a, i) => !a.startsWith('--') && (outIdx < 0 || i !== outIdx + 1),
  );

  const simDir = simDirOrExit();
  const entry = resolveEntry(cwd, explicit);
  const pub = resolve(outDir, 'public');
  mkdirSync(pub, {recursive: true});

  // Bundle the app + bake assets into public/, then drop the prebuilt module + host page alongside.
  await buildApp({
    entry,
    projectRoot: cwd,
    libSrc: libSrc(),
    nodePaths: nodePaths(cwd),
    outDir: pub,
  });
  for (const f of ['embedded-react.js', 'embedded-react.wasm'])
    copyFileSync(resolve(simDir, f), resolve(pub, f));
  copyFileSync(resolve(simDir, 'index.html'), resolve(outDir, 'index.html'));

  console.log(
    `✓ exported a static playground → ${relative(cwd, outDir) || '.'}/`,
  );
  console.log(
    `  serve it over http (e.g. \`npx serve ${relative(cwd, outDir) || '.'}\`) or deploy the folder to any static host.`,
  );
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
  console.error(
    'No App component found. Pass one explicitly:  embedded-react build --aot <App.jsx>',
  );
  process.exit(1);
}

/** Flow B (--aot): compile the App component to C (app.gen.{c,h}) + bake assets, to compile into firmware. */
async function buildAot(cwd, explicit, outDir) {
  const {compileSource, bakeSvgArtifacts} = await import('./aot/compile.mjs');
  const {bakeAssets} = await import('./assets/index.mjs');
  const appPath = resolveAppComponent(cwd, explicit);
  const appDir = dirname(appPath);
  const src = readFileSync(appPath, 'utf8');
  let result;
  try {
    // Bake <Svg source> .svg imports → vector artifacts (incl. gradients), then compile with them in hand.
    const svgArtifacts = await bakeSvgArtifacts(src, appDir);
    result = compileSource(src, 'app', {filename: appPath, svgArtifacts});
  } catch (e) {
    console.error(e && e.aotLoc ? e.message : e?.message || String(e));
    process.exit(1);
  }
  writeFileSync(resolve(outDir, 'app.gen.c'), result.c);
  writeFileSync(resolve(outDir, 'app.gen.h'), result.h);

  const imageJobs = result.images.map(im => ({
    name: im.name,
    path: resolve(appDir, im.importPath),
  }));
  for (const j of imageJobs) {
    if (!existsSync(j.path)) {
      console.error(`<Image> asset "${j.name}" not found at ${j.path}`);
      process.exit(1);
    }
  }
  const baked = bakeAssets({images: imageJobs, fonts: [], outDir});
  console.log(
    `✓ Flow B (AOT) → ${relative(cwd, outDir) || '.'}/app.gen.c (+ app.gen.h, assets.generated.c — ${baked.images} image(s))`,
  );
  console.log(
    '  No QuickJS on the device: compile these into your firmware against the engine (er_scene.h).',
  );
}

/**
 * Flow A (default): bundle → QuickJS bytecode (via the prebuilt wasm) + baked assets → app.erpkg.
 *
 * The artifact is the vendor/app SPLIT: a vendor section (react + reconciler + the embedded-react lib,
 * ~94% of the bytecode) plus an app section. The device runs the vendor first, then the app — and on a
 * hot reload it keeps the resident vendor and swaps just the ~6% app section. The same firmware boots
 * this for production and hot-reloads it. The loader still accepts a plain monolithic container, so older
 * artifacts keep booting unchanged.
 */
async function buildContainer(cwd, explicit, outDir) {
  const simDir = simDirOrExit();
  const entry = resolveEntry(cwd, explicit);
  const lib = libSrc();
  const paths = nodePaths(cwd);

  // Release artifact: strip the embedded source text + debug tables from the bytecode (~8x smaller
  // erpkg; stack traces lose line numbers). The dev loop (`embedded-react dev`) keeps them.
  const vendor = await packVendor({libSrc: lib, nodePaths: paths, simDir, strip: true});
  const app = await packApp({
    entry,
    projectRoot: cwd,
    libSrc: lib,
    nodePaths: paths,
    simDir,
    strip: true,
  });
  const container = await emitBootContainer({
    vendorBytecode: vendor.bytecode,
    appBytecode: app.bytecode,
    assetPack: app.assetPack,
  });

  const outPath = resolve(outDir, 'app.erpkg');
  writeFileSync(outPath, container);
  const kb = n => `${(n / 1024).toFixed(1)} KB`;
  console.log(
    `✓ Flow A → ${relative(cwd, outPath) || 'app.erpkg'} (${kb(container.length)}; qjs ${QJS_TAG}, ` +
      `vendor ${kb(vendor.bytecodeLen)} + app ${kb(app.bytecodeLen)}` +
      (app.assetsLen ? `, assets ${kb(app.assetsLen)}` : '') +
      ')',
  );
  console.log(
    "  Upload app.erpkg to your device's config region (er_runtime_load_container), or run it on the desktop host.",
  );
  console.log(
    '  Tip: `embedded-react dev --device <port>` hot-reloads this app live over USB on every save.',
  );
}

/** `embedded-react build [--aot] [entry] [--out dir]` — produce the device artifact. */
async function build(args) {
  const cwd = process.cwd();
  const aot = args.includes('--aot');
  const outIdx = args.indexOf('--out');
  const outDir = resolve(cwd, outIdx >= 0 ? args[outIdx + 1] : 'dist');
  const explicit = args.find(
    (a, i) => !a.startsWith('--') && (outIdx < 0 || i !== outIdx + 1),
  );
  mkdirSync(outDir, {recursive: true});
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
