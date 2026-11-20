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

// erb-gdb.mts — the GDB script (gdb/erb.py) run against a Flow B app on the Linux host (`npm run erb:gdb`).
//
// It builds tools/erb-frames' headless host with debug info, compiles the native-calls fixture with its debug file,
// and runs the host under GDB: a tap presses a button whose handler calls the probe module, which GDB stops in. Each
// command's output is then checked against what the app must show at that point.
//
// Needs CMake, a C compiler and GDB with Python, so it runs on Linux.

import {spawnSync} from 'node:child_process';
import {mkdirSync, readFileSync, writeFileSync} from 'node:fs';
import {basename, dirname, join, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';

import {
  compilePackage,
  packPackage,
  readNativeFiles,
} from '../../aot/compile.mts';

/** A check on one command's output. */
interface Check {
  command: string;
  expected: RegExp;
}

const HERE = dirname(fileURLToPath(import.meta.url));
const PACKAGE_DIR = resolve(HERE, '../..');
const ROOT = resolve(PACKAGE_DIR, '../..');
const HOST_DIR = join(ROOT, 'tools/erb-frames');
const BUILD_DIR = process.env.ERB_GDB_BUILD_DIR || join(HOST_DIR, 'build-gdb');
const FIXTURE = join(PACKAGE_DIR, 'test/erb-frames/fixtures/native-calls.tsx');
const SCRIPT = join(PACKAGE_DIR, 'gdb/erb.py');

/** The "next" button: its handler calls Probe.next() first. */
const NEXT_BUTTON = '32,20';
const SCREEN = {width: 240, height: 320};

/** Marks each command's output, so the checks can find it. */
const MARK = '@@';

const file = basename(FIXTURE);
const CHECKS: Check[] = [
  {command: 'erb where', expected: new RegExp(`^${file}:32  onPress$`, 'm')},
  {
    command: 'bt',
    expected: new RegExp(`erb_vm_run \\[${file}:32  onPress\\]`),
  },
  {command: 'erb state', expected: /^total = 0$[\s\S]*^even = false$/m},
  {command: 'erb state total', expected: /^total = 0$/m},
  {command: 'erb pools', expected: /^ {2}nodes +\d+ of \d+$/m},
  {
    command: 'er nodes',
    expected: new RegExp(
      `^ +pressable #\\d+ +\\d+,\\d+ \\d+x\\d+  ${file}:29$`,
      'm',
    ),
  },
  {command: 'erb error', expected: /^No error: the app is running\.$/m},
  {
    command: 'erb watch total',
    expected: /^Watching total \(watchpoint \d+\); it is 0$/m,
  },
  {
    command: 'continue',
    expected: new RegExp(`^total: 0 → 1, written at ${file}:33  onPress$`, 'm'),
  },
  {command: 'erb set total 41', expected: /^total = 41$/m},
];

/**
 * Runs a command, exiting with its output when it fails.
 *
 * @param command  The program.
 * @param args  Its arguments.
 *
 * @returns Its output.
 */
function run(command: string, args: string[]): string {
  const result = spawnSync(command, args, {encoding: 'utf8'});
  if (result.error || result.status !== 0) {
    console.error(
      `${command} ${args.join(' ')} failed:\n${result.error?.message ?? ''}${result.stdout ?? ''}${result.stderr ?? ''}`,
    );
    process.exit(1);
  }
  return `${result.stdout}${result.stderr}`;
}

if (spawnSync('gdb', ['--version']).status !== 0) {
  console.error('erb-gdb needs GDB with Python (it runs on Linux).');
  process.exit(1);
}

mkdirSync(BUILD_DIR, {recursive: true});
run('cmake', ['-S', HOST_DIR, '-B', BUILD_DIR, '-DCMAKE_BUILD_TYPE=Debug']);
run('cmake', ['--build', BUILD_DIR, '-j']);
const host = join(BUILD_DIR, 'erb-frames');

const source = readFileSync(FIXTURE, 'utf8');
const result = compilePackage(source, 'native-calls', {
  filename: FIXTURE,
  nativeFiles: readNativeFiles(source, dirname(FIXTURE)),
  screen: SCREEN,
});
const packagePath = join(BUILD_DIR, 'native-calls.erpkg');
const debugPath = join(BUILD_DIR, 'native-calls.erdbg');
writeFileSync(packagePath, packPackage(result).bytes);
writeFileSync(debugPath, result.debugFile);

const commands = [
  'set pagination off',
  'set confirm off',
  'set breakpoint pending on',
  `source ${SCRIPT}`,
  `erb file ${debugPath}`,
  'break probe_next',
  `run ${join(BUILD_DIR, 'frame.raw')} ${SCREEN.width} ${SCREEN.height} ${packagePath} ${NEXT_BUTTON} 0`,
  ...CHECKS.flatMap(({command}) => [`echo ${MARK} ${command}\\n`, command]),
  'kill',
];
const scriptPath = join(BUILD_DIR, 'erb-gdb.gdb');
writeFileSync(scriptPath, `${commands.join('\n')}\n`);
const output = spawnSync(
  'gdb',
  ['-q', '-batch', '-nx', '-x', scriptPath, host],
  {
    encoding: 'utf8',
  },
);
const transcript = `${output.stdout}${output.stderr}`;

// Each command's output runs from its mark to the next.
const sections = new Map<string, string>();
const marks = [...transcript.matchAll(new RegExp(`^${MARK} (.+)$`, 'gm'))];
marks.forEach((mark, index) => {
  const end = index + 1 < marks.length ? marks[index + 1].index : undefined;
  sections.set(mark[1], transcript.slice(mark.index + mark[0].length + 1, end));
});

let failed = 0;
for (const {command, expected} of CHECKS) {
  const section = sections.get(command);
  if (section !== undefined && expected.test(section)) {
    console.log(`✓ ${command}`);
  } else {
    failed++;
    console.log(
      `✗ ${command}: expected ${expected}, got:\n${section ?? '(no output)'}`,
    );
  }
}
if (failed) {
  console.log(`\nThe whole session:\n${transcript}`);
  process.exit(1);
}
