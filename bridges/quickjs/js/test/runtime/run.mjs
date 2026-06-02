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

// The harness exe is built by the standalone bridge build (no SDL).
const exeName = process.platform === 'win32' ? 'er-bridge-quickjs-runtest.exe' : 'er-bridge-quickjs-runtest';
const exe = path.resolve(jsRoot, '..', 'build', exeName); // .../bridges/quickjs/build/<exe>

if (!existsSync(exe)) {
  console.error(`Runtime test harness not found at:\n  ${exe}\n`);
  console.error('Build it first:\n  cmake --build bridges/quickjs/build --target er-bridge-quickjs-runtest');
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

  process.stdout.write(`\n=== ${test} ===\n`);
  try {
    const out = execFileSync(exe, [bundle], { encoding: 'utf8' });
    process.stdout.write(out);
  } catch (e) {
    if (e.stdout) process.stdout.write(e.stdout);
    if (e.stderr) process.stderr.write(e.stderr);
    failed++;
    console.error(`FAILED: ${test} (exit ${e.status})`);
  }
}

console.log(`\nRuntime tests: ${tests.length - failed}/${tests.length} passed`);
process.exit(failed === 0 ? 0 : 1);
