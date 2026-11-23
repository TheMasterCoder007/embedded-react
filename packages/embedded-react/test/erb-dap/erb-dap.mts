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

// erb-dap.mts — the debug adapter (`embedded-react debug-adapter`) run against a Flow B app on the Linux host
// (`npm run erb:dap`).
//
// It builds tools/erb-frames' headless host as erb-gdb does, compiles the native-calls fixture with its debug file, and
// plays an editor's part of a session: a breakpoint in the fixture's source and one on a C function, a tap that
// reaches both, the call stack and variables at each stop, and the step buttons in C and in the app. A second session
// runs the list-fault fixture, whose handler reads a row its list doesn't have, to the stop at the fault, and a third
// gives the host that package cut short, to the stop at the refused load.
//
// Needs CMake, a C compiler and GDB 14 or later with Python, so it runs on Linux.

import {spawn, spawnSync} from 'node:child_process';
import {mkdirSync, readFileSync, writeFileSync} from 'node:fs';
import {dirname, join, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';

import {
  compilePackage,
  packPackage,
  readNativeFiles,
} from '../../aot/compile.mts';
import {
  readMessages,
  writeMessage,
  type Message,
} from '../../debug-adapter.mts';

type Body = Record<string, any>;

const HERE = dirname(fileURLToPath(import.meta.url));
const PACKAGE_DIR = resolve(HERE, '../..');
const ROOT = resolve(PACKAGE_DIR, '../..');
const HOST_DIR = join(ROOT, 'tools/erb-frames');
const BUILD_DIR = process.env.ERB_GDB_BUILD_DIR || join(HOST_DIR, 'build-gdb');
const FIXTURES = join(PACKAGE_DIR, 'test/erb-frames/fixtures');
const FIXTURE = join(FIXTURES, 'native-calls.tsx');
const FAULT_FIXTURE = join(FIXTURES, 'list-fault.tsx');

const SCREEN = {width: 240, height: 320};
/** The "next" button: its handler calls Probe.next() first. */
const TAP = '32,20';
const WAIT_MS = Number(process.env.ERB_DAP_WAIT_MS) || 60000;
const VM_FUNCTION = 'erb_vm_run';
/** What is left of a package cut short, so its load is refused. */
const CUT_BYTES = 200;

let failed = 0;

/**
 * Prints one check and counts it when it fails.
 *
 * @param name  What is checked.
 * @param passed  Whether it holds.
 * @param got  What was there instead, shown when it fails.
 */
function check(name: string, passed: boolean, got?: unknown): void {
  if (passed) {
    console.log(`✓ ${name}`);
  } else {
    failed++;
    console.log(`✗ ${name}: got ${JSON.stringify(got, null, 1)}`);
  }
}

/**
 * Runs a command, exiting with its output when it fails.
 *
 * @param command  The program.
 * @param args  Its arguments.
 */
function run(command: string, args: string[]): void {
  const result = spawnSync(command, args, {encoding: 'utf8'});
  if (result.error || result.status !== 0) {
    console.error(
      `${command} ${args.join(' ')} failed:\n${result.error?.message ?? ''}${result.stdout ?? ''}${result.stderr ?? ''}`,
    );
    process.exit(1);
  }
}

/** The editor's end of a session with the adapter. */
class Editor {
  readonly transcript: string[] = [];
  private readonly adapter = spawn(
    process.execPath,
    [join(PACKAGE_DIR, 'cli.mts'), 'debug-adapter'],
    {stdio: ['pipe', 'pipe', 'inherit']},
  );
  private readonly responses = new Map<number, (response: Message) => void>();
  private readonly events: Message[] = [];
  private eventWaiter: (() => void) | null = null;
  private seq = 1;

  constructor() {
    readMessages(this.adapter.stdout, message => {
      this.transcript.push(`<- ${JSON.stringify(message)}`);
      if (message.type === 'response') {
        this.responses.get(message.request_seq)?.(message);
      } else if (message.type === 'event') {
        this.events.push(message);
        this.eventWaiter?.();
      }
    });
  }

  /**
   * Sends a request.
   *
   * @param command  The request.
   * @param args  Its arguments.
   *
   * @returns The response.
   */
  request(command: string, args?: Body): Promise<Message> {
    return new Promise((resolveResponse, reject) => {
      const message: Message = {
        seq: this.seq++,
        type: 'request',
        command,
        arguments: args,
      };
      const timer = setTimeout(
        () => reject(new Error(`no response to ${command}`)),
        WAIT_MS,
      );
      this.responses.set(message.seq, response => {
        clearTimeout(timer);
        resolveResponse(response);
      });
      this.transcript.push(`-> ${JSON.stringify(message)}`);
      writeMessage(this.adapter.stdin, message);
    });
  }

  /**
   * Waits for the next event of a kind, taking it and the ones before it.
   *
   * @param name  The event.
   *
   * @returns The event.
   */
  async event(name: string): Promise<Message> {
    const deadline = Date.now() + WAIT_MS;
    for (;;) {
      const index = this.events.findIndex(event => event.event === name);
      if (index >= 0) {
        return this.events.splice(0, index + 1)[index];
      }
      if (Date.now() > deadline) {
        throw new Error(`no ${name} event`);
      }
      await new Promise<void>(wake => {
        const timer = setTimeout(wake, 1000);
        this.eventWaiter = () => {
          clearTimeout(timer);
          wake();
        };
      });
    }
  }

  /**
   * A thread's call stack.
   *
   * @param threadId  The thread.
   *
   * @returns Its frames, innermost first.
   */
  async stack(threadId: number): Promise<Body[]> {
    const response = await this.request('stackTrace', {threadId});
    return response.body?.stackFrames ?? [];
  }

  /**
   * A frame's variables, by scope.
   *
   * @param frameId  The frame.
   *
   * @returns Each scope's variables as `name = value` lines, by the scope's name.
   */
  async variables(frameId: number): Promise<Record<string, string[]>> {
    const scopes = await this.request('scopes', {frameId});
    const found: Record<string, string[]> = {};
    for (const scope of scopes.body?.scopes ?? []) {
      const response = await this.request('variables', {
        variablesReference: scope.variablesReference,
      });
      found[scope.name] = (response.body?.variables ?? []).map(
        (variable: Body) => `${variable.name} = ${variable.value}`,
      );
    }
    return found;
  }

  end(): void {
    this.adapter.kill();
  }
}

/**
 * Plays the session.
 *
 * @param editor  The editor's end.
 * @param host  The host's program.
 * @param packagePath  The fixture's package.
 * @param debugPath  Its debug file.
 */
async function session(
  editor: Editor,
  host: string,
  packagePath: string,
  debugPath: string,
): Promise<void> {
  const appSource = {name: 'native-calls.tsx', path: FIXTURE};
  await editor.request('initialize', {
    clientID: 'erb-dap',
    adapterID: 'embedded-react',
    linesStartAt1: true,
    columnsStartAt1: true,
  });
  const launched = editor.request('launch', {
    program: host,
    args: [
      join(BUILD_DIR, 'frame.raw'),
      String(SCREEN.width),
      String(SCREEN.height),
      packagePath,
      TAP,
      '0',
    ],
    debugFile: debugPath,
    setupCommands: ['set debuginfod enabled off'],
  });
  await editor.event('initialized');

  const set = await editor.request('setBreakpoints', {
    source: appSource,
    breakpoints: [{line: 34}, {line: 31}],
  });
  const appBreakpoints: Body[] = set.body?.breakpoints ?? [];
  check(
    'a breakpoint in the app is taken, and one on a line with no code moves on',
    appBreakpoints.length === 2 &&
      appBreakpoints[0].verified &&
      appBreakpoints[0].line === 34 &&
      appBreakpoints[1].verified &&
      appBreakpoints[1].line === 32,
    set,
  );
  const kept = await editor.request('setBreakpoints', {
    source: appSource,
    breakpoints: [{line: 34}],
  });
  check(
    'a breakpoint that stays keeps its id',
    kept.body?.breakpoints?.[0]?.id === appBreakpoints[0].id,
    kept,
  );
  const functionSet = await editor.request('setFunctionBreakpoints', {
    breakpoints: [{name: 'probe_next'}],
  });
  check(
    "a breakpoint on a C function is GDB's",
    functionSet.success,
    functionSet,
  );
  await editor.request('configurationDone');
  check('the host launches', (await launched).success, await launched);

  // The handler's first statement calls the probe module.
  let stopped = await editor.event('stopped');
  let frames = await editor.stack(stopped.body.threadId);
  const vmFrame = frames.findIndex(frame =>
    String(frame.name).startsWith(VM_FUNCTION),
  );
  check(
    "a stop in C shows the app's frame inside the VM's",
    frames[0]?.name === 'probe_next' &&
      vmFrame > 1 &&
      frames[vmFrame - 1].name === 'onPress' &&
      frames[vmFrame - 1].line === 32 &&
      frames[vmFrame - 1].source?.path === FIXTURE,
    frames.map(frame => `${frame.name}:${frame.line}`),
  );
  let variables = await editor.variables(frames[0].id);
  check(
    "a C frame has the app's state after its own scopes",
    variables['App state']?.includes('total = 0') &&
      Object.keys(variables).length > 1,
    variables,
  );
  await editor.request('next', {threadId: stopped.body.threadId});
  stopped = await editor.event('stopped');
  frames = await editor.stack(stopped.body.threadId);
  check(
    'the step button steps C at a stop in C',
    stopped.body.reason === 'step' && frames[0]?.name === 'probe_next',
    [stopped.body, frames[0]],
  );

  await editor.request('setFunctionBreakpoints', {breakpoints: []});
  await editor.request('continue', {threadId: stopped.body.threadId});
  stopped = await editor.event('stopped');
  frames = await editor.stack(stopped.body.threadId);
  check(
    "the app's breakpoint stops on its line",
    stopped.body.reason === 'breakpoint' &&
      stopped.body.hitBreakpointIds?.[0] === appBreakpoints[0].id &&
      frames[0]?.name === 'onPress' &&
      frames[0].line === 34 &&
      frames[0].source?.path === FIXTURE &&
      String(frames[1]?.name).startsWith(VM_FUNCTION),
    [stopped.body, frames.slice(0, 2)],
  );
  variables = await editor.variables(frames[0].id);
  check(
    "the app's frame has its locals and the app's state",
    variables.Locals?.includes('step = 1') &&
      variables['App state']?.some(line => line.startsWith('total = ')),
    variables,
  );
  const hover = await editor.request('evaluate', {
    expression: 'step',
    frameId: frames[0].id,
    context: 'hover',
  });
  check('a local evaluates by name', hover.body?.result === '1', hover);

  const steps: [string, string, (stop: Body, top: Body) => boolean][] = [
    [
      'stepIn',
      'step in',
      (stop, top) => stop.reason === 'step' && top.line === 35,
    ],
    [
      'next',
      'step over',
      (stop, top) => stop.reason === 'step' && top.line === 36,
    ],
    [
      'stepOut',
      'step out',
      stop => /returns to the runtime/.test(stop.description ?? ''),
    ],
  ];
  for (const [command, name, expected] of steps) {
    await editor.request(command, {threadId: stopped.body.threadId});
    stopped = await editor.event('stopped');
    frames = await editor.stack(stopped.body.threadId);
    check(`${name} steps the app`, expected(stopped.body, frames[0] ?? {}), [
      stopped.body,
      frames[0],
    ]);
  }

  const scopes = await editor.request('scopes', {frameId: frames[0].id});
  const state = (scopes.body?.scopes ?? []).find(
    (scope: Body) => scope.name === 'App state',
  );
  const written = await editor.request('setVariable', {
    variablesReference: state?.variablesReference,
    name: 'total',
    value: '41',
  });
  check(
    'a state is set from the Variables pane',
    written.body?.value === '41',
    written,
  );

  const console = await editor.request('evaluate', {
    expression: 'erb state total',
    frameId: frames[0].id,
    context: 'repl',
  });
  check(
    "the debug console runs the script's commands",
    /^total = 41$/m.test(console.body?.result ?? ''),
    console,
  );
  const ended = await editor.request('disconnect', {terminateDebuggee: true});
  check('the session ends', ended.success, ended);
}

/**
 * Plays a session that runs to a fault.
 *
 * @param editor  The editor's end.
 * @param host  The host's program.
 * @param packagePath  The fixture's package.
 * @param debugPath  Its debug file.
 */
async function faultSession(
  editor: Editor,
  host: string,
  packagePath: string,
  debugPath: string,
): Promise<void> {
  const initialized = await editor.request('initialize', {
    clientID: 'erb-dap',
    adapterID: 'embedded-react',
    linesStartAt1: true,
    columnsStartAt1: true,
  });
  const filters: Body[] = initialized.body?.exceptionBreakpointFilters ?? [];
  check(
    'the adapter offers a stop on app faults, on by default',
    filters.some(filter => filter.filter === 'erb-faults' && filter.default),
    initialized,
  );
  const launched = editor.request('launch', {
    program: host,
    args: [
      join(BUILD_DIR, 'frame.raw'),
      String(SCREEN.width),
      String(SCREEN.height),
      packagePath,
      TAP,
      '0',
    ],
    debugFile: debugPath,
    // No breakpoint names the source here, and it isn't beside the debug file.
    source: FAULT_FIXTURE,
    setupCommands: ['set debuginfod enabled off'],
  });
  await editor.event('initialized');
  const set = await editor.request('setExceptionBreakpoints', {
    filters: ['erb-faults'],
  });
  check(
    'the fault stop is taken',
    set.body?.breakpoints?.[0]?.verified === true,
    set,
  );
  await editor.request('configurationDone');
  check('the host launches', (await launched).success, await launched);

  const stopped = await editor.event('stopped');
  const frames = await editor.stack(stopped.body.threadId);
  check(
    "a fault stops as an exception on the app's line",
    stopped.body.reason === 'exception' &&
      stopped.body.text === 'list read past its end' &&
      frames[0]?.name === 'onPress' &&
      frames[0].line === 18 &&
      frames[0].source?.path === FAULT_FIXTURE &&
      String(frames[1]?.name).startsWith(VM_FUNCTION),
    [stopped.body, frames.slice(0, 2)],
  );
  const info = await editor.request('exceptionInfo', {
    threadId: stopped.body.threadId,
  });
  check(
    'the exception has the reason and the place',
    info.body?.exceptionId === 'list read past its end' &&
      /list-fault\.tsx:18 {2}onPress/.test(info.body?.description ?? ''),
    info,
  );
  const variables = await editor.variables(frames[0].id);
  check(
    'the handler that faulted has its locals',
    variables.Locals?.includes('row = 5') &&
      variables['App state']?.includes('picked = 0'),
    variables,
  );
  const ended = await editor.request('disconnect', {terminateDebuggee: true});
  check('the session ends', ended.success, ended);
}

/**
 * Plays a session whose package is refused.
 *
 * @param editor  The editor's end.
 * @param host  The host's program.
 * @param packagePath  The fixture's package.
 * @param debugPath  Its debug file.
 */
async function refusedSession(
  editor: Editor,
  host: string,
  packagePath: string,
  debugPath: string,
): Promise<void> {
  const cutPath = join(BUILD_DIR, 'list-fault-cut.erpkg');
  writeFileSync(cutPath, readFileSync(packagePath).subarray(0, CUT_BYTES));
  await editor.request('initialize', {
    clientID: 'erb-dap',
    adapterID: 'embedded-react',
    linesStartAt1: true,
    columnsStartAt1: true,
  });
  const launched = editor.request('launch', {
    program: host,
    args: [
      join(BUILD_DIR, 'frame.raw'),
      String(SCREEN.width),
      String(SCREEN.height),
      cutPath,
      TAP,
      '0',
    ],
    debugFile: debugPath,
    source: FAULT_FIXTURE,
    setupCommands: ['set debuginfod enabled off'],
  });
  await editor.event('initialized');
  await editor.request('setExceptionBreakpoints', {filters: ['erb-faults']});
  await editor.request('configurationDone');
  check('the host launches', (await launched).success, await launched);

  const stopped = await editor.event('stopped');
  const frames = await editor.stack(stopped.body.threadId);
  check(
    'a refused load stops as an exception, with no app frame',
    stopped.body.reason === 'exception' &&
      stopped.body.description === 'Load refused' &&
      stopped.body.text === 'not a package, or corrupt' &&
      frames.some(frame => frame.name === 'erb_load') &&
      !frames.some(frame => frame.source?.path === FAULT_FIXTURE),
    [stopped.body, frames.slice(0, 3)],
  );
  const info = await editor.request('exceptionInfo', {
    threadId: stopped.body.threadId,
  });
  check(
    'the exception has why the load was refused',
    info.body?.exceptionId === 'not a package, or corrupt',
    info,
  );
  const ended = await editor.request('disconnect', {terminateDebuggee: true});
  check('the session ends', ended.success, ended);
}

/**
 * Compiles a fixture into the build folder.
 *
 * @param fixture  The fixture's source file.
 * @param name  The package's name.
 *
 * @returns The package's path and its debug file's.
 */
function compileFixture(fixture: string, name: string): [string, string] {
  const source = readFileSync(fixture, 'utf8');
  const result = compilePackage(source, name, {
    filename: fixture,
    nativeFiles: readNativeFiles(source, dirname(fixture)),
    screen: SCREEN,
  });
  const packagePath = join(BUILD_DIR, `${name}.erpkg`);
  const debugPath = join(BUILD_DIR, `${name}.erdbg`);
  writeFileSync(packagePath, packPackage(result).bytes);
  writeFileSync(debugPath, result.debugFile);
  return [packagePath, debugPath];
}

if (spawnSync('gdb', ['--version']).status !== 0) {
  console.error(
    'erb-dap needs GDB 14 or later with Python (it runs on Linux).',
  );
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
const sessions: [typeof session, string, string][] = [
  [session, FIXTURE, 'native-calls'],
  [faultSession, FAULT_FIXTURE, 'list-fault'],
  [refusedSession, FAULT_FIXTURE, 'list-fault'],
];
for (const [play, fixture, name] of sessions) {
  const [packagePath, debugPath] = compileFixture(fixture, name);
  const editor = new Editor();
  try {
    await play(editor, host, packagePath, debugPath);
  } catch (error: any) {
    failed++;
    console.log(`✗ ${error.message}`);
  }
  editor.end();
  if (failed) {
    console.log(`\nThe whole session:\n${editor.transcript.join('\n')}`);
    process.exit(1);
  }
}
