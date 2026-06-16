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

// consumer-smoke.mjs — pack the npm package, install it into a throwaway project, and run the consumer
// build commands. This guards the bugs the repo's own tests can't see, because they only surface once the
// package is PACKED and INSTALLED elsewhere: a file missing from the `files` whitelist, ESM-vs-CJS module
// resolution, and the QuickJS bytecode compile through the prebuilt wasm. (Every consumer regression this
// project hit — persist-transform recursion, the missing `.cjs`, the un-terminated bytecode buffer — would
// have been caught here.)
//
//   node tools/consumer-smoke.mjs
//
// Flow A (app.erpkg) runs only when the prebuilt sim wasm is present (build it first with
// tools/web-sim/build.mjs); the AOT path (app.gen.c) always runs. Exits non-zero on any failure.

import { execFileSync } from 'node:child_process';
import { mkdtempSync, writeFileSync, existsSync, readFileSync, cpSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { resolve, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const JS = resolve(ROOT, 'bridges/quickjs/js');
const TEMPLATE = resolve(ROOT, 'create-embedded-react/template');

const run = (cmd, args, cwd) => execFileSync(cmd, args, { stdio: 'inherit', cwd, shell: process.platform === 'win32' });
const capture = (cmd, args, cwd) => execFileSync(cmd, args, { encoding: 'utf8', cwd, shell: process.platform === 'win32' });
const ok = (cond, msg) => {
  console.log(`${cond ? '✓' : '✗'} ${msg}`);
  if (!cond) process.exitCode = 1;
};

// 1. Pack exactly what would publish (includes the staged sim/ wasm, if built).
const tgz = resolve(JS, JSON.parse(capture('npm', ['pack', '--json'], JS))[0].filename);
const hasWasm = existsSync(resolve(JS, 'sim/embedded-react.wasm'));
console.log(`packed ${tgz}\nprebuilt wasm present: ${hasWasm}\n`);

// 2. A throwaway consumer project from the scaffolder template (the pulsing-logo starter = a Flow A app).
const proj = mkdtempSync(join(tmpdir(), 'er-smoke-'));
cpSync(TEMPLATE, proj, { recursive: true });
writeFileSync(
  resolve(proj, 'package.json'),
  JSON.stringify({ name: 'smoke', private: true, type: 'module', dependencies: { 'embedded-react': `file:${tgz}`, react: '18.3.1' } }, null, 2),
);
run('npm', ['install', '--no-audit', '--no-fund'], proj);

// 3. Flow A → app.erpkg. Exercises the wasm bytecode compiler end to end (the highest-value leg — it loads
//    the .cjs module under Node and compiles a real ~240 KB bundle, the exact path that broke before).
if (hasWasm) {
  run('npx', ['embedded-react', 'build'], proj);
  const erpkg = resolve(proj, 'dist/app.erpkg');
  ok(existsSync(erpkg), 'embedded-react build → dist/app.erpkg');
  ok(existsSync(erpkg) && readFileSync(erpkg).subarray(0, 4).toString('latin1') === 'ERCF', 'app.erpkg has the ERCF magic');
} else {
  console.log('• skipping Flow A — no prebuilt wasm (run node tools/web-sim/build.mjs to include it)');
}

// 4. Flow B (AOT) → app.gen.c. Pure JS (no wasm). The template app uses Animated.loop, which AOT doesn't
//    support yet, so use a minimal AOT-subset app via an explicit entry.
writeFileSync(
  resolve(proj, 'aot-app.jsx'),
  `import { useState } from 'react';
import { View, Text, Pressable, StyleSheet } from 'embedded-react';
export function App() {
  const [c, setC] = useState(0);
  return (
    <View style={s.r}>
      <Text style={s.t}>count is {c}</Text>
      <Pressable onPress={() => setC((n) => n + 1)}><Text style={s.t}>tap</Text></Pressable>
    </View>
  );
}
const s = StyleSheet.create({ r: { flex: 1 }, t: { color: '#fff', fontSize: 20 } });
`,
);
run('npx', ['embedded-react', 'build', '--aot', 'aot-app.jsx', '--out', 'dist-aot'], proj);
ok(existsSync(resolve(proj, 'dist-aot/app.gen.c')), 'embedded-react build --aot → dist-aot/app.gen.c');

// Clean up the packed tarball + throwaway project (best-effort).
rmSync(tgz, { force: true });
rmSync(proj, { recursive: true, force: true });

console.log(process.exitCode ? '\n✗ consumer smoke FAILED' : '\n✓ consumer smoke passed');
