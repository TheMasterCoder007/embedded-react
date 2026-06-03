// Runtime test runner. Runtime tests need the real QuickJS + C engine, so each one is bundled and
// then executed by the headless harness (er-bridge-quickjs-runtest), which installs the bridge, a
// no-op backend, console, and a `screen` global, evaluates the bundle, and exits non-zero if the
// test recorded any failures (globalThis.__runtime_failed) or threw.
import { build } from 'esbuild';
import { execFileSync } from 'node:child_process';
import { readdirSync, existsSync, mkdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url)); // .../js/test/runtime
const jsRoot = path.resolve(here, '../..'); // .../js
const outDir = path.join(jsRoot, 'dist', 'runtime');

// --bytecode: precompile each bundle to a QuickJS bytecode blob (.qbc) and run THAT instead of the
// source — exercises the bytecode load path the MCU hosts use (er-bridge-quickjs-compile +
// JS_ReadObject). The QuickJS VM still runs it; this is Flow A, not the Flow B AOT compiler.
const bytecodeMode = process.argv.includes('--bytecode');

const bridgeBuild = path.resolve(jsRoot, '..', 'build');
const exeSuffix = process.platform === 'win32' ? '.exe' : '';
const exe = path.join(bridgeBuild, `er-bridge-quickjs-runtest${exeSuffix}`);
const compileExe = path.join(bridgeBuild, `er-bridge-quickjs-compile${exeSuffix}`);

if (!existsSync(exe)) {
  console.error(`Runtime test harness not found at:\n  ${exe}\n`);
  console.error('Build it first:\n  cmake --build bridges/quickjs/build --target er-bridge-quickjs-runtest');
  process.exit(2);
}
if (bytecodeMode && !existsSync(compileExe)) {
  console.error(`Bytecode compiler not found at:\n  ${compileExe}\n`);
  console.error('Build it first:\n  cmake --build bridges/quickjs/build --target er-bridge-quickjs-compile');
  process.exit(2);
}

const tests = readdirSync(here).filter((f) => f.endsWith('.runtime.test.jsx') || f.endsWith('.runtime.test.js'));
if (tests.length === 0) {
  console.log('No runtime tests found.');
  process.exit(0);
}

mkdirSync(outDir, { recursive: true });

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
    define: { 'process.env.NODE_ENV': '"production"' },
    legalComments: 'none',
    logLevel: 'warning',
  });

  // In bytecode mode, precompile the bundle and run the blob instead of the source.
  let runArg = bundle;
  if (bytecodeMode) {
    runArg = bundle.replace(/\.bundle\.js$/, '.bundle.qbc');
    execFileSync(compileExe, [bundle, runArg]);
  }

  process.stdout.write(`\n=== ${test}${bytecodeMode ? ' [bytecode]' : ''} ===\n`);
  try {
    const out = execFileSync(exe, [runArg], { encoding: 'utf8' });
    process.stdout.write(out);
  } catch (e) {
    if (e.stdout) process.stdout.write(e.stdout);
    if (e.stderr) process.stderr.write(e.stderr);
    failed++;
    console.error(`FAILED: ${test} (exit ${e.status})`);
  }
}

console.log(`\nRuntime tests${bytecodeMode ? ' [bytecode]' : ''}: ${tests.length - failed}/${tests.length} passed`);
process.exit(failed === 0 ? 0 : 1);
