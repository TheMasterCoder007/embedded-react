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

// Runtime test runner. Runtime tests need the real QuickJS + C engine, so each one is bundled and
// then executed by the headless harness (er-bridge-quickjs-runtest), which installs the bridge, a
// no-op backend, console, and a `screen` global, evaluates the bundle, and exits non-zero if the
// test recorded any failures (globalThis.__runtime_failed) or threw.
import {build} from 'esbuild';
import {execFileSync, spawnSync} from 'node:child_process';
import {readdirSync, existsSync, mkdirSync, readFileSync} from 'node:fs';
import {fileURLToPath} from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url)); // .../js/test/runtime
const jsRoot = path.resolve(here, '../..'); // .../js
const outDir = path.join(jsRoot, 'dist', 'runtime');

// --bytecode: precompile each bundle to a QuickJS bytecode blob (.qbc) and run THAT instead of the
// source — exercises the bytecode load path the MCU hosts use (er-bridge-quickjs-compile +
// JS_ReadObject). The QuickJS VM still runs it; this is Flow A, not the Flow B AOT compiler.
const bytecodeMode = process.argv.includes('--bytecode');

// The bridge build holding the harness (and the precompiler, for --bytecode): bridges/quickjs/build, or the
// directory ER_BRIDGE_BUILD_DIR names (CI builds into its own), resolved from the current directory.
const bridgeBuild = process.env.ER_BRIDGE_BUILD_DIR
  ? path.resolve(process.env.ER_BRIDGE_BUILD_DIR)
  : path.resolve(jsRoot, '..', 'build');
const exeSuffix = process.platform === 'win32' ? '.exe' : '';
const exe = path.join(bridgeBuild, `er-bridge-quickjs-runtest${exeSuffix}`);
const compileExe = path.join(
  bridgeBuild,
  `er-bridge-quickjs-compile${exeSuffix}`,
);

// The build directory as the hint's cmake command should name it: from the repo root when it is inside it.
const repoRoot = path.resolve(jsRoot, '..', '..', '..');
const fromRoot = path.relative(repoRoot, bridgeBuild);
const buildArg = fromRoot.startsWith('..') ? bridgeBuild : fromRoot;
const buildHint = target =>
  `Build it first (from the repo root):\n  cmake --build ${buildArg} --target ${target}\n` +
  'or set ER_BRIDGE_BUILD_DIR to a bridge build that has it.';

if (!existsSync(exe)) {
  console.error(`Runtime test harness not found at:\n  ${exe}\n`);
  console.error(buildHint('er-bridge-quickjs-runtest'));
  process.exit(2);
}
if (bytecodeMode && !existsSync(compileExe)) {
  console.error(`Bytecode compiler not found at:\n  ${compileExe}\n`);
  console.error(buildHint('er-bridge-quickjs-compile'));
  process.exit(2);
}

const tests = readdirSync(here).filter(
  f => f.endsWith('.runtime.test.jsx') || f.endsWith('.runtime.test.js'),
);
if (tests.length === 0) {
  console.log('No runtime tests found.');
  process.exit(0);
}

mkdirSync(outDir, {recursive: true});

let failed = 0;
for (const test of tests) {
  const bundle = path.join(outDir, test.replace(/\.jsx?$/, '.bundle.js'));
  await build({
    entryPoints: [path.join(here, test)],
    bundle: true,
    format: 'iife',
    outfile: bundle,
    platform: 'neutral',
    target: 'es2020',
    jsx: 'automatic',
    define: {'process.env.NODE_ENV': '"production"'},
    legalComments: 'none',
    logLevel: 'warning',
  });

  // In bytecode mode, precompile the bundle and run the blob instead of the source.
  let runArg = bundle;
  if (bytecodeMode) {
    runArg = bundle.replace(/\.bundle\.js$/, '.bundle.qbc');
    execFileSync(compileExe, [bundle, runArg]);
  }

  // The harness runs the device's lite intrinsic profile so a bundle that needs a stripped intrinsic
  // fails here rather than on hardware. A test that is specifically about an opt-in intrinsic asks for
  // it with a marker comment, and only that test gets it.
  const source = readFileSync(path.join(here, test), 'utf8');
  const flags = source.includes('@runtime-intrinsics: typed-arrays')
    ? ['--typed-arrays']
    : [];

  // A test about a diagnostic lists the stderr lines it expects, one marker each; its stderr must be
  // exactly those lines.
  const expectedStderr = [...source.matchAll(/@runtime-stderr: (.*)$/gm)].map(
    m => m[1],
  );

  process.stdout.write(
    `\n=== ${test}${bytecodeMode ? ' [bytecode]' : ''} ===\n`,
  );
  const run = spawnSync(exe, [...flags, runArg], {encoding: 'utf8'});
  process.stdout.write(run.stdout ?? '');
  const stderrLines = (run.stderr ?? '').split('\n').filter(Boolean);
  const stderrOk =
    expectedStderr.length === 0 ||
    JSON.stringify(stderrLines) === JSON.stringify(expectedStderr);
  if (run.status !== 0 || !stderrOk) {
    process.stderr.write(run.stderr ?? '');
    failed++;
    console.error(
      run.status !== 0
        ? `FAILED: ${test} (exit ${run.status ?? run.error})`
        : `FAILED: ${test} (stderr is not the expected ${JSON.stringify(expectedStderr)})`,
    );
  } else if (expectedStderr.length === 0) {
    process.stderr.write(run.stderr ?? '');
  }
}

console.log(
  `\nRuntime tests${bytecodeMode ? ' [bytecode]' : ''}: ${tests.length - failed}/${tests.length} passed`,
);
process.exit(failed === 0 ? 0 : 1);
