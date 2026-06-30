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

// Desktop round-trip for the vendor/app split + soft reload. Builds a SPLIT boot container (vendor +
// boot-app) and an app-only frame (reloaded-app) with pack-split, then runs the C harness
// (er-bridge-quickjs-splittest) which loads them through er_runtime in real QuickJS and asserts the
// resident vendor is reused + usePersistentState survives. Needs the prebuilt sim wasm and the harness
// binary, so it's a local/integration check — NOT part of the CI vitest job.
import {resolve, dirname} from 'node:path';
import {fileURLToPath} from 'node:url';
import {writeFileSync, mkdirSync, existsSync} from 'node:fs';
import {execFileSync} from 'node:child_process';
import {
  packVendor,
  packApp,
  emitBootContainer,
  emitAppFrame,
} from '../../hotreload/pack-split.mjs';

const here = dirname(fileURLToPath(import.meta.url)); // .../js/test/runtime
const jsRoot = resolve(here, '../..'); // .../js
const libSrc = resolve(jsRoot, 'src/embedded-react/index.js');
const simDir = resolve(jsRoot, 'sim');
const nodePaths = [resolve(jsRoot, 'node_modules')];
const fixtures = resolve(here, 'split-fixtures');
const outDir = resolve(jsRoot, 'dist', 'runtime');

const exeSuffix = process.platform === 'win32' ? '.exe' : '';
const exe = resolve(
  jsRoot,
  '..',
  'build',
  `er-bridge-quickjs-splittest${exeSuffix}`,
);

if (!existsSync(resolve(simDir, 'embedded-react.cjs'))) {
  console.error(
    `Prebuilt sim wasm not found at ${simDir}/embedded-react.cjs\n  Build it: node tools/web-sim/build.mjs`,
  );
  process.exit(2);
}
if (!existsSync(exe)) {
  console.error(
    `Harness not built at:\n  ${exe}\n  Build it: cmake --build bridges/quickjs/build --target er-bridge-quickjs-splittest`,
  );
  process.exit(2);
}

mkdirSync(outDir, {recursive: true});

console.log('building vendor + boot-app + reloaded-app …');
const vendor = await packVendor({libSrc, nodePaths, simDir});
const bootApp = await packApp({
  entry: resolve(fixtures, 'boot-app.jsx'),
  projectRoot: fixtures,
  libSrc,
  nodePaths,
  simDir,
});
const reloadedApp = await packApp({
  entry: resolve(fixtures, 'reloaded-app.jsx'),
  projectRoot: fixtures,
  libSrc,
  nodePaths,
  simDir,
});

const boot = await emitBootContainer({
  vendorBytecode: vendor.bytecode,
  appBytecode: bootApp.bytecode,
  assetPack: bootApp.assetPack,
});
const frame = await emitAppFrame({
  appBytecode: reloadedApp.bytecode,
  assetPack: reloadedApp.assetPack,
});

const bootPath = resolve(outDir, 'split-boot.erpkg');
const framePath = resolve(outDir, 'split-reloaded.erpkg');
writeFileSync(bootPath, boot);
writeFileSync(framePath, frame);
const kb = n => `${(n / 1024).toFixed(1)} KB`;
console.log(
  `boot ${kb(boot.length)} (vendor ${kb(vendor.bytecodeLen)} + app ${kb(bootApp.bytecodeLen)}), ` +
    `soft-reload frame ${kb(frame.length)} (app ${kb(reloadedApp.bytecodeLen)})\n`,
);

try {
  process.stdout.write(
    execFileSync(exe, [bootPath, framePath], {encoding: 'utf8'}),
  );
  console.log('\nsplit round-trip: PASS');
  process.exit(0);
} catch (e) {
  if (e.stdout) process.stdout.write(e.stdout);
  if (e.stderr) process.stderr.write(e.stderr);
  console.error('\nsplit round-trip: FAIL');
  process.exit(1);
}
