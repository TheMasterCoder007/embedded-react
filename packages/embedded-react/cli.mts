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
  statSync,
  writeFileSync,
} from 'node:fs';
import {fileURLToPath, pathToFileURL} from 'node:url';
import {basename, dirname, relative, resolve} from 'node:path';
import {tmpdir} from 'node:os';
import {spawnSync} from 'node:child_process';
import {buildApp, runDevServer} from './sim-server.mts';
import {
  packVendor,
  packApp,
  emitBootContainer,
} from './hotreload/pack-split.mts';
import {runDeviceDevServer} from './hotreload/dev-device.mts';
import {detectDevicePorts, preferCalloutPath} from './hotreload/uploader.mts';
import {boardByName, flashSlot, packSlot} from './flash.mts';
import {
  ELF_MACHINE_ARM,
  ELF_MACHINE_RISCV,
  ELF_MACHINE_XTENSA,
  callChain,
  faultReport,
  mergeElfs,
  parseFaultRecord,
  readElf,
} from './fault-decode.mts';
import type {AssetsConfig} from './assets/font-config.mts';
import type {Screen} from './aot/compiler/options.mts';

/** Where a flag sits in the arguments, and its value. */
interface FlagValue<Value> {
  value: Value;
  index: number;
  valueIndex: number;
}

const PACKAGE_ROOT = dirname(fileURLToPath(import.meta.url));

// QuickJS release the bytecode container targets — MUST match bridges/quickjs/CMakeLists.txt and
// ER_QUICKJS_TAG in er_runtime.c (the device loader rejects a mismatch). The wasm that compiles the
// bytecode embeds this same QuickJS.
const QUICKJS_TAG = 'v0.15.0';

const DEFAULT_PORT = 3333;
const BYTES_PER_KILOBYTE = 1024;

function usage() {
  console.log(`embedded-react — React Native for embedded MCUs

Usage:
  embedded-react dev [entry] [--port <n>]      Run the WASM simulator with hot reload
  embedded-react dev [entry] --device [port]   Hot-reload on a real board over USB (Flow A): re-pack
                                                 the app and stream it on every save. Omit <port> (or
                                                 pass 'auto') to auto-detect the ESP32 USB-Serial-JTAG.
                                                 --aot  → Flow B: compile a package and send it instead.
  embedded-react export [entry] [--out <dir>]  Build a self-contained static playground (no server)
  embedded-react build [entry] [--out <dir>]   Build the device artifact:
                                                 default → app.erpkg (Flow A: QuickJS bytecode + assets,
                                                           upload to the device's config region)
                                                 --aot   → app.erpkg (Flow B package, for the Flow B
                                                           runtime: write it to a slot with flash)
                                                           It has a layout for every panel size:
                                                           the screen picks one at boot.
                                                 --aot --screen <WxH>,<WxH>  keep only the layouts
                                                           those panels pick, to save flash
                                                 --aot --release  → a package without its debug
                                                           table
                                                 --aot writes app.erdbg beside the package in every
                                                           build: the source lines, state names and
                                                           node lines, for host tools
                                                 --aot --bake  → app.erpkg.c (the package as a const
                                                           array, for a board with no spare partition)
  embedded-react flash [package] --board <name> [--port <p>]
                                               Write a package to the board's staging slot. The firmware
                                                 runs it at the next boot if it passes verification, and
                                                 keeps the last good package until then.
  embedded-react pack [package] --board <name> [--slot a|b] [--sequence <n>] [--out <dir>]
                                               Write a slot's image to a file, for a factory line or a
                                                 drag-and-drop UF2, without a board attached.
  embedded-react where <function> <offset> [--debug <file>] [--build <id>]
                                               Print the source line of a Flow B fault, from the function
                                                 and offset an error screen or log shows. Reads
                                                 dist/app.erdbg unless --debug names another; --build
                                                 refuses a file from another build.
  embedded-react decode-fault <record> --elf <firmware.elf> [--elf <more.elf>] [--debug <file>]
                              [--addr2line <tool>]
                                               Trace a hard fault from the record the board logs after the
                                                 reset ("fault record: ..."): the C call chain from the
                                                 firmware's ELF, and the app's source line. <record> is the
                                                 hex, or a file that holds it, such as a saved log. Reads
                                                 dist/app.erdbg when it is there, unless --debug names
                                                 another. File and line come from the toolchain's addr2line.
                                                 Give --elf once for each image of a firmware built as
                                                 several (a bootloader, a module it loads).
  embedded-react native-header <package or file> [--out <dir>]
                                               Write a C header for each native module a device package
                                                 or a .ts file declares: its records, where each argument
                                                 starts, and an entry for the firmware's table.

  board   esp32-2432s028r, esp32-s3 or rp2040-touch-lcd-1.69
  package defaults to dist/app.erpkg

  entry   dev/export/build use ./index.jsx, ./src/index.jsx, or package.json "main";
          build --aot uses ./App.jsx or ./src/App.jsx. Pass one to override.
`);
}

/** Locate the package's prebuilt simulator assets (the .wasm ships with this package). */
function simDirOrExit(): string {
  const simDir = resolve(PACKAGE_ROOT, 'sim');
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

const libSrc = () => resolve(PACKAGE_ROOT, 'src/embedded-react/index.js');
const nodePaths = (cwd: string) => [
  resolve(PACKAGE_ROOT, 'node_modules'),
  resolve(cwd, 'node_modules'),
];

/** Resolve the app entry: an explicit arg, else common conventions, else package.json "main"/"source". */
function resolveEntry(cwd: string, explicit?: string | null): string {
  if (explicit) {
    const entryPath = resolve(cwd, explicit);
    if (!existsSync(entryPath)) {
      console.error(`Entry not found: ${entryPath}`);
      process.exit(1);
    }
    return entryPath;
  }
  for (const relativePath of [
    'index.jsx',
    'src/index.jsx',
    'index.tsx',
    'src/index.tsx',
    'App.jsx',
    'src/App.jsx',
  ]) {
    const entryPath = resolve(cwd, relativePath);
    if (existsSync(entryPath)) return entryPath;
  }
  const packageJsonPath = resolve(cwd, 'package.json');
  if (existsSync(packageJsonPath)) {
    try {
      const packageJson = JSON.parse(readFileSync(packageJsonPath, 'utf8'));
      for (const entryField of [packageJson.source, packageJson.main]) {
        if (entryField) {
          const entryPath = resolve(cwd, entryField);
          if (existsSync(entryPath)) return entryPath;
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

function optValue(args: string[], flag: string): FlagValue<string> | null {
  const flagIndex = args.indexOf(flag);
  if (flagIndex < 0) return null;
  const value = args[flagIndex + 1];
  if (!value || value.startsWith('--')) {
    console.error(`Missing value for ${flag}\n`);
    usage();
    process.exit(1);
  }
  return {value, index: flagIndex, valueIndex: flagIndex + 1};
}

/** Like optValue, but the value is OPTIONAL: `--flag` alone → {value: null}; `--flag x` → {value: 'x'}. */
function optFlagMaybeValue(
  args: string[],
  flag: string,
): FlagValue<string | null> | null {
  const flagIndex = args.indexOf(flag);
  if (flagIndex < 0) return null;
  const nextArg = args[flagIndex + 1];
  const hasValue = nextArg != null && !nextArg.startsWith('--');
  return {
    value: hasValue ? nextArg : null,
    index: flagIndex,
    valueIndex: hasValue ? flagIndex + 1 : -1,
  };
}

/** A token that names a serial port rather than an app entry file (so `--device App.jsx` isn't a port). */
const looksLikeDevicePath = (token: string) =>
  /^(\/dev\/|com\d|\\\\\.\\)/i.test(token) ||
  /(tty|usbmodem|usbserial|cu\.)/i.test(token);

/**
 * Resolve the `--device` port. An explicit path is the universal, board-agnostic mechanism (any serial
 * port, any board). With no path we auto-detect — but only the ESP32-S3/C3 built-in USB-Serial-JTAG has a
 * fixed USB id to match on, so auto-detect is ESP32-only (at least for now); other boards fall through to the
 * explicit path. Prints an actionable message and exits when the serialport is missing, nothing matches, or
 * it's ambiguous.
 */
async function resolveDevicePortOrExit(
  explicit: string | null,
): Promise<string> {
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
    const port = matches[0];
    const path = preferCalloutPath(port.path);
    console.log(
      `✓ auto-detected ESP32 USB-Serial-JTAG at ${path}` +
        (port.serialNumber ? `  (SN ${port.serialNumber})` : ''),
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
      for (const port of all)
        console.error(
          `   ${port.path}` +
            (port.vendorId && port.productId
              ? `  [${port.vendorId}:${port.productId}]`
              : '') +
            (port.manufacturer ? `  ${port.manufacturer}` : ''),
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
  for (const port of matches)
    console.error(
      `   ${preferCalloutPath(port.path)}${port.serialNumber ? `  (SN ${port.serialNumber})` : ''}`,
    );
  process.exit(1);
}

async function dev(args: string[]): Promise<void> {
  const cwd = process.cwd();
  const deviceOption = optFlagMaybeValue(args, '--device');
  const aot = args.includes('--aot');
  if (aot && !deviceOption) {
    console.error(
      '--aot only applies to --device: the simulator runs the app as Flow A.\n',
    );
    usage();
    process.exit(1);
  }

  // --device [port] → stream the app to a real board over USB on every save (on-device hot reload).
  // With no port (or `auto`), the ESP32 USB-Serial-JTAG is auto-detected. Otherwise → the WASM simulator.
  if (deviceOption) {
    // The token-after --device is normally the port. But `--device App.jsx` means "auto-detect + this
    // entry": if the value names an existing file (and isn't a device path), treat it as the entry.
    let deviceValue = deviceOption.value;
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
        (arg, argIndex) =>
          !arg.startsWith('--') &&
          argIndex !== deviceOption.index &&
          argIndex !== deviceOption.valueIndex,
      );

    if (aot) {
      const appPath = resolveAppComponent(cwd, explicit);
      const device = await resolveDevicePortOrExit(deviceValue);
      await runDeviceDevServer({
        entry: appPath,
        projectRoot: cwd,
        device,
        label: appPath,
        compilePackage: async () => {
          const {result, file} = await compileAppPackage(cwd, appPath);
          const distDir = resolve(cwd, 'dist');
          mkdirSync(distDir, {recursive: true});
          writeFileSync(resolve(distDir, 'app.erdbg'), result.debugFile);
          return Buffer.from(file.bytes);
        },
      });
      return;
    }

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

  const portOption = optValue(args, '--port');
  const consumed = new Set(
    portOption ? [portOption.index, portOption.valueIndex] : [],
  );
  const explicit = args.find(
    (arg, argIndex) => !arg.startsWith('--') && !consumed.has(argIndex),
  );

  const simDir = simDirOrExit();
  const entry = resolveEntry(cwd, explicit);

  const port = portOption ? parseInt(portOption.value, 10) : DEFAULT_PORT;
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

async function exportApp(args: string[]): Promise<void> {
  const cwd = process.cwd();
  const outIndex = args.indexOf('--out');
  const outDir = resolve(
    cwd,
    outIndex >= 0 ? args[outIndex + 1] : 'sim-export',
  );
  const explicit = args.find(
    (arg, argIndex) =>
      !arg.startsWith('--') && (outIndex < 0 || argIndex !== outIndex + 1),
  );

  const simDir = simDirOrExit();
  const entry = resolveEntry(cwd, explicit);
  const publicDir = resolve(outDir, 'public');
  mkdirSync(publicDir, {recursive: true});

  // Bundle the app + bake assets into public/, then drop the prebuilt module + host page alongside.
  await buildApp({
    entry,
    projectRoot: cwd,
    libSrc: libSrc(),
    nodePaths: nodePaths(cwd),
    outDir: publicDir,
  });
  for (const fileName of ['embedded-react.js', 'embedded-react.wasm'])
    copyFileSync(resolve(simDir, fileName), resolve(publicDir, fileName));
  copyFileSync(resolve(simDir, 'index.html'), resolve(outDir, 'index.html'));

  console.log(
    `✓ exported a static playground → ${relative(cwd, outDir) || '.'}/`,
  );
  console.log(
    `  serve it over http (e.g. \`npx serve ${relative(cwd, outDir) || '.'}\`) or deploy the folder to any static host.`,
  );
}

/** Resolve the root component file for the AOT compiler (App.jsx), distinct from the registry entry. */
function resolveAppComponent(cwd: string, explicit?: string | null): string {
  if (explicit) {
    const entryPath = resolve(cwd, explicit);
    if (!existsSync(entryPath)) {
      console.error(`Entry not found: ${entryPath}`);
      process.exit(1);
    }
    return entryPath;
  }
  for (const relativePath of [
    'App.jsx',
    'src/App.jsx',
    'App.tsx',
    'src/App.tsx',
  ]) {
    const entryPath = resolve(cwd, relativePath);
    if (existsSync(entryPath)) return entryPath;
  }
  console.error(
    'No App component found. Pass one explicitly:  embedded-react build --aot <App.jsx>',
  );
  process.exit(1);
}

/**
 * Compiles the App component with what goes into the build: its package file and its image and font jobs.
 * Throws with the reason, so a watch loop can carry on past a bad save.
 */
async function compileAppPackage(
  cwd: string,
  appPath: string,
  {
    screens = null,
    release = false,
  }: {screens?: Screen[] | null; release?: boolean} = {},
) {
  const {
    compilePackage,
    bakeSvgArtifacts,
    bakeSvgTextPaths,
    packPackage,
    readNativeFiles,
  } = await import('./aot/compile.mts');
  const {bakePackageAssets} = await import('./aot/package-assets.mts');
  const appDir = dirname(appPath);
  const source = bakeSvgTextPaths(readFileSync(appPath, 'utf8'), appDir);
  // Bake <Svg source> .svg imports → vector artifacts (incl. gradients), then compile with them in hand.
  const svgArtifacts = await bakeSvgArtifacts(source, appDir);
  // A package has every layout the app's breakpoints call for, and --screen <WxH>,<WxH> keeps only those the
  // sizes pick.
  const result = compilePackage(source, basename(dirname(appPath)), {
    filename: appPath,
    svgArtifacts,
    // Native modules and their interfaces in other files and packages, read here so the compile stays pure.
    nativeFiles: readNativeFiles(source, appDir),
    screen: screens?.[0] ?? null, // null without --screen
    screens,
    release,
  });

  // Optional per-asset overrides (same file the JS flows read): assets.config.js
  //   export default { images: { 'bg': { format: 'rgb565' } }, fonts: { 'Manrope-Light': { sizes: [64, 72] } } }
  let assetsConfig: AssetsConfig = {};
  const assetsConfigPath = resolve(cwd, 'assets.config.js');
  if (existsSync(assetsConfigPath))
    assetsConfig =
      (
        await import(
          `${pathToFileURL(assetsConfigPath).href}?t=${statSync(assetsConfigPath).mtimeMs}`
        )
      ).default || {};
  const {assetPack, imageJobs, fontJobs} = bakePackageAssets(
    result,
    source,
    appDir,
    assetsConfig,
  );
  const file = packPackage(result, assetPack);
  return {result, file, imageJobs, fontJobs};
}

/**
 * Flow B (--aot): compile the App component to a Flow B package (app.erpkg) with its assets inside, for the Flow B
 * runtime to load: for release without its debug table, and baked as a const array (app.erpkg.c) to link into the
 * firmware. Its debug file (app.erdbg) goes beside it.
 */
async function buildAot(
  cwd: string,
  explicit: string | undefined,
  outDir: string,
  screens: Screen[] | null,
  {release = false, bake = false} = {},
): Promise<void> {
  const {bakePackage, resourceReport} = await import('./aot/compile.mts');
  const appPath = resolveAppComponent(cwd, explicit);
  let compiled;
  try {
    compiled = await compileAppPackage(cwd, appPath, {screens, release});
  } catch (error: any) {
    console.error(
      error && error.aotLoc ? error.message : error?.message || String(error),
    );
    process.exit(1);
  }
  const {result, file, imageJobs, fontJobs} = compiled;
  const fontSizes = fontJobs.reduce(
    (total, font) => total + font.sizes.length,
    0,
  );
  const displayDir = relative(cwd, outDir) || '.';
  const name = bake ? 'app.erpkg.c' : 'app.erpkg';
  writeFileSync(
    resolve(outDir, name),
    bake ? bakePackage(file.bytes) : file.bytes,
  );
  console.log(
    `✓ Flow B package → ${displayDir}/${name} (${release ? 'release' : 'debug'}, ${imageJobs.length} image(s), ${fontSizes} font size(s))`,
  );
  for (const font of fontJobs) {
    console.log(
      `  font ${font.family} at ${font.sizes.join(', ')} px, ${font.bpp} bpp`,
    );
  }
  writeFileSync(resolve(outDir, 'app.erdbg'), result.debugFile);
  console.log(
    `  Debug file → ${displayDir}/app.erdbg (build ${hexBuildId(result.buildId)}), for \`embedded-react where\` and a debugger`,
  );
  console.log(resourceReport(result, file).join('\n'));
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
async function buildContainer(
  cwd: string,
  explicit: string | undefined,
  outDir: string,
): Promise<void> {
  const simDir = simDirOrExit();
  const entry = resolveEntry(cwd, explicit);
  const librarySource = libSrc();
  const paths = nodePaths(cwd);

  // Release artifact: strip the embedded source text + debug tables from the bytecode (~8x smaller
  // erpkg; stack traces lose line numbers). The dev loop (`embedded-react dev`) keeps them.
  const vendor = await packVendor({
    libSrc: librarySource,
    nodePaths: paths,
    simDir,
    strip: true,
  });
  const app = await packApp({
    entry,
    projectRoot: cwd,
    libSrc: librarySource,
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
  const kb = (byteCount: number) =>
    `${(byteCount / BYTES_PER_KILOBYTE).toFixed(1)} KB`;
  console.log(
    `✓ Flow A → ${relative(cwd, outPath) || 'app.erpkg'} (${kb(container.length)}; qjs ${QUICKJS_TAG}, ` +
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

/**
 * `embedded-react build [--aot [--release] [--bake]] [entry] [--out dir] [--screen WxH[,WxH…]]` — produce the
 * device artifact.
 */
async function build(args: string[]): Promise<void> {
  const cwd = process.cwd();
  const aot = args.includes('--aot');
  const release = args.includes('--release');
  const bake = args.includes('--bake');
  for (const flag of ['--release', '--bake']) {
    if (args.includes(flag) && !aot) {
      console.error(`${flag} only applies to --aot.\n`);
      usage();
      process.exit(1);
    }
  }
  const outIndex = args.indexOf('--out');
  const outDir = resolve(cwd, outIndex >= 0 ? args[outIndex + 1] : 'dist');

  // --screen <WxH> (AOT only): the panels to keep layouts for, comma-separated. Optional: a package has every
  // layout without it. Flow A reads the screen from the host at runtime.
  const screenIndex = args.indexOf('--screen');
  let screens: Screen[] | null = null;
  if (screenIndex >= 0) {
    if (!aot) {
      console.error(
        '--screen only applies to --aot (Flow A reads the panel size from the host at runtime).\n',
      );
      usage();
      process.exit(1);
    }
    const screenList = args[screenIndex + 1];
    const sizes = (screenList ?? '')
      .split(',')
      .map(size => /^([1-9]\d*)x([1-9]\d*)$/i.exec(size));
    if (!screenList || sizes.some(size => !size)) {
      console.error(
        `--screen expects <width>x<height> (e.g. --screen 240x320), got: ${screenList ?? '(nothing)'}\n`,
      );
      usage();
      process.exit(1);
    }
    screens = sizes.map(size => ({
      width: Number(size![1]),
      height: Number(size![2]),
    }));
  }

  // Consume the flag VALUES so they aren't mistaken for the entry (which is a bare, non-`--` token).
  const consumed = new Set<number>();
  if (outIndex >= 0) consumed.add(outIndex + 1);
  if (screenIndex >= 0) consumed.add(screenIndex + 1);
  const explicit = args.find(
    (arg, argIndex) => !arg.startsWith('--') && !consumed.has(argIndex),
  );

  mkdirSync(outDir, {recursive: true});
  if (aot) await buildAot(cwd, explicit, outDir, screens, {release, bake});
  else await buildContainer(cwd, explicit, outDir);
}

/** Reads the package a `pack` or `flash` names, or dist/app.erpkg; exits saying how to build one when it's missing. */
function readPackageOrExit(cwd: string, explicit?: string): Buffer {
  const path = resolve(cwd, explicit ?? 'dist/app.erpkg');
  if (!existsSync(path)) {
    console.error(
      `No package at ${path}. Build one with: embedded-react build --aot`,
    );
    process.exit(1);
  }
  return readFileSync(path);
}

/** The bare arguments, once the values of the listed flags are set aside. */
function bareArgs(args: string[], flagsWithValues: string[]): string[] {
  const consumed = new Set<number>();
  for (const flag of flagsWithValues) {
    const flagIndex = args.indexOf(flag);
    if (flagIndex >= 0) consumed.add(flagIndex + 1);
  }
  return args.filter(
    (arg, argIndex) => !arg.startsWith('--') && !consumed.has(argIndex),
  );
}

/** `embedded-react pack [package] --board <name> [--slot a|b] [--sequence n] [--out dir]` */
function pack(args: string[]): void {
  const cwd = process.cwd();
  const boardName = optValue(args, '--board')?.value;
  const outDir = resolve(cwd, optValue(args, '--out')?.value ?? 'dist');
  const sequence = Number(optValue(args, '--sequence')?.value ?? 1);
  try {
    const board = boardByName(boardName);
    const [explicit] = bareArgs(args, [
      '--board',
      '--slot',
      '--sequence',
      '--out',
    ]);
    mkdirSync(outDir, {recursive: true});
    const packed = packSlot({
      packageBytes: readPackageOrExit(cwd, explicit),
      board,
      boardName,
      slotName: optValue(args, '--slot')?.value,
      sequence,
      outDir,
    });
    console.log(
      `✓ Slot ${packed.slot}, sequence ${packed.sequence} → ${relative(cwd, packed.file)}, for flash address 0x${packed.address.toString(16)}`,
    );
  } catch (error: any) {
    console.error(error.message);
    process.exit(1);
  }
}

/** `embedded-react flash [package] --board <name> [--port p]` */
function flash(args: string[]): void {
  const cwd = process.cwd();
  const boardName = optValue(args, '--board')?.value;
  try {
    const board = boardByName(boardName);
    const [explicit] = bareArgs(args, ['--board', '--port']);
    const written = flashSlot({
      packageBytes: readPackageOrExit(cwd, explicit),
      board,
      port: optValue(args, '--port')?.value,
      log: line => console.log(line),
    });
    console.log(
      `✓ Package written to slot ${written.slot}, sequence ${written.sequence}. The board runs it at its next boot if it passes verification` +
        (written.lastGood
          ? `, and slot ${written.lastGood} stays as the last good package.`
          : '.'),
    );
  } catch (error: any) {
    console.error(error.message);
    process.exit(1);
  }
}

/** A build_id as the error screen shows it. */
const hexBuildId = (buildId: number): string =>
  (buildId >>> 0).toString(16).padStart(8, '0');

/** `embedded-react where <function> <offset> [--debug file] [--build id]` */
async function where(args: string[]): Promise<void> {
  const cwd = process.cwd();
  const debugPath = resolve(
    cwd,
    optValue(args, '--debug')?.value ?? 'dist/app.erdbg',
  );
  const buildOption = optValue(args, '--build')?.value;
  const [functionText, offsetText] = bareArgs(args, ['--debug', '--build']);
  const isCount = (text: string | undefined): text is string =>
    text !== undefined && /^\d+$/.test(text);
  if (!isCount(functionText) || !isCount(offsetText)) {
    console.error(
      'Pass the function and offset the error screen shows: embedded-react where 3 12\n',
    );
    usage();
    process.exit(1);
  }
  if (!existsSync(debugPath)) {
    console.error(
      `No debug file at ${debugPath}. Build the app with: embedded-react build --aot`,
    );
    process.exit(1);
  }
  const {placeOf, readDebugFile} = await import('./aot/compile.mts');
  let debugFile;
  try {
    debugFile = readDebugFile(readFileSync(debugPath, 'utf8'));
  } catch (error: any) {
    console.error(`${relative(cwd, debugPath)}: ${error.message}`);
    process.exit(1);
  }
  const fileBuild = hexBuildId(debugFile.buildId);
  if (buildOption !== undefined) {
    const wanted = buildOption.toLowerCase().replace(/^0x/, '');
    if (!/^[0-9a-f]{1,8}$/.test(wanted)) {
      console.error(
        `--build expects the build id the error screen shows (e.g. --build 1a2b3c4d), got: ${buildOption}`,
      );
      process.exit(1);
    }
    if (wanted.padStart(8, '0') !== fileBuild) {
      console.error(
        `${relative(cwd, debugPath)} is build ${fileBuild}, but the board runs build ${wanted.padStart(8, '0')}. ` +
          'Use the debug file of the build the board runs.',
      );
      process.exit(1);
    }
  }
  const places = placeOf(debugFile, Number(functionText), Number(offsetText));
  if (!places.length) {
    console.error(
      `Build ${fileBuild} has no source line for function ${functionText} at ${offsetText}.`,
    );
    process.exit(1);
  }
  for (const place of places) {
    // The panels number their functions apart, so each layout says where its own function is.
    const sizes = place.panels.map(panel => {
      const {width, height} = debugFile.panels[panel];
      return `${width}x${height}`;
    });
    const layouts =
      places.length > 1
        ? `layout${sizes.length > 1 ? 's' : ''} ${sizes.join(', ')}: `
        : '';
    console.log(
      `${layouts}${debugFile.file}:${place.line}:${place.column}  ${place.function}`,
    );
  }
}

/** addr2line as each toolchain names it, by the ELF's machine, then the host's own. */
const ADDR2LINE_TOOLS: Record<number, string[]> = {
  [ELF_MACHINE_ARM]: ['arm-none-eabi-addr2line'],
  [ELF_MACHINE_XTENSA]: [
    'xtensa-esp32s3-elf-addr2line',
    'xtensa-esp32-elf-addr2line',
    'xtensa-esp-elf-addr2line',
  ],
  [ELF_MACHINE_RISCV]: ['riscv32-esp-elf-addr2line'],
};
const ADDR2LINE_HOST_TOOLS = ['llvm-addr2line', 'addr2line'];

/**
 * Asks addr2line for each address's file and line.
 *
 * @returns The lines by address, and the tool that gave them; null when no tool ran.
 */
function sourceLinesOf(
  elfPath: string,
  machine: number,
  addresses: number[],
  explicitTool: string | undefined,
  cwd: string,
): {lines: Map<number, string>; tool: string | null} {
  const lines = new Map<number, string>();
  const tools = explicitTool
    ? [explicitTool]
    : [...(ADDR2LINE_TOOLS[machine] ?? []), ...ADDR2LINE_HOST_TOOLS];
  for (const tool of tools) {
    const run = spawnSync(
      tool,
      ['-e', elfPath, ...addresses.map(address => `0x${address.toString(16)}`)],
      {encoding: 'utf8'},
    );
    if (run.error || run.status !== 0) {
      continue;
    }
    run.stdout
      .trim()
      .split('\n')
      .forEach((line, index) => {
        const place = /^(.*):(\d+)(?: \(discriminator \d+\))?$/.exec(
          line.trim(),
        );
        if (place && place[1] !== '??' && place[2] !== '0') {
          const inProject = relative(cwd, place[1]);
          lines.set(
            addresses[index],
            `${inProject.startsWith('..') ? resolve(place[1]) : inProject}:${place[2]}`,
          );
        }
      });
    return {lines, tool};
  }
  return {lines, tool: null};
}

/** `embedded-react decode-fault <record> --elf <firmware.elf> [--elf more.elf] [--debug file] [--addr2line tool]` */
async function decodeFault(args: string[]): Promise<void> {
  const cwd = process.cwd();
  // --elf may be given once for each image of the firmware.
  const elfOptions = args.filter(
    (_, argIndex) => argIndex > 0 && args[argIndex - 1] === '--elf',
  );
  const debugOption = optValue(args, '--debug')?.value;
  const [recordArg] = args.filter(
    (arg, argIndex) =>
      !arg.startsWith('--') &&
      !['--elf', '--debug', '--addr2line'].includes(args[argIndex - 1]),
  );
  if (!recordArg || !elfOptions.length) {
    console.error(
      'Pass the record the board logged and the firmware it runs: embedded-react decode-fault <record> --elf firmware.elf\n',
    );
    usage();
    process.exit(1);
  }
  const elfPaths = elfOptions.map(option => resolve(cwd, option));
  for (const elfPath of elfPaths) {
    if (!existsSync(elfPath)) {
      console.error(`No ELF at ${elfPath}.`);
      process.exit(1);
    }
  }
  const recordPath = resolve(cwd, recordArg);
  let record;
  let elf;
  try {
    record = parseFaultRecord(
      existsSync(recordPath) ? readFileSync(recordPath) : recordArg,
    );
    elf = mergeElfs(
      elfPaths.map(elfPath => {
        try {
          return readElf(readFileSync(elfPath));
        } catch (error: any) {
          throw new Error(`${relative(cwd, elfPath)}: ${error.message}`);
        }
      }),
    );
  } catch (error: any) {
    console.error(error.message);
    process.exit(1);
  }

  // The app's line: from the debug file of the same build, else what the board's own debug table gave.
  let appLine: string | null = null;
  let debugNote: string | null = null;
  if (record.app) {
    const app = record.app;
    const build = hexBuildId(app.buildId);
    appLine = app.file
      ? `${app.file}:${app.line}  (build ${build})`
      : `function ${app.function} at ${app.offset} (build ${build}): pass --debug with that build's app.erdbg for the line`;
    const debugPath = resolve(cwd, debugOption ?? 'dist/app.erdbg');
    if (existsSync(debugPath)) {
      const {placeOf, readDebugFile} = await import('./aot/compile.mts');
      try {
        const debugFile = readDebugFile(readFileSync(debugPath, 'utf8'));
        if (debugFile.buildId >>> 0 !== app.buildId) {
          debugNote = `${relative(cwd, debugPath)} is build ${hexBuildId(debugFile.buildId)}, but the board ran build ${build}: it wasn't used.`;
        } else {
          const place = placeOf(debugFile, app.function, app.offset).find(
            candidate => candidate.panels.includes(app.panel),
          );
          if (place) {
            appLine = `${debugFile.file}:${place.line}:${place.column}  ${place.function}  (build ${build})`;
          }
        }
      } catch (error: any) {
        debugNote = `${relative(cwd, debugPath)}: ${error.message}`;
      }
    } else if (debugOption) {
      debugNote = `No debug file at ${debugPath}.`;
    }
  }

  const frames = callChain(record, elf);
  // Each image's own ELF has the lines for its frames.
  const source: {lines: Map<number, string>; tool: string | null} = {
    lines: new Map(),
    tool: null,
  };
  let anyAddress = false;
  elfPaths.forEach((elfPath, image) => {
    const addresses = frames
      .filter(
        frame =>
          frame.kind !== 'interrupt' &&
          frame.function !== null &&
          frame.image === image,
      )
      .map(frame => frame.address);
    if (!addresses.length) {
      return;
    }
    anyAddress = true;
    const found = sourceLinesOf(
      elfPath,
      elf.machine,
      addresses,
      optValue(args, '--addr2line')?.value,
      cwd,
    );
    found.lines.forEach((line, address) => source.lines.set(address, line));
    source.tool ??= found.tool;
  });
  console.log(faultReport(record, frames, source.lines, appLine).join('\n'));
  if (debugNote) {
    console.log(debugNote);
  }
  if (anyAddress && !source.tool) {
    console.log(
      'No addr2line found, so the chain has no files or lines: put the toolchain on PATH, or name its addr2line with --addr2line.',
    );
  }
}

/** `embedded-react native-header <package or file> [--out dir]` */
async function nativeHeader(args: string[]): Promise<void> {
  const cwd = process.cwd();
  const outDir = resolve(cwd, optValue(args, '--out')?.value ?? '.');
  const [target] = bareArgs(args, ['--out']);
  if (!target) {
    console.error(
      'Pass the package or the .ts file that declares the modules.\n',
    );
    usage();
    process.exit(1);
  }
  const {nativeHeaders, readModuleFile} = await import('./aot/compile.mts');
  let shownFile = target;
  try {
    const {path, source, files} = readModuleFile(target, cwd);
    shownFile = relative(cwd, path);
    const headers = nativeHeaders(source, files);
    mkdirSync(outDir, {recursive: true});
    for (const header of headers) {
      writeFileSync(resolve(outDir, header.file), header.text);
      console.log(
        `✓ ${header.module} → ${relative(cwd, resolve(outDir, header.file))}`,
      );
    }
  } catch (error: any) {
    const where = error?.aotLoc
      ? `${shownFile}:${error.aotLoc.line}:${error.aotLoc.column + 1}: `
      : '';
    console.error(where + (error?.message || String(error)));
    if (error?.aotHint) console.error(`  ${error.aotHint}`);
    process.exit(1);
  }
}

const [command, ...commandArgs] = process.argv.slice(2);
if (command === 'dev') {
  await dev(commandArgs);
} else if (command === 'export') {
  await exportApp(commandArgs);
} else if (command === 'build') {
  await build(commandArgs);
} else if (command === 'pack') {
  pack(commandArgs);
} else if (command === 'flash') {
  flash(commandArgs);
} else if (command === 'where') {
  await where(commandArgs);
} else if (command === 'decode-fault') {
  await decodeFault(commandArgs);
} else if (command === 'native-header') {
  await nativeHeader(commandArgs);
} else if (
  !command ||
  command === '--help' ||
  command === '-h' ||
  command === 'help'
) {
  usage();
} else {
  console.error(`Unknown command: ${command}\n`);
  usage();
  process.exit(1);
}
