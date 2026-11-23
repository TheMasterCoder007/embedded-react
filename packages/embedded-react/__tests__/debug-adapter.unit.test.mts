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

import {describe, expect, it} from 'vitest';
import {PassThrough} from 'node:stream';
import {
  DebugAdapter,
  readMessages,
  writeMessage,
  type Message,
} from '../debug-adapter.mts';

const APP_ID = 0x10000000;
const STATE_ID = 0x20000000;
const WRITER_FRAME = 0x0fffffff;
const LOCALS = 0x20000000;
const STATE = 0x30000000;
const APP_PATH = '/work/app/App.tsx';

type Body = Record<string, any>;

/** A stand-in for `gdb -i dap` with gdb/erb.py loaded: it answers from a script and records what it was asked. */
class FakeGdb {
  readonly stdin = new PassThrough();
  readonly stdout = new PassThrough();
  /** Each request GDB got, as `command` or `erb <do>`. */
  readonly asked: string[] = [];
  readonly erbRequests: Body[] = [];
  readonly consoleCommands: string[] = [];
  /** What `erb dap status` answers at the next stop. */
  status: Body = {loaded: true, hookStop: false, frames: [], hit: []};
  frames: Body[] = [];
  /** Whether the firmware has the hook. */
  hook = true;
  /** The exception filters GDB was last given. */
  exceptionFilters: string[] = [];
  private seq = 1;
  private deferred: Message | null = null;
  /** GDB can't list a running board's threads. */
  private running = false;

  constructor() {
    readMessages(this.stdin, request => this.onRequest(request));
  }

  event(event: string, body?: Body): void {
    writeMessage(this.stdout, {seq: this.seq++, type: 'event', event, body});
  }

  private respond(request: Message, body?: Body, message?: string): void {
    writeMessage(this.stdout, {
      seq: this.seq++,
      type: 'response',
      request_seq: request.seq,
      command: request.command,
      success: message === undefined,
      message,
      body,
    });
  }

  private erb(request: Body): Body {
    this.asked.push(`erb ${request.do}`);
    this.erbRequests.push(request);
    switch (request.do) {
      case 'info':
        return {file: 'App.tsx', source: '', hook: true};
      case 'status':
        return this.status;
      case 'breakpoints':
        return {
          breakpoints: request.breakpoints.map(
            ({line, condition}: Body, index: number) =>
              line === 99
                ? {verified: false, message: 'App.tsx:99 has no code'}
                : condition === 'count ='
                  ? {
                      line,
                      verified: false,
                      message: '`count =` has an assignment',
                    }
                  : {
                      id: index + 1,
                      line: line === 30 ? 32 : line,
                      verified: true,
                    },
          ),
        };
      case 'variables':
        if (request.scope === 'state') {
          return {
            variables: [
              {name: 'total', value: '4', type: 'int', storage: 'total'},
              {
                name: 'rows',
                value: '[…]',
                type: 'list',
                storage: 's3',
                rows: 2,
              },
            ],
          };
        }
        if (request.scope === 'rows') {
          return {
            variables: [{name: '[0]', value: request.state, type: 'row'}],
          };
        }
        return {variables: [{name: 'step', value: '1', type: 'int'}]};
      case 'faults':
        return this.hook
          ? {verified: true}
          : {verified: false, message: 'the firmware has no breakpoint hook'};
      case 'set':
        return {value: `${request.state}=${request.value}`};
      case 'watch':
        return {
          breakpoints: request.states.map((state: string, index: number) =>
            index < 2
              ? {id: index + 1, verified: true}
              : {
                  verified: false,
                  message:
                    'the chip has no hardware watchpoint left: all 2 are in use',
                },
          ),
        };
      case 'evaluate':
        switch (request.expression) {
          case 'step':
            return {value: '1', type: 'number'};
          case 'total + 1':
            return {value: '5', type: 'number'};
          case 'rows':
            return {value: '[…]', type: 'list', rows: 2, storage: 's3'};
          default:
            return {error: `nothing here is named ${request.expression}`};
        }
      default:
        return {};
    }
  }

  private onRequest(request: Message): void {
    const args: Body = request.arguments ?? {};
    if (request.command === 'evaluate' && args.context === 'repl') {
      const encoded = /^erb dap (.+)$/.exec(args.expression);
      if (!encoded) {
        this.consoleCommands.push(args.expression);
        return this.respond(request, {result: '', variablesReference: 0});
      }
      const answer = this.erb(
        JSON.parse(Buffer.from(encoded[1], 'base64').toString('utf8')),
      );
      return this.respond(request, {
        result: `Note: a line a command printed\n@erb ${JSON.stringify(answer)}\n`,
        variablesReference: 0,
      });
    }
    this.asked.push(request.command);
    switch (request.command) {
      case 'initialize':
        this.respond(request, {
          supportsDisassembleRequest: true,
          exceptionBreakpointFilters: [{filter: 'throw', label: 'C++ throw'}],
        });
        return this.event('initialized');
      case 'launch':
      case 'attach':
        this.deferred = request;
        return;
      case 'configurationDone':
        this.respond(request);
        if (this.deferred?.command === 'attach') {
          this.event('stopped', {reason: 'attach', threadId: 1});
        }
        return this.respond(this.deferred!);
      case 'threads':
        return this.running
          ? this.respond(request, undefined, 'Cannot execute this command')
          : this.respond(request, {threads: [{id: 1, name: 'main'}]});
      case 'continue':
        this.running = true;
        return this.respond(request, {});
      case 'pause':
        this.running = false;
        this.respond(request);
        return this.event('stopped', {reason: 'pause', threadId: 1});
      case 'stackTrace':
        return this.respond(request, {stackFrames: this.frames});
      case 'scopes':
        return this.respond(request, {
          scopes: [{name: 'Locals', variablesReference: 7}],
        });
      case 'setBreakpoints':
        return this.respond(request, {breakpoints: [{id: 4, verified: true}]});
      case 'evaluate':
        return this.respond(
          request,
          undefined,
          'No symbol in current context.',
        );
      case 'setExceptionBreakpoints':
        this.exceptionFilters = args.filters;
        return this.respond(request, {
          breakpoints: args.filters.map(() => ({id: 9, verified: true})),
        });
      default:
        return this.respond(request, {});
    }
  }
}

/** The editor's end of a session. */
class Editor {
  readonly gdb = new FakeGdb();
  readonly events: Message[] = [];
  private readonly input = new PassThrough();
  private readonly output = new PassThrough();
  private readonly responses = new Map<number, (response: Message) => void>();
  private readonly eventWaiters: {name: string; take(event: Message): void}[] =
    [];
  private seq = 1;

  constructor() {
    new DebugAdapter({
      input: this.input,
      output: this.output,
      script: '/pkg/gdb/erb.py',
      startGdb: () => ({
        stdin: this.gdb.stdin,
        stdout: this.gdb.stdout,
        onExit: () => {},
        kill: () => {},
      }),
    });
    readMessages(this.output, message => {
      if (message.type === 'response') {
        this.responses.get(message.request_seq)?.(message);
        return;
      }
      this.events.push(message);
      const waiter = this.eventWaiters.findIndex(
        candidate => candidate.name === message.event,
      );
      if (waiter >= 0) {
        this.eventWaiters.splice(waiter, 1)[0].take(message);
      }
    });
  }

  request(command: string, args?: Body): Promise<Message> {
    return new Promise(resolve => {
      const seq = this.seq++;
      this.responses.set(seq, resolve);
      writeMessage(this.input, {
        seq,
        type: 'request',
        command,
        arguments: args,
      });
    });
  }

  nextEvent(name: string): Promise<Message> {
    return new Promise(take => this.eventWaiters.push({name, take}));
  }

  /** Starts a session and runs it to where the board runs. */
  async start(kind: 'launch' | 'attach', config: Body = {}): Promise<void> {
    await this.request('initialize', {linesStartAt1: true});
    const initialized = this.nextEvent('initialized');
    const started = this.request(kind, {
      program: '/work/build/my firmware.elf',
      debugFile: '/work/app/dist/app.erdbg',
      ...config,
    });
    await initialized;
    await this.request('setBreakpoints', {
      source: {path: APP_PATH},
      breakpoints: [{line: 30}, {line: 99}],
    });
    await this.request('configurationDone');
    expect((await started).success).toBe(true);
    this.gdb.asked.length = 0;
  }

  /** Stops the board in a handler, at the hook. */
  async stopInApp(hit: number[] = [1]): Promise<Message> {
    this.gdb.status = {
      loaded: true,
      hookStop: true,
      frames: [
        {name: 'onPress', line: 32},
        {name: 'update', line: null},
      ],
      hit,
    };
    this.gdb.frames = [
      {id: 1, name: 'erb_debug_break', line: 874},
      {id: 2, name: 'erb_vm_run [App.tsx:32  onPress]', line: 914},
      {id: 3, name: 'main', line: 10},
    ];
    const stopped = this.nextEvent('stopped');
    this.gdb.event('stopped', {
      reason: 'breakpoint',
      threadId: 1,
      hitBreakpointIds: [-1],
    });
    return stopped;
  }
}

describe('debug adapter', () => {
  it('loads the ELF, the script and the debug file before the editor sends breakpoints', async () => {
    const editor = new Editor();
    await editor.request('initialize', {});
    const initialized = editor.nextEvent('initialized');
    const launched = editor.request('launch', {
      program: '/work/build/my firmware.elf',
      debugFile: '/work/app/dist/app.erdbg',
      gdb: 'arm-none-eabi-gdb',
      setupCommands: ['set mem inaccessible-by-default off'],
    });
    await initialized;
    expect(editor.gdb.consoleCommands).toEqual([
      'set mem inaccessible-by-default off',
      'file /work/build/my\\ firmware.elf',
      'source /pkg/gdb/erb.py',
      'erb file /work/app/dist/app.erdbg',
    ]);
    expect(editor.events.map(event => event.event)).toEqual([
      'capabilities',
      'initialized',
    ]);
    expect(editor.events[0].body.capabilities).toMatchObject({
      supportsDisassembleRequest: true,
      supportsExceptionInfoRequest: true,
      exceptionBreakpointFilters: [
        {filter: 'erb-faults', label: 'Stop on app faults', default: true},
        {filter: 'throw', label: 'C++ throw'},
      ],
    });
    await editor.request('configurationDone');
    expect((await launched).success).toBe(true);
  });

  it('refuses a configuration with no debug file', async () => {
    const editor = new Editor();
    const response = await editor.request('launch', {program: 'firmware.elf'});
    expect(response.success).toBe(false);
    expect(response.message).toMatch(/debugFile/);
  });

  it("sends the app's breakpoints to the hook and the rest to GDB", async () => {
    const editor = new Editor();
    await editor.start('launch');
    await editor.stopInApp();
    const app = await editor.request('setBreakpoints', {
      source: {path: APP_PATH},
      breakpoints: [{line: 30}, {line: 99}],
    });
    expect(editor.gdb.erbRequests.at(-1)).toEqual({
      do: 'breakpoints',
      breakpoints: [{line: 30}, {line: 99}],
    });
    expect(app.body.breakpoints).toEqual([
      {id: APP_ID + 1, verified: true, line: 32, source: {path: APP_PATH}},
      {
        verified: false,
        message: 'App.tsx:99 has no code',
        source: {path: APP_PATH},
      },
    ]);
    editor.gdb.asked.length = 0;
    const c = await editor.request('setBreakpoints', {
      source: {path: '/work/firmware/main.c'},
      breakpoints: [{line: 12}],
    });
    expect(c.body.breakpoints).toEqual([{id: 4, verified: true}]);
    expect(editor.gdb.asked).toEqual(['setBreakpoints']);
  });

  it("sends an app breakpoint's condition, hit count and log message to the script", async () => {
    const editor = new Editor();
    await editor.start('launch');
    await editor.stopInApp();
    const response = await editor.request('setBreakpoints', {
      source: {path: APP_PATH},
      breakpoints: [
        {line: 40, condition: 'count === 5', hitCondition: '%2'},
        {line: 41, condition: 'count ='},
        {line: 42, logMessage: 'pressed {count}'},
      ],
    });
    expect(editor.gdb.erbRequests.at(-1)).toEqual({
      do: 'breakpoints',
      breakpoints: [
        {line: 40, condition: 'count === 5', hitCondition: '%2'},
        {line: 41, condition: 'count ='},
        {line: 42, logMessage: 'pressed {count}'},
      ],
    });
    expect(
      response.body.breakpoints.map((entry: Body) => entry.verified),
    ).toEqual([true, false, true]);
    // The script says what is wrong with a condition it can't read.
    expect(response.body.breakpoints[1].message).toMatch(/assignment/);
    expect(response.body.breakpoints[1].line).toBe(41);
    expect(response.body.breakpoints[2].line).toBe(42);
  });

  it("halts a running board to set the app's breakpoints, without a stop in the editor", async () => {
    const editor = new Editor();
    await editor.start('launch');
    await editor.request('setBreakpoints', {
      source: {path: APP_PATH},
      breakpoints: [{line: 40}],
    });
    expect(editor.gdb.asked).toEqual(['pause', 'erb breakpoints', 'continue']);
    expect(editor.events.some(event => event.event === 'stopped')).toBe(false);
  });

  it("shows a stop of the hook's as a stop in the app", async () => {
    const editor = new Editor();
    await editor.start('launch');
    const stopped = await editor.stopInApp();
    expect(stopped.body).toEqual({
      reason: 'breakpoint',
      threadId: 1,
      hitBreakpointIds: [APP_ID + 1],
    });
    // The hook's own breakpoint stays out of the editor's list.
    editor.gdb.event('breakpoint', {reason: 'new', breakpoint: {id: -1}});
    editor.gdb.event('breakpoint', {reason: 'new', breakpoint: {id: 4}});
    expect((await editor.nextEvent('breakpoint')).body.breakpoint.id).toBe(4);
    const stack = await editor.request('stackTrace', {threadId: 1});
    expect(stack.body.totalFrames).toBe(4);
    expect(stack.body.stackFrames.map((frame: Body) => frame.name)).toEqual([
      'onPress',
      'update',
      'erb_vm_run [App.tsx:32  onPress]',
      'main',
    ]);
    expect(stack.body.stackFrames[0]).toMatchObject({
      id: APP_ID,
      line: 32,
      source: {name: 'App.tsx', path: APP_PATH},
    });
    expect(stack.body.stackFrames[1].source).toBeUndefined();
    const paged = await editor.request('stackTrace', {
      threadId: 1,
      startFrame: 1,
      levels: 2,
    });
    expect(paged.body.stackFrames.map((frame: Body) => frame.id)).toEqual([
      APP_ID + 1,
      2,
    ]);
  });

  it("shows a fault as an exception on the app's line", async () => {
    const editor = new Editor();
    await editor.start('launch');
    editor.gdb.status = {
      loaded: true,
      hookStop: true,
      frames: [{name: 'onPress', line: 32}],
      hit: [],
      fault: {
        reason: 'list read past its end',
        text: 'list read past its end\n  at App.tsx:32  onPress',
        refused: false,
      },
    };
    editor.gdb.frames = [
      {id: 1, name: 'erb_debug_break', line: 874},
      {id: 2, name: 'erb_rt_fault', line: 460},
      {id: 3, name: 'erb_vm_run [App.tsx:32  onPress]', line: 1272},
      {id: 4, name: 'main', line: 10},
    ];
    const stopped = editor.nextEvent('stopped');
    editor.gdb.event('stopped', {
      reason: 'breakpoint',
      threadId: 1,
      hitBreakpointIds: [-1],
    });
    expect((await stopped).body).toEqual({
      reason: 'exception',
      description: 'App fault',
      text: 'list read past its end',
      threadId: 1,
    });
    const info = await editor.request('exceptionInfo', {threadId: 1});
    expect(info.body).toEqual({
      exceptionId: 'list read past its end',
      description: 'list read past its end\n  at App.tsx:32  onPress',
      breakMode: 'always',
    });
    const stack = await editor.request('stackTrace', {threadId: 1});
    expect(stack.body.stackFrames.map((frame: Body) => frame.name)).toEqual([
      'onPress',
      'erb_vm_run [App.tsx:32  onPress]',
      'main',
    ]);
    const locals = await editor.request('variables', {
      variablesReference: LOCALS,
    });
    expect(locals.body.variables[0]).toMatchObject({name: 'step', value: '1'});
    // The app has no next statement, so the step button runs the firmware on.
    editor.gdb.asked.length = 0;
    await editor.request('next', {threadId: 1});
    expect(editor.gdb.asked).toEqual(['next']);
    // The next stop is not the fault's.
    const paused = editor.nextEvent('stopped');
    editor.gdb.status = {loaded: false, note: 'no app is running'};
    editor.gdb.event('stopped', {reason: 'pause', threadId: 1});
    expect((await paused).body.reason).toBe('pause');
    editor.gdb.asked.length = 0;
    await editor.request('exceptionInfo', {threadId: 1});
    expect(editor.gdb.asked).toEqual(['exceptionInfo']);
  });

  it('shows a refused load with no app running as an exception', async () => {
    const editor = new Editor();
    await editor.start('launch');
    editor.gdb.status = {
      loaded: false,
      note: 'no app is running',
      fault: {
        reason: 'needs more than the firmware reserves',
        text: 'needs more than the firmware reserves\n  arena: needs 57552, the firmware reserves 32768',
        refused: true,
      },
    };
    const stopped = editor.nextEvent('stopped');
    editor.gdb.event('stopped', {reason: 'breakpoint', threadId: 1});
    expect((await stopped).body).toEqual({
      reason: 'exception',
      description: 'Load refused',
      text: 'needs more than the firmware reserves',
      threadId: 1,
    });
  });

  it('takes the fault checkbox itself and hands GDB its own filters', async () => {
    const editor = new Editor();
    await editor.start('launch');
    const set = await editor.request('setExceptionBreakpoints', {
      filters: ['throw', 'erb-faults'],
    });
    expect(editor.gdb.erbRequests.at(-1)).toEqual({do: 'faults', stop: true});
    expect(editor.gdb.exceptionFilters).toEqual(['throw']);
    expect(set.body.breakpoints).toEqual([
      {id: 9, verified: true},
      {verified: true},
    ]);
    await editor.request('setExceptionBreakpoints', {filters: []});
    expect(editor.gdb.erbRequests.at(-1)).toEqual({do: 'faults', stop: false});
    // A firmware without the hook can't stop at a fault, and the checkbox says so.
    editor.gdb.hook = false;
    const refused = await editor.request('setExceptionBreakpoints', {
      filters: ['erb-faults'],
    });
    expect(refused.body.breakpoints).toEqual([
      {verified: false, message: 'the firmware has no breakpoint hook'},
    ]);
  });

  it('keeps what the app called above its frames at a stop in C', async () => {
    const editor = new Editor();
    await editor.start('launch');
    editor.gdb.status = {
      loaded: true,
      hookStop: false,
      frames: [{name: 'onPress', line: 32}],
      hit: [],
    };
    editor.gdb.frames = [
      {id: 1, name: 'probe_next', line: 5},
      {id: 2, name: 'erb_vm_run', line: 1200},
      {id: 3, name: 'main', line: 10},
    ];
    const stopped = editor.nextEvent('stopped');
    editor.gdb.event('stopped', {
      reason: 'breakpoint',
      threadId: 1,
      hitBreakpointIds: [4],
    });
    expect((await stopped).body.hitBreakpointIds).toEqual([4]);
    const stack = await editor.request('stackTrace', {threadId: 1});
    expect(stack.body.stackFrames.map((frame: Body) => frame.name)).toEqual([
      'probe_next',
      'onPress',
      'erb_vm_run',
      'main',
    ]);
    // The editor's buttons step C here.
    editor.gdb.asked.length = 0;
    await editor.request('next', {threadId: 1});
    expect(editor.gdb.asked).toEqual(['next']);
  });

  it("steps the app with the editor's buttons at a stop in the app", async () => {
    const editor = new Editor();
    await editor.start('launch');
    await editor.stopInApp();
    editor.gdb.asked.length = 0;
    expect((await editor.request('next', {threadId: 1})).success).toBe(true);
    expect(editor.gdb.asked).toEqual(['erb step', 'continue']);
    expect(editor.gdb.erbRequests.at(-1)).toEqual({do: 'step', kind: 'over'});
    const stopped = await editor.stopInApp([]);
    expect(stopped.body).toEqual({reason: 'step', threadId: 1});
    await editor.request('stepIn', {threadId: 1});
    await editor.stopInApp();
    await editor.request('stepOut', {threadId: 1});
    expect(
      editor.gdb.erbRequests
        .filter(request => request.do === 'step')
        .map(request => request.kind),
    ).toEqual(['over', 'into', 'out']);
    // An instruction step is GDB's.
    await editor.stopInApp();
    editor.gdb.asked.length = 0;
    await editor.request('next', {threadId: 1, granularity: 'instruction'});
    expect(editor.gdb.asked).toEqual(['next']);
  });

  it("answers an app frame's scopes and variables from the app", async () => {
    const editor = new Editor();
    await editor.start('launch');
    await editor.stopInApp();
    const scopes = await editor.request('scopes', {frameId: APP_ID});
    expect(
      scopes.body.scopes.map((scope: Body) => scope.variablesReference),
    ).toEqual([LOCALS, STATE]);
    const locals = await editor.request('variables', {
      variablesReference: LOCALS,
    });
    expect(locals.body.variables).toEqual([
      {name: 'step', value: '1', type: 'int', variablesReference: 0},
    ]);
    const state = await editor.request('variables', {
      variablesReference: STATE,
    });
    expect(state.body.variables[1]).toMatchObject({
      name: 'rows',
      indexedVariables: 2,
    });
    const rows = await editor.request('variables', {
      variablesReference: state.body.variables[1].variablesReference,
    });
    expect(rows.body.variables[0].value).toBe('s3');
    const set = await editor.request('setVariable', {
      variablesReference: STATE,
      name: 'rows',
      value: '5',
    });
    expect(set.body.value).toBe('s3=5');
    // A C frame keeps GDB's scopes, with the app's state after them.
    const cScopes = await editor.request('scopes', {frameId: 3});
    expect(cScopes.body.scopes.map((scope: Body) => scope.name)).toEqual([
      'Locals',
      'App state',
    ]);
  });

  it('evaluates an expression in an app frame, and hands the debug console to GDB', async () => {
    const editor = new Editor();
    await editor.start('launch');
    await editor.stopInApp();
    const hover = await editor.request('evaluate', {
      expression: 'step',
      frameId: APP_ID,
      context: 'hover',
    });
    expect(hover.body.result).toBe('1');
    const watch = await editor.request('evaluate', {
      expression: ' total + 1 ',
      frameId: APP_ID + 1,
      context: 'watch',
    });
    expect(watch.body.result).toBe('5');
    expect(editor.gdb.erbRequests.at(-1)).toEqual({
      do: 'evaluate',
      expression: 'total + 1',
      frame: 1,
    });
    const unknown = await editor.request('evaluate', {
      expression: 'styles.root',
      frameId: APP_ID,
      context: 'hover',
    });
    expect(unknown.success).toBe(false);
    expect(unknown.message).toMatch(/nothing here is named/);
    await editor.request('evaluate', {
      expression: 'erb pools',
      frameId: APP_ID,
      context: 'repl',
    });
    expect(editor.gdb.consoleCommands.at(-1)).toBe('erb pools');
  });

  it("opens a list an expression names to the list's rows", async () => {
    const editor = new Editor();
    await editor.start('launch');
    await editor.stopInApp();
    const list = await editor.request('evaluate', {
      expression: 'rows',
      frameId: APP_ID,
      context: 'watch',
    });
    expect(list.body.indexedVariables).toBe(2);
    const rows = await editor.request('variables', {
      variablesReference: list.body.variablesReference,
    });
    expect(rows.body.variables).toEqual([
      {name: '[0]', value: 's3', type: 'row', variablesReference: 0},
    ]);
  });

  it("reads `erb print` in the debug console from the editor's selected frame", async () => {
    const editor = new Editor();
    await editor.start('launch');
    await editor.stopInApp();
    const printed = await editor.request('evaluate', {
      expression: 'erb print total + 1',
      frameId: APP_ID + 1,
      context: 'repl',
    });
    expect(printed.body.result).toBe('5');
    expect(editor.gdb.erbRequests.at(-1)).toEqual({
      do: 'evaluate',
      expression: 'total + 1',
      frame: 1,
    });
  });

  it("watches the app's state from a C frame, where GDB has no such name", async () => {
    const editor = new Editor();
    await editor.start('launch');
    await editor.stopInApp();
    const watch = await editor.request('evaluate', {
      expression: 'total + 1',
      frameId: 3,
      context: 'watch',
    });
    expect(watch.body.result).toBe('5');
    expect(editor.gdb.erbRequests.at(-1)).toEqual({
      do: 'evaluate',
      expression: 'total + 1',
      frame: -1,
    });
    // What neither reads keeps GDB's error, and a hover in C stays GDB's.
    const neither = await editor.request('evaluate', {
      expression: 'buffer->length',
      frameId: 3,
      context: 'watch',
    });
    expect(neither.message).toMatch(/No symbol/);
    const hover = await editor.request('evaluate', {
      expression: 'total + 1',
      frameId: 3,
      context: 'hover',
    });
    expect(hover.success).toBe(false);
  });

  it("offers a data breakpoint on a state, and on nothing of GDB's", async () => {
    const editor = new Editor();
    await editor.start('launch');
    await editor.stopInApp();
    await editor.request('variables', {variablesReference: STATE});
    const state = await editor.request('dataBreakpointInfo', {
      variablesReference: STATE,
      name: 'total',
    });
    expect(state.body).toEqual({
      dataId: 'erb:total',
      description: 'total',
      accessTypes: ['write'],
      canPersist: false,
    });
    const list = await editor.request('dataBreakpointInfo', {
      variablesReference: STATE,
      name: 'rows',
    });
    expect(list.body.dataId).toBe('erb:s3');
    expect(list.body.description).toBe('rows (rows added or removed)');
    for (const variablesReference of [LOCALS, 7]) {
      const other = await editor.request('dataBreakpointInfo', {
        variablesReference,
        name: 'step',
      });
      expect(other.body.dataId).toBeNull();
    }
    expect(editor.gdb.asked).not.toContain('dataBreakpointInfo');
  });

  it('sets data breakpoints as the script watches a state, and says which the chip has no watchpoint for', async () => {
    const editor = new Editor();
    await editor.start('launch');
    // None to set and none to remove: the board runs on.
    editor.gdb.asked.length = 0;
    await editor.request('setDataBreakpoints', {breakpoints: []});
    expect(editor.gdb.asked).toEqual([]);

    const set = await editor.request('setDataBreakpoints', {
      breakpoints: [
        {dataId: 'erb:total'},
        {dataId: 'erb:s3'},
        {dataId: 'erb:on'},
      ],
    });
    expect(editor.gdb.asked).toEqual(['pause', 'erb watch', 'continue']);
    expect(editor.gdb.erbRequests.at(-1)).toEqual({
      do: 'watch',
      states: ['total', 's3', 'on'],
    });
    expect(set.body.breakpoints).toEqual([
      {id: STATE_ID + 1, verified: true},
      {id: STATE_ID + 2, verified: true},
      {
        verified: false,
        message: 'the chip has no hardware watchpoint left: all 2 are in use',
      },
    ]);
    expect(editor.events.some(event => event.event === 'stopped')).toBe(false);

    // The last ones are removed by the script too.
    editor.gdb.asked.length = 0;
    await editor.request('setDataBreakpoints', {breakpoints: []});
    expect(editor.gdb.asked).toEqual(['pause', 'erb watch', 'continue']);
  });

  it("shows a state's change on the line that wrote it, with the old and new value", async () => {
    const editor = new Editor();
    await editor.start('launch');
    editor.gdb.status = {
      loaded: true,
      hookStop: false,
      frames: [{name: 'onPress', line: 32}],
      hit: [],
      watch: {
        id: 1,
        label: 'total',
        previous: '4',
        value: '5',
        where: 'App.tsx:32  onPress',
        line: 32,
        name: 'onPress',
        live: true,
      },
    };
    editor.gdb.frames = [
      {id: 1, name: 'erb_store', line: 5},
      {id: 2, name: 'erb_vm_run', line: 1200},
      {id: 3, name: 'main', line: 10},
    ];
    const stopped = editor.nextEvent('stopped');
    editor.gdb.event('stopped', {
      reason: 'data breakpoint',
      threadId: 1,
      hitBreakpointIds: [-3],
    });
    expect((await stopped).body).toEqual({
      reason: 'data breakpoint',
      description: 'total: 4 → 5',
      text: 'Written at App.tsx:32  onPress',
      threadId: 1,
      hitBreakpointIds: [STATE_ID + 1],
    });
    const stack = await editor.request('stackTrace', {threadId: 1});
    expect(
      stack.body.stackFrames.map(
        (frame: Body) => `${frame.name}:${frame.line}`,
      ),
    ).toEqual(['onPress:32', 'erb_vm_run:1200', 'main:10']);
    // The app is still on that statement, so the editor's buttons step it.
    editor.gdb.asked.length = 0;
    await editor.request('next', {threadId: 1});
    expect(editor.gdb.asked).toEqual(['erb step', 'continue']);
  });

  it('gives the line that wrote a state its own frame once the app has moved on', async () => {
    const editor = new Editor();
    await editor.start('launch');
    editor.gdb.status = {
      loaded: true,
      hookStop: false,
      frames: [],
      hit: [],
      watch: {
        id: null,
        label: 'label',
        previous: '"a"',
        value: '"b"',
        where: 'App.tsx:33  onPress',
        line: 33,
        name: 'onPress',
        live: false,
      },
    };
    editor.gdb.frames = [
      {id: 1, name: 'erb_tick', line: 40},
      {id: 2, name: 'main', line: 10},
    ];
    const stopped = editor.nextEvent('stopped');
    editor.gdb.event('stopped', {reason: 'data breakpoint', threadId: 1});
    expect((await stopped).body.description).toBe('label: "a" → "b"');
    // `erb watch` set this one, so it is none of the editor's breakpoints.
    expect((await stopped).body.hitBreakpointIds).toBeUndefined();
    const stack = await editor.request('stackTrace', {threadId: 1});
    expect(stack.body.stackFrames[0]).toEqual({
      id: WRITER_FRAME,
      name: 'onPress (wrote label)',
      line: 33,
      column: 0,
      source: {name: 'App.tsx', path: APP_PATH},
    });
    expect(
      stack.body.stackFrames.slice(1).map((frame: Body) => frame.name),
    ).toEqual(['erb_tick', 'main']);
    const scopes = await editor.request('scopes', {frameId: WRITER_FRAME});
    expect(scopes.body.scopes.map((scope: Body) => scope.name)).toEqual([
      'App state',
    ]);
    const hover = await editor.request('evaluate', {
      expression: 'total + 1',
      frameId: WRITER_FRAME,
      context: 'hover',
    });
    expect(hover.body.result).toBe('5');
    expect(editor.gdb.erbRequests.at(-1)?.frame).toBe(-1);
    // The buttons step C from here.
    editor.gdb.asked.length = 0;
    await editor.request('next', {threadId: 1});
    expect(editor.gdb.asked).toEqual(['next']);
  });

  it('runs the board on after attaching, with its breakpoints written', async () => {
    const editor = new Editor();
    await editor.request('initialize', {});
    const initialized = editor.nextEvent('initialized');
    const attached = editor.request('attach', {
      program: 'firmware.elf',
      debugFile: 'dist/app.erdbg',
      target: 'localhost:3333',
      postAttachCommands: ['monitor reset halt'],
      disconnectCommands: ['monitor resume'],
    });
    await initialized;
    // An editor lists the threads as soon as it has sent configurationDone.
    const done = editor.request('configurationDone');
    const threads = editor.request('threads');
    await done;
    expect((await attached).success).toBe(true);
    expect(editor.gdb.asked.slice(-5)).toEqual([
      'attach',
      'configurationDone',
      'erb sync',
      'threads',
      'continue',
    ]);
    // The board runs by now, so the list is the one taken while it was halted.
    expect((await threads).body.threads).toEqual([{id: 1, name: 'main'}]);
    expect(editor.gdb.consoleCommands.at(-1)).toBe('monitor reset halt');
    expect(editor.events.some(event => event.event === 'stopped')).toBe(false);

    editor.gdb.asked.length = 0;
    await editor.request('disconnect');
    expect(editor.gdb.asked).toEqual([
      'pause',
      'erb breakpoints',
      'disconnect',
    ]);
    expect(editor.gdb.consoleCommands.slice(-2)).toEqual([
      'monitor resume',
      'disconnect',
    ]);
  });

  it('stays halted after attaching when the configuration asks', async () => {
    const editor = new Editor();
    await editor.request('initialize', {});
    const initialized = editor.nextEvent('initialized');
    const stopped = editor.nextEvent('stopped');
    const attached = editor.request('attach', {
      debugFile: 'dist/app.erdbg',
      target: 'localhost:3333',
      stopOnAttach: true,
    });
    await initialized;
    await editor.request('configurationDone');
    await attached;
    expect((await stopped).body.reason).toBe('attach');
    expect(editor.gdb.asked).not.toContain('continue');
  });
});
