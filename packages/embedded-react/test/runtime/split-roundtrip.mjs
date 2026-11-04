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

const here = dirname(fileURLToPath(import.meta.url)); // .../embedded-react/test/runtime
const jsRoot = resolve(here, '../..'); // .../embedded-react
const librarySource = resolve(jsRoot, 'src/embedded-react/index.js');
const simDirectory = resolve(jsRoot, 'sim');
const nodePaths = [resolve(jsRoot, 'node_modules')];
const fixtures = resolve(here, 'split-fixtures');
const outputDirectory = resolve(jsRoot, 'dist', 'runtime');

const exeSuffix = process.platform === 'win32' ? '.exe' : '';
// bridges/quickjs/build, or the bridge build ER_BRIDGE_BUILD_DIR names, as for run.mjs.
const executable = resolve(
  process.env.ER_BRIDGE_BUILD_DIR ||
    resolve(jsRoot, '../../bridges/quickjs/build'),
  `er-bridge-quickjs-splittest${exeSuffix}`,
);

if (!existsSync(resolve(simDirectory, 'embedded-react.cjs'))) {
  console.error(
    `Prebuilt sim wasm not found at ${simDirectory}/embedded-react.cjs\n  Build it: node tools/web-sim/build.mjs`,
  );
  process.exit(2);
}
if (!existsSync(executable)) {
  console.error(
    `Harness not built at:\n  ${executable}\n  Build it: cmake --build bridges/quickjs/build --target er-bridge-quickjs-splittest\n` +
      '  or set ER_BRIDGE_BUILD_DIR to a bridge build that has it.',
  );
  process.exit(2);
}

mkdirSync(outputDirectory, {recursive: true});

console.log('building vendor + boot-app + reloaded-app …');
const vendor = await packVendor({
  libSrc: librarySource,
  nodePaths,
  simDir: simDirectory,
});
const bootApp = await packApp({
  entry: resolve(fixtures, 'boot-app.jsx'),
  projectRoot: fixtures,
  libSrc: librarySource,
  nodePaths,
  simDir: simDirectory,
});
const reloadedApp = await packApp({
  entry: resolve(fixtures, 'reloaded-app.jsx'),
  projectRoot: fixtures,
  libSrc: librarySource,
  nodePaths,
  simDir: simDirectory,
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

const bootPath = resolve(outputDirectory, 'split-boot.erpkg');
const framePath = resolve(outputDirectory, 'split-reloaded.erpkg');
writeFileSync(bootPath, boot);
writeFileSync(framePath, frame);
const BYTES_PER_KILOBYTE = 1024;
const kb = bytes => `${(bytes / BYTES_PER_KILOBYTE).toFixed(1)} KB`;
console.log(
  `boot ${kb(boot.length)} (vendor ${kb(vendor.bytecodeLen)} + app ${kb(bootApp.bytecodeLen)}), ` +
    `soft-reload frame ${kb(frame.length)} (app ${kb(reloadedApp.bytecodeLen)})\n`,
);

try {
  process.stdout.write(
    execFileSync(executable, [bootPath, framePath], {encoding: 'utf8'}),
  );
  console.log('\nsplit round-trip: PASS');
  process.exit(0);
} catch (error) {
  if (error.stdout) process.stdout.write(error.stdout);
  if (error.stderr) process.stderr.write(error.stderr);
  console.error('\nsplit round-trip: FAIL');
  process.exit(1);
}
