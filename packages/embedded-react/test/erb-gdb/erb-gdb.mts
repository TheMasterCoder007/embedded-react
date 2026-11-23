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
// It builds tools/erb-frames' headless host with debug info and the VM's breakpoint hook, compiles fixtures with their
// debug files, and runs the host under GDB, once for each. In the native-calls fixture a tap presses a button whose
// handler calls the probe module, which GDB stops in. A breakpoint set before the run then stops the same handler,
// which is stepped through. The lists fixture stops in a row's handler, which is entered with its row, and the
// animations fixture in a fade's completion, which another function calls. The list-fault fixture's handler reads a
// row its list doesn't have, which stops the run by itself, and runs through with fault stops off. Each command's
// output is checked against what the app must show at that point.
//
// Needs CMake, a C compiler and GDB with Python, so it runs on Linux.

import {spawnSync} from 'node:child_process';
import {mkdirSync, readFileSync, writeFileSync} from 'node:fs';
import {dirname, join, resolve} from 'node:path';
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
  /** What GDB runs, when it isn't the command: an error stops a batch script, so one that must fail is caught. */
  run?: string;
}

const HERE = dirname(fileURLToPath(import.meta.url));
const PACKAGE_DIR = resolve(HERE, '../..');
const ROOT = resolve(PACKAGE_DIR, '../..');
const HOST_DIR = join(ROOT, 'tools/erb-frames');
const BUILD_DIR = process.env.ERB_GDB_BUILD_DIR || join(HOST_DIR, 'build-gdb');
const FIXTURES = join(PACKAGE_DIR, 'test/erb-frames/fixtures');
const SCRIPT = join(PACKAGE_DIR, 'gdb/erb.py');

const SCREEN = {width: 240, height: 320};

/** Marks each command's output, so the checks can find it. */
const MARK = '@@';

/** One run of the host under GDB. */
interface Session {
  fixture: string;
  /** The host's taps, and how long it runs after them in ms. */
  taps: string;
  settle: number;
  /** Checks before the run: a breakpoint waits for the load. */
  before: Check[];
  /** Commands before the run that aren't checked. */
  setup: string[];
  /** What the run prints as it stops, when that is checked. */
  stop?: RegExp;
  /** Checks once the run stops. */
  checks: Check[];
}

/**
 * A command that must fail, its error caught so the batch script goes on.
 *
 * @param command  The command.
 *
 * @returns What GDB runs.
 */
const caught = (command: string): string =>
  [
    'python',
    'try:',
    `    gdb.execute("${command}")`,
    'except gdb.error as error:',
    '    print(error)',
    'end',
  ].join('\n');

let file = 'native-calls.tsx';
/** Before the app runs: a breakpoint waits for the load. */
const BEFORE_RUN: Check[] = [
  {
    command: `erb break ${file}:34`,
    expected: new RegExp(
      `^erb breakpoint 1 at ${file}:34 \\(set when the app loads\\)$`,
      'm',
    ),
  },
];
const CHECKS: Check[] = [
  {command: 'erb where', expected: new RegExp(`^${file}:32  onPress$`, 'm')},
  // Partway through `const step = Probe.next()`, so step holds nothing yet.
  {command: 'erb locals', expected: /^No locals\.$/m},
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
  {command: 'print (int) *erb_debug.halted', expected: /^\$\d+ = 1$/m},
  {command: 'print (int) *er_debug.halted', expected: /^\$\d+ = 1$/m},
  {
    command: `erb break ${file}:31`,
    run: caught(`erb break ${file}:31`),
    expected: new RegExp(
      `^${file}:31 has no code; the nearest lines with code are 22 and 32$`,
      'm',
    ),
  },
  {
    command: 'continue',
    // The watch and the native call's breakpoint would stop first.
    run: 'delete\ncontinue',
    expected: new RegExp(`^erb breakpoint 1, ${file}:34  onPress$`, 'm'),
  },
  {command: 'erb locals', expected: /^step = 1$/m},
  {
    command: 'erb locals 1',
    run: caught('erb locals 1'),
    expected: /^the app's call chain has 1 frame$/m,
  },
  {command: 'erb step', expected: new RegExp(`^${file}:35  onPress$`, 'm')},
  {command: 'erb next', expected: new RegExp(`^${file}:36  onPress$`, 'm')},
  {command: 'erb finish', expected: /^onPress returns to the runtime$/m},
  // Past the handler: the app's next statement, wherever it is.
  {command: 'erb step', expected: new RegExp(`^${file}:\\d+  \\S`, 'm')},
  {command: 'erb delete', expected: /^Deleted 1 erb breakpoint\.$/m},
  {command: 'erb break', expected: /^No erb breakpoints\.$/m},
];

file = 'lists.tsx';
/** A row's press: its handler is entered with the row, which it reads as its item. */
const LIST_BEFORE: Check[] = [
  {
    command: `erb break ${file}:59`,
    expected: new RegExp(`^erb breakpoint 1 at ${file}:59`, 'm'),
  },
];
const LIST_CHECKS: Check[] = [
  {
    command: 'erb where',
    expected: new RegExp(
      `^${file}:59  onPress  \\(item=\\{[^}]*label: "(one|two|three)"[^}]*\\}\\)$`,
      'm',
    ),
  },
  {
    command: 'erb locals',
    expected: /^item = \{[^}]*label: "(one|two|three)"[^}]*\}$/m,
  },
];

file = 'animations.tsx';
/** The fade's completion, which the animation's own function calls with whether it finished. */
const COMPLETION_BEFORE: Check[] = [
  {
    command: `erb break ${file}:40`,
    expected: new RegExp(`^erb breakpoint 1 at ${file}:40`, 'm'),
  },
];
const COMPLETION_CHECKS: Check[] = [
  {
    command: 'erb where',
    expected: new RegExp(
      `^${file}:40  completion done_\\d+  \\(finished=true\\)\\n  called from animation done_\\d+`,
      'm',
    ),
  },
  {command: 'erb locals', expected: /^finished = true$/m},
  {command: 'erb locals 1', expected: /^No locals\.$/m},
];

file = 'list-fault.tsx';
/** A fault stops the run by itself, before the runtime tears the app down. */
const FAULT_STOP = new RegExp(
  `^App fault: list read past its end\\n  check: list row past the list's end\\n  at ${file}:18  onPress$`,
  'm',
);
const FAULT_CHECKS: Check[] = [
  {command: 'erb where', expected: new RegExp(`^${file}:18  onPress$`, 'm')},
  {command: 'erb locals', expected: /^row = 5$/m},
  {
    command: 'erb error',
    expected: /^list read past its end$[\s\S]*^The app stopped\.$/m,
  },
  {
    command: 'erb step',
    run: caught('erb step'),
    expected: /^the app faulted, so it has no next statement/m,
  },
  {command: 'erb faults', expected: /^A fault stops the board\.$/m},
];
const FAULTS_OFF: Check[] = [
  {command: 'erb faults off', expected: /^A fault doesn't stop the board\.$/m},
];

const SESSIONS: Session[] = [
  {
    fixture: 'native-calls',
    // The "next" button: its handler calls Probe.next() first.
    taps: '32,20',
    settle: 0,
    before: BEFORE_RUN,
    setup: ['break probe_next'],
    checks: CHECKS,
  },
  {
    fixture: 'lists',
    taps: '20,76',
    settle: 0,
    before: LIST_BEFORE,
    setup: [],
    checks: LIST_CHECKS,
  },
  {
    fixture: 'animations',
    // The fade runs for 400 ms.
    taps: '28,23',
    settle: 800,
    before: COMPLETION_BEFORE,
    setup: [],
    checks: COMPLETION_CHECKS,
  },
  {
    fixture: 'list-fault',
    taps: '32,20',
    settle: 0,
    before: [],
    setup: [],
    stop: FAULT_STOP,
    checks: FAULT_CHECKS,
  },
  {
    fixture: 'list-fault',
    taps: '32,20',
    settle: 0,
    before: FAULTS_OFF,
    setup: [],
    // The host runs to its end, and says the app stopped.
    stop: /exited with code 01/,
    checks: [],
  },
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

/**
 * Each check's command, after a mark that numbers it, as a command can be checked more than once.
 *
 * @param checks  The checks.
 * @param first  The first one's number.
 *
 * @returns GDB's commands.
 */
function marked(checks: Check[], first: number): string[] {
  return checks.flatMap(({command, run: gdbCommand}, index) => [
    `echo ${MARK} ${first + index} ${command}\\n`,
    gdbCommand ?? command,
  ]);
}

/**
 * Compiles a session's fixture and runs the host under GDB, printing each check.
 *
 * @param session  The session.
 * @param host  The host.
 *
 * @returns How many checks failed.
 */
function runSession(session: Session, host: string): number {
  const fixture = join(FIXTURES, `${session.fixture}.tsx`);
  const source = readFileSync(fixture, 'utf8');
  const result = compilePackage(source, session.fixture, {
    filename: fixture,
    nativeFiles: readNativeFiles(source, dirname(fixture)),
    screen: SCREEN,
  });
  const packagePath = join(BUILD_DIR, `${session.fixture}.erpkg`);
  const debugPath = join(BUILD_DIR, `${session.fixture}.erdbg`);
  writeFileSync(packagePath, packPackage(result).bytes);
  writeFileSync(debugPath, result.debugFile);

  const runCommand = `run ${join(BUILD_DIR, 'frame.raw')} ${SCREEN.width} ${SCREEN.height} ${packagePath} ${session.taps} ${session.settle}`;
  const atRun: Check[] = session.stop
    ? [{command: 'run', run: runCommand, expected: session.stop}]
    : [];
  const checks = [...session.before, ...atRun, ...session.checks];
  const commands = [
    'set pagination off',
    'set confirm off',
    'set breakpoint pending on',
    `source ${SCRIPT}`,
    `erb file ${debugPath}`,
    ...marked(session.before, 0),
    ...session.setup,
    ...(atRun.length ? marked(atRun, session.before.length) : [runCommand]),
    ...marked(session.checks, session.before.length + atRun.length),
    'kill',
  ];
  const scriptPath = join(BUILD_DIR, `erb-gdb-${session.fixture}.gdb`);
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
  const sections = new Map<number, string>();
  const marks = [
    ...transcript.matchAll(new RegExp(`^${MARK} (\\d+) .+$`, 'gm')),
  ];
  marks.forEach((mark, index) => {
    const end = index + 1 < marks.length ? marks[index + 1].index : undefined;
    sections.set(
      Number(mark[1]),
      transcript.slice(mark.index + mark[0].length + 1, end),
    );
  });

  let failed = 0;
  for (const [index, {command, expected}] of checks.entries()) {
    const section = sections.get(index);
    if (section !== undefined && expected.test(section)) {
      console.log(`✓ ${session.fixture}: ${command}`);
    } else {
      failed++;
      console.log(
        `✗ ${session.fixture}: ${command}: expected ${expected}, got:\n${section ?? '(no output)'}`,
      );
    }
  }
  if (failed) {
    console.log(`\nThe whole session:\n${transcript}`);
  }
  return failed;
}

if (spawnSync('gdb', ['--version']).status !== 0) {
  console.error('erb-gdb needs GDB with Python (it runs on Linux).');
  process.exit(1);
}

mkdirSync(BUILD_DIR, {recursive: true});
run('cmake', [
  '-S',
  HOST_DIR,
  '-B',
  BUILD_DIR,
  '-DCMAKE_BUILD_TYPE=Debug',
  '-DERB_DEBUG_HOOK=ON',
]);
run('cmake', ['--build', BUILD_DIR, '-j']);
const host = join(BUILD_DIR, 'erb-frames');

let failed = 0;
for (const session of SESSIONS) {
  failed += runSession(session, host);
}
if (failed) {
  process.exit(1);
}
