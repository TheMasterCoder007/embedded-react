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

// debug-adapter — debugs a Flow B app from an editor (`embedded-react debug-adapter`).
//
// It speaks the Debug Adapter Protocol on stdin and stdout, and sits between the editor and GDB, which it runs with
// GDB's own adapter (`gdb -i dap`, GDB 14 or later with Python) and gdb/erb.py loaded. What is about C passes through
// as it is. What is about the app is answered from the script's `erb dap` command:
//
//   - a breakpoint in the app's source file goes to the VM's hook, as `erb break` does, with its condition, hit count
//     and log message
//   - a stop the hook made shows the app's line, and step in, over and out step the app
//   - the call stack has the app's frames inside the VM's frame
//   - an app frame's variables are its locals and the app's state: a list, a row and an object open to what they
//     hold, and a state, a row's field and a local can be set
//   - hover and Watch evaluate an expression over them, as `erb print` does
//   - a data breakpoint on a state stops where the app changes it, as `erb watch` does
//   - the `embedded-react.nodes` request, which is the adapter's own, answers the node tree as `er nodes` prints it,
//     with each node's props, for an editor to show, and `embedded-react.outline` outlines a node on the halted
//     board's panel
//
// The app's commands only work on a halted board, so one that arrives while it runs halts it and resumes it.
//
// With "simulator" in the configuration there is no board and no GDB: the Flow B simulator (`embedded-react dev
// --aot`) answers the same requests about the app from its own memory, and the editor's side is unchanged.

import {spawn} from 'node:child_process';
import {rmSync} from 'node:fs';
import {connect} from 'node:net';
import {posix} from 'node:path';
import {fileURLToPath} from 'node:url';
import type {Readable, Writable} from 'node:stream';
import {SimulatorLink} from './aot-sim/adapter-link.mts';
import {startGuard, type Guard} from './debug-guard.mts';
import {flashStampPath} from './flash.mts';
import {checkGdb, type CheckedGdb} from './gdb-check.mts';

/** A protocol message: a request, a response or an event. */
export interface Message {
  seq: number;
  type: 'request' | 'response' | 'event';
  [field: string]: any;
}

/** GDB as the adapter drives it. */
export interface GdbProcess {
  stdin: Writable;
  stdout: Readable;
  /** Calls the listener once when GDB ends, with what it wrote to stderr. */
  onExit(listener: (errors: string) => void): void;
  /** Ends GDB: at once when `now`, as GDB puts off a request to end while it waits on a board. */
  kill(now?: boolean): void;
}

type StartGdb = (
  command: string,
  cwd: string | undefined,
  environment: NodeJS.ProcessEnv,
) => GdbProcess;

type CheckGdb = (
  command: string,
  cwd: string | undefined,
  environment: NodeJS.ProcessEnv,
) => Promise<CheckedGdb>;

export interface AdapterOptions {
  input: Readable;
  output: Writable;
  /** Starts GDB. Tests pass a stand-in. */
  startGdb?: StartGdb;
  /** Checks GDB before it starts. Left out with a stand-in GDB, nothing is checked. */
  checkGdb?: CheckGdb;
  /** Whether a board session runs the guard that resumes the board if the adapter is killed. Off with a stand-in GDB. */
  guard?: boolean;
  /** gdb/erb.py, when it isn't beside this file. */
  script?: string;
  /** The CLI a launch starts the simulator with, when it isn't beside this file. */
  cli?: string;
}

/** What `erb dap status` says of a stop. */
interface Status {
  loaded: boolean;
  note?: string;
  hookStop: boolean;
  /** Each frame's file, by its index in the debug file's files, and its line there; null for one with no line. */
  frames: {name: string; file: number | null; line: number | null}[];
  hit: number[];
  description?: string;
  fault?: Fault | null;
  watch?: Change | null;
  /** Why the debug file isn't the one of the app the board runs, when it isn't. */
  mismatch?: string | null;
}

/** A watched state's change the board stopped at. */
interface Change {
  /** The data breakpoint's number, or null for one `erb watch` set. */
  id: number | null;
  label: string;
  previous: string;
  value: string;
  /** The place that wrote it, as `erb watch` prints it. */
  where: string;
  /** The file and line that wrote it, or null when the runtime did. */
  file: number | null;
  line: number | null;
  name: string | null;
  /** Whether the app is still on the statement that wrote it. */
  live: boolean;
}

/** A fault or a refused load the board stopped at. */
interface Fault {
  reason: string;
  /** What `erb error` prints. */
  text: string;
  refused: boolean;
}

/** What a variables reference opens. */
interface Opened {
  /** Where it is, as the script names it: a scope or an expression, and a path of names and row numbers. */
  where: Record<string, any>;
  /** The states in it, by the name the Variables pane shows: each one's storage name, and whether it is a list. */
  states: Map<string, {storage: string; list: boolean}>;
}

/** A node of the tree, as the script answers it. */
interface ScriptNode {
  tag: number;
  type: string;
  rect: [number, number, number, number];
  file: number | null;
  line: number | null;
  column: number | null;
  uses: [number, number, number][];
  props: [string, string][];
  children: ScriptNode[];
}

/** A node of the tree, as an editor gets it. */
export interface TreeNode {
  /** The engine's tag for the node, which a later stop may give to another. */
  id: number;
  /** The node's type: view, text, pressable and so on. */
  type: string;
  /** Where layout put it on the screen. */
  rect: {x: number; y: number; width: number; height: number};
  /** Its element in the app's source, for a node an element made. */
  source?: {name: string; path?: string; line: number; column?: number};
  /** Where the components it is made inside are used, outermost first: `<Card />`'s place for a node of Card's. */
  uses?: {name: string; path?: string; line: number; column: number}[];
  /** The props it has set, by the names the app gives them. */
  props: {name: string; value: string}[];
  children: TreeNode[];
}

interface Waiter {
  resolve(response: Message): void;
  reject(error: Error): void;
}

// The adapter's own ids, above any GDB hands out.
const APP_BREAKPOINT = 0x10000000;
const STATE_BREAKPOINT = 0x20000000;
const APP_FRAME = 0x10000000;
/** The frame that shows the line that wrote a state, once the app has moved on from it. */
const WRITER_FRAME = APP_FRAME - 1;
const LOCALS_REFERENCE = 0x20000000;
const STATE_REFERENCE = 0x30000000;
/** The first reference of a list, a row or an object the editor can open. */
const OPENED_REFERENCE = 0x30000001;

const VM_FUNCTION = 'erb_vm_run';
const PAUSE_TIMEOUT_MS = 5000;
const SERVER_TIMEOUT_MS = 10000;
const SERVER_POLL_MS = 200;
const SIMULATOR_PORT = 3333;
const SIMULATOR_START_MS = 30000;
const SIMULATOR_ATTACH_MS = 2000;
/** Tells the editor where the simulator's page is, for it to show beside the code. */
const SIMULATOR_EVENT = 'embedded-react.simulator';
/** The editor's request for the node tree. Its response's `nodes` are the tops, each a TreeNode. */
export const NODES_REQUEST = 'embedded-react.nodes';
/**
 * The editor's request to outline a node on the panel while the app is stopped: its `rect` is a node's, and with
 * none the outline goes. Its response's `shown` is false when the app runs or the panel can't be drawn on.
 */
export const OUTLINE_REQUEST = 'embedded-react.outline';
const GDB_EXIT_MS = 1000;
const ERRORS_KEPT = 2000;
const ANSWER_MARK = '@erb ';
/** Starts the data id of a state, before its storage name. */
const STATE_DATA = 'erb:';
/** The script's own evaluator, typed in the debug console. */
const PRINT_COMMAND = /^erb print\s+(.+)$/s;

/** The Breakpoints pane's checkbox for the app's faults. */
const FAULT_FILTER = {
  filter: 'erb-faults',
  label: 'Stop on app faults',
  description:
    'Stops when the app faults or a package is refused, before the firmware handles it.',
  default: true,
};

/** What the adapter can do before GDB is running. GDB's own list follows in a `capabilities` event. */
const CAPABILITIES = {
  supportsConfigurationDoneRequest: true,
  supportsFunctionBreakpoints: true,
  supportsConditionalBreakpoints: true,
  supportsHitConditionalBreakpoints: true,
  supportsLogPoints: true,
  supportsEvaluateForHovers: true,
  supportsSetVariable: true,
  supportsDataBreakpoints: true,
  supportsSteppingGranularity: true,
  supportsExceptionInfoRequest: true,
  exceptionBreakpointFilters: [FAULT_FILTER],
  supportsTerminateRequest: true,
  supportTerminateDebuggee: true,
};

/** The fields of a launch configuration the adapter reads itself. The rest go to GDB. */
const OWN_FIELDS = [
  'cli',
  'debugFile',
  'environment',
  'gdb',
  'source',
  'server',
  'serverReady',
  'serverTimeout',
  'setupCommands',
  'postAttachCommands',
  'disconnectCommands',
  'stopOnAttach',
  'simulator',
  'entry',
  'port',
  'request',
  'screen',
];

/**
 * Reads protocol messages from a stream.
 *
 * @param input  The stream.
 * @param onMessage  Called with each message.
 */
export function readMessages(
  input: Readable,
  onMessage: (message: Message) => void,
): void {
  let buffered = Buffer.alloc(0);
  input.on('data', (chunk: Buffer) => {
    buffered = Buffer.concat([buffered, chunk]);
    for (;;) {
      const headerEnd = buffered.indexOf('\r\n\r\n');
      if (headerEnd < 0) {
        return;
      }
      const bodyStart = headerEnd + 4;
      const length = /content-length: *(\d+)/i.exec(
        buffered.subarray(0, headerEnd).toString('ascii'),
      );
      if (!length) {
        buffered = buffered.subarray(bodyStart);
        continue;
      }
      const bodyEnd = bodyStart + Number(length[1]);
      if (buffered.length < bodyEnd) {
        return;
      }
      const body = buffered.subarray(bodyStart, bodyEnd).toString('utf8');
      buffered = buffered.subarray(bodyEnd);
      onMessage(JSON.parse(body));
    }
  });
}

/**
 * Writes a protocol message to a stream.
 *
 * @param output  The stream.
 * @param message  The message.
 */
export function writeMessage(output: Writable, message: Message): void {
  const body = JSON.stringify(message);
  output.write(`Content-Length: ${Buffer.byteLength(body)}\r\n\r\n${body}`);
}

/**
 * A file name as GDB's `file` command reads it.
 *
 * @param name  The file name.
 *
 * @returns The name with its spaces, quotes and backslashes escaped.
 */
function escapeFilename(name: string): string {
  return name.replace(/[ \t\n\r\f\v\\'"]/g, '\\$&');
}

/**
 * A path with forward slashes.
 *
 * @param path  The path.
 *
 * @returns The path, as the debug file writes one.
 */
const slashed = (path: string): string => path.replace(/\\/g, '/');

/**
 * A path's last part, whichever slash it uses.
 *
 * @param path  The path.
 *
 * @returns The file's name.
 */
function fileName(path: string): string {
  return path.slice(
    Math.max(path.lastIndexOf('/'), path.lastIndexOf('\\')) + 1,
  );
}

/**
 * An environment with a configuration's variables over it. Windows ignores a name's case, so there a variable
 * replaces one spelled `Path` when the configuration says `PATH`.
 *
 * @param base  The environment.
 * @param over  The configuration's variables.
 * @param platform  The machine's system.
 *
 * @returns The merged environment.
 */
export function mergeEnvironment(
  base: NodeJS.ProcessEnv,
  over: Record<string, string>,
  platform: NodeJS.Platform = process.platform,
): NodeJS.ProcessEnv {
  const merged = {...base};
  for (const [name, value] of Object.entries(over)) {
    if (platform === 'win32') {
      for (const key of Object.keys(merged)) {
        if (key.toUpperCase() === name.toUpperCase()) {
          delete merged[key];
        }
      }
    }
    merged[name] = value;
  }
  return merged;
}

/**
 * Starts GDB with its own debug adapter.
 *
 * @param command  The GDB to run.
 * @param cwd  The folder it runs in.
 * @param environment  Its environment.
 *
 * @returns The process.
 */
function startGdbProcess(
  command: string,
  cwd: string | undefined,
  environment: NodeJS.ProcessEnv,
): GdbProcess {
  const child = spawn(command, ['-q', '-i', 'dap'], {cwd, env: environment});
  let errors = '';
  let listener: ((errors: string) => void) | null = null;
  const ended = (reason: string) => {
    listener?.(`${errors}${reason}`.trim());
    listener = null;
  };
  child.stderr.on('data', (chunk: Buffer) => {
    errors = (errors + chunk.toString('utf8')).slice(-ERRORS_KEPT);
  });
  child.on('error', error => ended(error.message));
  child.on('exit', () => ended(''));
  child.stdin.on('error', () => {});
  return {
    stdin: child.stdin,
    stdout: child.stdout,
    onExit: onEnd => {
      listener = onEnd;
    },
    kill: now => child.kill(now ? 'SIGKILL' : 'SIGTERM'),
  };
}

/** One debug session: the editor on one side, GDB on the other. */
export class DebugAdapter {
  private readonly output: Writable;
  private readonly startGdb: StartGdb;
  private readonly checkGdb: CheckGdb | null;
  /** Removes what the check of GDB made. */
  private cleanUpGdb: (() => void) | null = null;
  private readonly script: string;
  private readonly cli: string;

  private gdb: GdbProcess | null = null;
  /** The simulator, which stands where GDB does when the configuration names it. */
  private simulator: SimulatorLink | null = null;
  private server: ReturnType<typeof spawn> | null = null;
  private readonly guarded: boolean;
  /** Runs the GDB server of a board session, and resumes the board if the adapter is killed. */
  private guard: Guard | null = null;
  /** Whether the adapter left the board itself, with nothing for the guard to do. */
  private left = false;
  private clientSeq = 1;
  private gdbSeq = 1;
  private readonly waiting = new Map<number, Waiter>();
  private initializeArguments: Record<string, any> = {};
  private config: Record<string, any> = {};
  private session: 'launch' | 'attach' | null = null;
  private ended = false;

  /** The app's source file: its name in the debug file, and its path once known. */
  /** The app's source files as the debug file names them, the entry first. */
  private appFiles: string[] | null = null;
  /** The entry's path, once it is known. */
  private appSource: string | null = null;

  private running = false;
  private thread = 1;
  /** The threads as last listed with the board halted. GDB can't list a running board's over a probe. */
  private threadList: Record<string, any>[] = [];
  /** The stop the board is at, when the app is loaded. */
  private status: Status | null = null;
  /** The fault the board is stopped at. */
  private fault: Fault | null = null;
  /** Set while a step of the app runs, so its stop reads as a step. */
  private appStep = false;
  private dropAttachStop = false;
  /** Settles when the launch or attach is over. */
  private started: Promise<unknown> = Promise.resolve();
  private lastNote = '';
  /** Takes the next stop instead of the editor, while a request halts the board for itself. */
  private quietStop: ((body: Record<string, any>) => void) | null = null;
  /** Each thread's call stack with the app's frames in it, or null when it has none. */
  private readonly stacks = new Map<number, Record<string, any>[] | null>();
  /** The lists, rows and objects the editor can open at this stop, by reference. */
  private readonly opened = new Map<number, Opened>();
  /** Each one's reference, by where it is, so a value read twice keeps its reference. */
  private readonly openedAt = new Map<string, number>();
  private readonly stateScope: Opened = {
    where: {scope: 'state'},
    states: new Map(),
  };
  /** Whether the firmware has the breakpoint hook. */
  private hook = false;
  /** How many data breakpoints the editor last set. */
  private dataBreakpoints = 0;
  /** Whether a node is outlined on the panel. */
  private outlined = false;
  /** Said once: why the panel has no outline. */
  private outlineNote = '';

  /** Requests that must not overlap: the app's breakpoints, and the start of the run. */
  private lane: Promise<void> = Promise.resolve();
  /** GDB's events, handled in the order they came. */
  private events: Promise<void> = Promise.resolve();

  constructor(options: AdapterOptions) {
    this.output = options.output;
    this.startGdb = options.startGdb ?? startGdbProcess;
    this.checkGdb = options.checkGdb ?? (options.startGdb ? null : checkGdb);
    // The guard leaves the adapter's process tree through a shell, which Windows doesn't have.
    this.guarded =
      options.guard ?? (!options.startGdb && process.platform !== 'win32');
    this.script =
      options.script ?? fileURLToPath(new URL('./gdb/erb.py', import.meta.url));
    // Beside this file: cli.mts in the repository, cli.mjs once published.
    this.cli =
      options.cli ??
      fileURLToPath(
        new URL(
          import.meta.url.endsWith('.mts') ? './cli.mts' : './cli.mjs',
          import.meta.url,
        ),
      );
    readMessages(options.input, message => {
      if (message.type === 'request') {
        this.onRequest(message).catch(error =>
          this.fail(message, error.message),
        );
      }
    });
    options.input.on('end', () => this.end());
  }

  // --------------------------------------------------------------------------------------------------------------
  // The editor's side
  // --------------------------------------------------------------------------------------------------------------

  private toClient(message: Omit<Message, 'seq'>): void {
    writeMessage(this.output, {...message, seq: this.clientSeq++} as Message);
  }

  private event(name: string, body?: Record<string, any>): void {
    this.toClient({type: 'event', event: name, body});
  }

  private respond(request: Message, body?: Record<string, any>): void {
    this.toClient({
      type: 'response',
      request_seq: request.seq,
      command: request.command,
      success: true,
      body,
    });
  }

  private fail(request: Message, message: string): void {
    this.toClient({
      type: 'response',
      request_seq: request.seq,
      command: request.command,
      success: false,
      message,
      body: {error: {id: 1, format: message}},
    });
  }

  private say(text: string): void {
    this.event('output', {category: 'console', output: `${text}\n`});
  }

  private async onRequest(request: Message): Promise<void> {
    const args: Record<string, any> = request.arguments ?? {};
    switch (request.command) {
      case 'initialize':
        this.initializeArguments = args;
        return this.respond(request, CAPABILITIES);
      case 'launch':
      case 'attach': {
        // An editor that can only send one of the two says which it means in the configuration's "request".
        const named: unknown = args.request;
        const started = this.start(
          request,
          named === 'launch' || named === 'attach' ? named : request.command,
        );
        this.started = started.catch(() => {});
        return started;
      }
      case 'threads':
        // GDB takes threads it first sees while attaching for a running board, so the list waits for the attach.
        await this.started;
        return this.threads(request);
      case 'setBreakpoints':
        return this.setBreakpoints(request, args);
      case 'setExceptionBreakpoints':
        return this.setExceptionBreakpoints(request, args);
      case 'dataBreakpointInfo':
        return this.dataBreakpointInfo(request, args);
      case 'setDataBreakpoints':
        return this.setDataBreakpoints(request, args);
      case 'exceptionInfo':
        return this.exceptionInfo(request);
      case NODES_REQUEST:
        await this.started;
        return this.nodes(request);
      case OUTLINE_REQUEST:
        return this.outline(request, args);
      case 'configurationDone':
        return this.inLane(() => {
          this.running = this.session === 'launch' || this.simulator !== null;
          return this.forward(request);
        });
      case 'stackTrace':
        return this.stackTrace(request, args);
      case 'scopes':
        return this.scopes(request, args);
      case 'variables':
        return this.variables(request, args);
      case 'setVariable':
        return this.setVariable(request, args);
      case 'evaluate':
        return this.evaluate(request, args);
      case 'completions':
        return this.forward(request, this.withoutAppFrame(args));
      case 'next':
        return this.step(request, args, 'over');
      case 'stepIn':
        return this.step(request, args, 'into');
      case 'stepOut':
        return this.step(request, args, 'out');
      case 'continue':
        return this.resume(request, args);
      case 'disconnect':
        return this.disconnect(request);
      case 'terminate':
        // A board the session attached to isn't its to end.
        if (this.simulator || this.session === 'attach') {
          return this.disconnect(request);
        }
        return this.forward(request);
      default:
        return this.forward(request);
    }
  }

  /**
   * Runs a task after the ones before it in the lane.
   *
   * @param task  The task.
   *
   * @returns The task's promise.
   */
  private inLane(task: () => Promise<void>): Promise<void> {
    const result = this.lane.then(task);
    this.lane = result.catch(() => {});
    return result;
  }

  // --------------------------------------------------------------------------------------------------------------
  // GDB's side
  // --------------------------------------------------------------------------------------------------------------

  /**
   * Sends GDB a request.
   *
   * @param command  The request.
   * @param args  Its arguments.
   *
   * @returns GDB's response, failed or not.
   */
  private gdbCall(
    command: string,
    args?: Record<string, any>,
  ): Promise<Message> {
    if (this.simulator) {
      return this.simulator
        .call(command, args)
        .then(response => ({seq: 0, type: 'response', ...response}));
    }
    const gdb = this.gdb;
    if (!gdb) {
      return Promise.reject(new Error('The debugger is not running.'));
    }
    return new Promise((resolve, reject) => {
      const seq = this.gdbSeq++;
      this.waiting.set(seq, {resolve, reject});
      writeMessage(gdb.stdin, {seq, type: 'request', command, arguments: args});
    });
  }

  /**
   * Sends GDB a request that must succeed.
   *
   * @param command  The request.
   * @param args  Its arguments.
   *
   * @returns The response's body.
   */
  private async gdbRequest(
    command: string,
    args?: Record<string, any>,
  ): Promise<Record<string, any>> {
    const response = await this.gdbCall(command, args);
    if (!response.success) {
      throw new Error(response.message || `${command} failed`);
    }
    return response.body ?? {};
  }

  /**
   * Hands a request to GDB and its response to the editor.
   *
   * @param request  The editor's request.
   * @param args  Its arguments, when they aren't the request's own.
   *
   * @returns GDB's response.
   */
  private async relay(
    request: Message,
    args: Record<string, any> | undefined = request.arguments,
  ): Promise<Message> {
    const response = await this.gdbCall(request.command, args);
    this.toClient({
      type: 'response',
      request_seq: request.seq,
      command: request.command,
      success: response.success,
      message: response.message,
      body: response.body,
    });
    return response;
  }

  /**
   * Hands a request to GDB and its response to the editor.
   *
   * @param request  The editor's request.
   * @param args  Its arguments, when they aren't the request's own.
   */
  private async forward(
    request: Message,
    args: Record<string, any> | undefined = request.arguments,
  ): Promise<void> {
    await this.relay(request, args);
  }

  /**
   * Runs a GDB command, as its console would.
   *
   * @param command  The command.
   *
   * @returns What it printed.
   */
  private async console(command: string): Promise<string> {
    const body = await this.gdbRequest('evaluate', {
      expression: command,
      context: 'repl',
    });
    return String(body.result ?? '');
  }

  /**
   * Asks gdb/erb.py about the app.
   *
   * @param request  The request, as `erb dap` reads it.
   *
   * @returns Its answer.
   */
  private async erb(
    request: Record<string, any>,
  ): Promise<Record<string, any>> {
    if (this.simulator) {
      return this.simulator.ask(request);
    }
    const encoded = Buffer.from(JSON.stringify(request)).toString('base64');
    const printed = await this.console(`erb dap ${encoded}`);
    const line = printed
      .split('\n')
      .find(candidate => candidate.startsWith(ANSWER_MARK));
    if (!line) {
      throw new Error(`gdb/erb.py gave no answer: ${printed.trim()}`);
    }
    const answer = JSON.parse(line.slice(ANSWER_MARK.length));
    if (answer.error) {
      throw new Error(answer.error);
    }
    return answer;
  }

  private onGdbMessage(message: Message): void {
    if (message.type === 'response') {
      const waiter = this.waiting.get(message.request_seq);
      this.waiting.delete(message.request_seq);
      waiter?.resolve(message);
    } else if (message.type === 'event') {
      this.events = this.events
        .then(() => this.onGdbEvent(message))
        .catch(error => this.say(`embedded-react: ${error.message}`));
    }
  }

  private async onGdbEvent(message: Message): Promise<void> {
    const body: Record<string, any> = message.body ?? {};
    switch (message.event) {
      case 'initialized':
        // Sent once the app's debug file is loaded instead.
        return;
      case 'stopped':
        if (body.reason === 'attach' && this.dropAttachStop) {
          this.dropAttachStop = false;
          return;
        }
        if (this.quietStop) {
          const taker = this.quietStop;
          this.quietStop = null;
          this.running = false;
          taker(body);
          return;
        }
        return this.reportStop(body);
      case 'continued':
        this.resumed();
        break;
      case 'breakpoint':
        // The hook's own breakpoint is not one of the editor's.
        if (body.breakpoint?.id < 0) {
          return;
        }
        break;
      case 'terminated':
        if (this.ended) {
          return;
        }
        this.ended = true;
        break;
    }
    this.event(message.event, message.body);
  }

  private resumed(): void {
    this.running = true;
    this.outlined = false;
    this.status = null;
    this.fault = null;
    this.stacks.clear();
    this.opened.clear();
    this.openedAt.clear();
  }

  /**
   * Tells the editor of a stop: where the hook made it, as a stop in the app.
   *
   * @param body  GDB's stopped event.
   */
  private async reportStop(body: Record<string, any>): Promise<void> {
    this.resumed();
    this.running = false;
    this.thread = body.threadId ?? this.thread;
    const stepped = this.appStep;
    this.appStep = false;
    const status = (await this.erb({do: 'status'}).catch(
      () => null,
    )) as Status | null;
    if (status?.mismatch && this.session === 'attach') {
      return this.refuse(refusal(status.mismatch));
    }
    if (
      status &&
      !status.loaded &&
      status.note &&
      status.note !== this.lastNote
    ) {
      this.lastNote = status.note;
      this.say(`embedded-react: ${status.note}`);
    }
    this.status = status?.loaded ? status : null;
    this.fault = status?.fault ?? null;
    const stopped = {...body};
    if (this.fault) {
      // The editor shows it as an exception, on the app's line when the app's code faulted.
      stopped.reason = 'exception';
      stopped.description = this.fault.refused ? 'Load refused' : 'App fault';
      stopped.text = this.fault.reason;
      delete stopped.hitBreakpointIds;
      // For an editor that shows a stop's place and not its reason.
      this.say(`embedded-react: ${stopped.description}: ${this.fault.text}`);
    } else if (this.status?.watch) {
      const change = this.status.watch;
      stopped.reason = 'data breakpoint';
      stopped.description = `${change.label}: ${change.previous} → ${change.value}`;
      stopped.text = `Written at ${change.where}`;
      delete stopped.hitBreakpointIds;
      this.say(
        `embedded-react: ${stopped.description}, written at ${change.where}`,
      );
      if (change.id !== null) {
        stopped.hitBreakpointIds = [STATE_BREAKPOINT + change.id];
      }
    } else if (this.status?.hookStop) {
      const hit = this.status.hit;
      stopped.reason = hit.length && !stepped ? 'breakpoint' : 'step';
      delete stopped.hitBreakpointIds;
      if (hit.length) {
        stopped.hitBreakpointIds = hit.map(number => APP_BREAKPOINT + number);
      }
      if (this.status.description) {
        stopped.description = this.status.description;
      }
    }
    this.event('stopped', stopped);
  }

  /** Drops what `flash --changed` goes by, so the next flash writes the package whatever the last one wrote. */
  private forgetFlash(): void {
    const debugFile: unknown = this.config.debugFile;
    if (typeof debugFile !== 'string' || !debugFile.endsWith('.erdbg')) {
      return;
    }
    try {
      rmSync(flashStampPath(debugFile.replace(/\.erdbg$/, '.erpkg')), {
        force: true,
      });
    } catch {}
  }

  /**
   * Ends a session that can't go on, with why, and leaves the board running.
   *
   * @param message  Why.
   */
  private async refuse(message: string): Promise<void> {
    this.forgetFlash();
    this.event('output', {category: 'important', output: `${message}\n`});
    this.event('output', {category: 'stderr', output: `${message}\n`});
    await this.leaveBoard();
    if (!this.ended) {
      this.ended = true;
      this.event('terminated');
    }
    this.end();
  }

  /**
   * Halts the board without telling the editor.
   *
   * @returns The stop's event.
   */
  private pauseQuietly(): Promise<Record<string, any>> {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.quietStop = null;
        reject(new Error('The board did not halt.'));
      }, PAUSE_TIMEOUT_MS);
      this.quietStop = body => {
        clearTimeout(timer);
        resolve(body);
      };
      this.gdbCall('pause', {threadId: this.thread}).catch(() => {});
    });
  }

  /**
   * Runs work that needs the board halted, halting it and resuming it when it runs.
   *
   * @param work  The work.
   *
   * @returns The work's result.
   */
  private async whileHalted<T>(work: () => Promise<T>): Promise<T> {
    // The simulator answers between the app's events, so it needs no halt.
    if (!this.running || this.simulator) {
      return work();
    }
    const stop = await this.pauseQuietly();
    try {
      return await work();
    } finally {
      if (stop.reason === 'pause' || stop.reason === 'signal') {
        await this.gdbRequest('continue', {threadId: stop.threadId});
        this.running = true;
      } else {
        // The board stopped by itself as it was asked to: that stop is the editor's.
        await this.reportStop(stop);
      }
    }
  }

  // --------------------------------------------------------------------------------------------------------------
  // Starting and ending
  // --------------------------------------------------------------------------------------------------------------

  private async start(
    request: Message,
    kind: 'launch' | 'attach',
  ): Promise<void> {
    const config: Record<string, any> = request.arguments ?? {};
    const simulated = Boolean(config.simulator);
    if (!simulated && !config.debugFile) {
      throw new Error(
        'The configuration needs "debugFile": the app.erdbg the build writes beside the package. To debug in the simulator with no board, give it "simulator": true instead.',
      );
    }
    this.config = config;
    this.session = kind;
    try {
      await (simulated
        ? this.prepareSimulator(config, kind)
        : this.prepare(config));
    } catch (error) {
      this.end();
      throw error;
    }
    this.event('initialized');
    if (simulated) {
      // The app runs on while the editor sends its breakpoints.
      this.running = true;
      this.respond(request);
      return;
    }

    const forwarded = Object.fromEntries(
      Object.entries(config).filter(([field]) => !OWN_FIELDS.includes(field)),
    );
    this.dropAttachStop = kind === 'attach' && !config.stopOnAttach;
    try {
      // GDB answers once the editor has sent its breakpoints and configurationDone.
      await this.gdbRequest(kind, forwarded);
      if (kind === 'attach') {
        for (const command of config.postAttachCommands ?? []) {
          await this.console(command);
        }
        // A board that was reset to attach has no app yet: its load writes the breakpoints.
        const synced = await this.erb({do: 'sync'}).catch(() => null);
        if (synced?.mismatch) {
          this.forgetFlash();
          throw new Error(refusal(synced.mismatch));
        }
        await this.listThreads();
        if (!config.stopOnAttach) {
          await this.gdbRequest('continue', {threadId: this.thread});
          this.running = true;
        }
      }
    } catch (error) {
      await this.leaveBoard();
      this.end();
      throw error;
    }
    this.respond(request);
  }

  /**
   * Lists the threads of a halted board, and keeps the list for while it runs.
   *
   * @returns GDB's response.
   */
  private async listThreads(): Promise<Message> {
    const response = await this.gdbCall('threads');
    if (response.success && response.body?.threads?.length) {
      this.threadList = response.body.threads;
    }
    return response;
  }

  private async threads(request: Message): Promise<void> {
    const response = await this.listThreads();
    if (response.success) {
      return this.respond(request, response.body);
    }
    if (!this.running) {
      return this.fail(request, response.message || 'threads failed');
    }
    this.respond(request, {
      threads: this.threadList.length
        ? this.threadList
        : [{id: this.thread, name: 'Board'}],
    });
  }

  /** Before GDB lets go of a board: none of the app's breakpoints, and the configuration's commands. */
  private async leaveBoard(): Promise<void> {
    if (!this.gdb || this.session !== 'attach') {
      return;
    }
    try {
      if (this.running) {
        await this.pauseQuietly();
      }
      await this.eraseOutline();
      await this.erb({do: 'breakpoints', breakpoints: []}).catch(() => {});
      for (const command of this.config.disconnectCommands ?? []) {
        await this.console(command);
      }
      if (this.config.target) {
        await this.console('disconnect');
      }
      this.left = true;
    } catch (error: any) {
      this.say(`embedded-react: ${error.message}`);
    }
  }

  /**
   * Starts GDB with the firmware's ELF, the script and the app's debug file loaded.
   *
   * @param config  The launch configuration.
   */
  private async prepare(config: Record<string, any>): Promise<void> {
    const gdbCommand = config.gdb ?? 'gdb';
    let environment = this.environment(config);
    // Before the server starts, so a GDB that can't run leaves none behind.
    if (this.checkGdb) {
      const checked = await this.checkGdb(gdbCommand, config.cwd, environment);
      environment = checked.environment;
      this.cleanUpGdb = checked.cleanUp;
      if (checked.note) {
        this.say(`embedded-react: ${checked.note}`);
      }
    }
    const guard =
      this.guarded && this.session === 'attach'
        ? () =>
            startGuard({
              server: config.server,
              cwd: config.cwd,
              environment,
              gdb: gdbCommand,
              target: config.target,
              disconnectCommands: config.disconnectCommands ?? [],
            })
        : null;
    if (config.server) {
      await this.startServer(config, guard);
    } else if (guard) {
      this.guard = guard();
    }
    const gdb = this.startGdb(gdbCommand, config.cwd, environment);
    this.gdb = gdb;
    readMessages(gdb.stdout, message => this.onGdbMessage(message));
    gdb.onExit(errors => this.gdbEnded(gdbCommand, errors));

    const capabilities = await this.gdbRequest('initialize', {
      ...this.initializeArguments,
      adapterID: 'gdb',
    });
    this.event('capabilities', {
      capabilities: {
        ...capabilities,
        supportsExceptionInfoRequest: true,
        supportsDataBreakpoints: true,
        exceptionBreakpointFilters: [
          FAULT_FILTER,
          ...(capabilities.exceptionBreakpointFilters ?? []),
        ],
      },
    });
    for (const command of config.setupCommands ?? []) {
      await this.console(command);
    }
    // The script finds the hook in the ELF, so the ELF is loaded before the breakpoints come.
    if (config.program) {
      await this.console(`file ${escapeFilename(config.program)}`);
    }
    // `source` and `erb file` take the rest of the line as the path, spaces and all, so these aren't escaped.
    await this.console(`source ${this.script}`);
    await this.console(`erb file ${config.debugFile}`);
    const info = await this.erb({do: 'info'});
    this.appFiles = info.files ?? null;
    this.appSource = config.source ?? (info.source || null);
    this.hook = Boolean(info.hook);
    if (config.program && !info.hook) {
      this.say(
        'embedded-react: the firmware has no breakpoint hook, so breakpoints and stepping in the app are off. Build it with ERB_DEBUG_HOOK=1.',
      );
    }
  }

  /**
   * Connects to the Flow B simulator, starting one for a launch.
   *
   * @param config  The launch configuration.
   * @param kind  A launch starts the simulator and ends it with the session; an attach takes one that runs.
   */
  private async prepareSimulator(
    config: Record<string, any>,
    kind: 'launch' | 'attach',
  ): Promise<void> {
    const port = Number(config.port ?? SIMULATOR_PORT);
    const address =
      typeof config.simulator === 'string'
        ? config.simulator
        : `localhost:${port}`;
    const [host, addressPort] = address.split(':');
    const started = kind === 'launch';
    if (started) {
      // A launch owns its simulator and ends it with the session, so it doesn't take one that already runs.
      if (await portOpen(host, Number(addressPort))) {
        throw new Error(
          `Port ${addressPort} is already in use. To debug a simulator that runs there, use an attach configuration. Otherwise end what holds the port, or give the configuration another "port".`,
        );
      }
      this.startSimulator(config, Number(addressPort));
    }
    // The simulator listens on the IPv4 loopback, which not every runtime tries for `localhost`.
    const link = new SimulatorLink(
      address.replace(/^localhost(?=:)/, '127.0.0.1'),
      (event, body) => this.onSimulatorEvent(event, body),
      reason => this.simulatorEnded(reason),
    );
    await link.connect(
      started
        ? (config.serverTimeout ?? SIMULATOR_START_MS)
        : SIMULATOR_ATTACH_MS,
    );
    this.simulator = link;
    const info = await this.erb({do: 'info'});
    this.appFiles = info.files ?? null;
    this.appSource = config.source ?? (info.source || null);
    this.hook = true;
    const url = `http://${address}/`;
    this.say(`embedded-react: debugging the app in the simulator at ${url}`);
    this.event(SIMULATOR_EVENT, {url});
  }

  /**
   * Starts `embedded-react dev --aot` for the session.
   *
   * @param config  The launch configuration.
   * @param port  The port it serves.
   */
  private startSimulator(config: Record<string, any>, port: number): void {
    const args = [
      this.cli,
      'dev',
      '--aot',
      ...(config.entry ? [String(config.entry)] : []),
      '--port',
      String(port),
      ...(config.screen ? ['--screen', String(config.screen)] : []),
    ];
    const server = spawn(process.execPath, args, {
      cwd: config.cwd,
      env: this.environment(config),
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    this.server = server;
    const onOutput = (chunk: Buffer) =>
      this.event('output', {
        category: 'console',
        output: chunk.toString('utf8'),
      });
    server.stdout?.on('data', onOutput);
    server.stderr?.on('data', onOutput);
    server.on('error', error =>
      this.say(
        `embedded-react: the simulator could not start: ${error.message}`,
      ),
    );
    server.on('exit', () => {
      this.server = null;
    });
  }

  private onSimulatorEvent(event: string, body: Record<string, any>): void {
    switch (event) {
      case 'stopped':
        this.onGdbMessage({
          seq: 0,
          type: 'event',
          event,
          body: {...body, threadId: this.thread, allThreadsStopped: true},
        });
        break;
      case 'reloaded': {
        // A save loaded a new build: its files, and the lines its code moved the breakpoints to.
        this.appFiles = body.files ?? this.appFiles;
        this.appSource = this.config.source ?? body.source ?? this.appSource;
        for (const made of body.breakpoints ?? []) {
          this.event('breakpoint', {
            reason: 'changed',
            breakpoint: {
              id: APP_BREAKPOINT + made.id,
              verified: made.verified,
              message: made.message,
              line:
                made.line === undefined
                  ? undefined
                  : made.line - this.lineShift,
            },
          });
        }
        this.say(`embedded-react: build ${body.build} loaded`);
        break;
      }
      default:
        this.event(event, body);
    }
  }

  private simulatorEnded(reason: string): void {
    this.simulator = null;
    if (!this.ended) {
      this.ended = true;
      this.say(`embedded-react: ${reason}`);
      this.event('terminated');
    }
    this.end();
  }

  /**
   * The environment GDB and the GDB server run in: the adapter's, with the configuration's over it.
   *
   * @param config  The launch configuration.
   *
   * @returns The environment.
   */
  private environment(config: Record<string, any>): NodeJS.ProcessEnv {
    return mergeEnvironment(process.env, config.environment ?? {});
  }

  /**
   * Starts the configuration's GDB server and waits until GDB can connect to it.
   *
   * @param config  The launch configuration.
   * @param guard  Starts the guard that runs the server, for a board session that has one.
   */
  private async startServer(
    config: Record<string, any>,
    guard: (() => Guard) | null,
  ): Promise<void> {
    const [command, ...args] = config.server as string[];
    const port = /^(?:tcp:)?([^:]*):(\d+)$/.exec(config.target ?? '');
    const host = port?.[1] || 'localhost';
    // A server left from another session would be the one GDB connects to.
    if (port && (await portOpen(host, Number(port[2])))) {
      throw new Error(
        `Port ${port[2]} is already in use, so another GDB server is running. End it, or leave "server" out to use it.`,
      );
    }
    let printed = '';
    let failure = '';
    const onOutput = (chunk: Buffer) => {
      const text = chunk.toString('utf8');
      printed = (printed + text).slice(-ERRORS_KEPT);
      this.event('output', {category: 'console', output: text});
    };
    if (guard) {
      // The guard starts it, outside the adapter's process tree.
      this.guard = guard();
      this.guard.stdout.on('data', onOutput);
      this.guard.onFailure(why => {
        failure ||= why;
      });
    } else {
      const server = spawn(command, args, {
        cwd: config.cwd,
        env: this.environment(config),
        stdio: ['ignore', 'pipe', 'pipe'],
      });
      this.server = server;
      server.stdout?.on('data', onOutput);
      server.stderr?.on('data', onOutput);
      server.on('error', error => {
        failure = `${command} could not start: ${error.message}`;
      });
      server.on('exit', code => {
        failure ||= `${command} ended with code ${code}`;
        this.server = null;
      });
    }

    const deadline = Date.now() + (config.serverTimeout ?? SERVER_TIMEOUT_MS);
    for (;;) {
      if (failure) {
        throw new Error(failure);
      }
      // Ready when it has printed what the configuration names, or else when its port takes a connection.
      const ready = config.serverReady
        ? printed.includes(config.serverReady)
        : port !== null && (await portOpen(host, Number(port[2])));
      if (ready) {
        return;
      }
      if (Date.now() > deadline) {
        throw new Error(`${command} was not ready in time.`);
      }
      await new Promise(resolve => setTimeout(resolve, SERVER_POLL_MS));
    }
  }

  private gdbEnded(command: string, errors: string): void {
    this.gdb = null;
    const reason = errors ? `${command} ended: ${errors}` : `${command} ended.`;
    for (const waiter of this.waiting.values()) {
      waiter.reject(new Error(reason));
    }
    this.waiting.clear();
    if (!this.ended) {
      this.ended = true;
      this.event('terminated');
    }
    this.end();
  }

  private async disconnect(request: Message): Promise<void> {
    if (this.simulator) {
      // The simulator drops the breakpoints and runs the app on when the connection closes.
      this.ended = true;
      this.respond(request);
      if (request.command === 'terminate') {
        this.event('terminated');
      }
      this.end();
      return;
    }
    await this.leaveBoard();
    if (this.gdb && this.session === 'attach') {
      // The board runs on, whatever the editor asks: some send `terminateDebuggee` for every session.
      await this.gdbCall('disconnect', {terminateDebuggee: false}).catch(
        () => {},
      );
      this.respond(request);
      if (request.command === 'terminate' && !this.ended) {
        this.ended = true;
        this.event('terminated');
      }
    } else if (this.gdb) {
      await this.forward(request).catch(() => this.respond(request));
    } else {
      this.respond(request);
    }
    this.end();
  }

  /**
   * Ends GDB and the GDB server.
   *
   * @param now  True when the adapter itself is ending, so GDB can't be given time to end by itself.
   */
  end(now = false): void {
    this.simulator?.close();
    this.simulator = null;
    this.server?.kill();
    this.server = null;
    // Without the adapter's word that it left the board, the guard resumes it.
    this.guard?.end(this.left);
    this.guard = null;
    this.cleanUpGdb?.();
    this.cleanUpGdb = null;
    const gdb = this.gdb;
    if (gdb && now) {
      gdb.kill(true);
    } else if (gdb) {
      gdb.stdin.end();
      setTimeout(() => gdb.kill(), GDB_EXIT_MS).unref();
    }
  }

  // --------------------------------------------------------------------------------------------------------------
  // Breakpoints and stepping
  // --------------------------------------------------------------------------------------------------------------

  /**
   * Which of the app's files a path is: the one whose path from the entry's folder it ends with, or the entry by
   * its name alone.
   *
   * @param path  A source path from the editor.
   *
   * @returns The file's index in the debug file's files, or -1 for a file that isn't the app's.
   */
  private appFileOf(path: unknown): number {
    if (typeof path !== 'string' || this.appFiles === null) return -1;
    const shown = slashed(path);
    // A path that is exactly one of the files is that file, whatever else it ends like.
    const exact = this.appFiles.findIndex((file, index) => {
      const full = this.appPathOf(index);
      return shown === file || (full !== undefined && shown === slashed(full));
    });
    if (exact >= 0) return exact;
    let found = -1;
    this.appFiles.forEach((file, index) => {
      // A file outside the entry's folder (`../shared/Card.tsx`) matches by its path from the entry's, once that
      // is known, and until then by what follows its `../`.
      const matches =
        shown.endsWith(`/${file.replace(/^(\.\.\/)+/, '')}`) ||
        (index === 0 && fileName(path) === fileName(file));
      if (
        matches &&
        (found < 0 || file.length > this.appFiles![found].length)
      ) {
        found = index;
      }
    });
    return found;
  }

  /**
   * One of the app's files' paths, from the entry's.
   *
   * @param file  The file's index in the debug file's files.
   *
   * @returns The path, or undefined until the entry's is known.
   */
  private appPathOf(file: number): string | undefined {
    if (this.appSource === null || this.appFiles === null) return undefined;
    if (file === 0) return this.appSource;
    const entry = this.appFiles[0];
    const source = slashed(this.appSource);
    if (!source.endsWith(entry)) return undefined;
    return posix.normalize(
      source.slice(0, source.length - entry.length) + this.appFiles[file],
    );
  }

  /**
   * One of the app's files as the editor opens it.
   *
   * @param file  The file's index in the debug file's files.
   *
   * @returns Its name, and its path once the entry's is known.
   */
  private appSourceOf(file: number): {name: string; path?: string} {
    return {
      name: fileName(this.appFiles?.[file] ?? ''),
      path: this.appPathOf(file),
    };
  }

  /** 1 when the editor counts lines from 0, as the app's lines count from 1. */
  private get lineShift(): number {
    return this.initializeArguments.linesStartAt1 === false ? 1 : 0;
  }

  private async setBreakpoints(
    request: Message,
    args: Record<string, any>,
  ): Promise<void> {
    const path = args.source?.path;
    const file = this.appFileOf(path);
    if (file < 0) {
      await this.forward(request);
      return;
    }
    return this.inLane(async () => {
      // The editor's path for one file gives the entry's, which the others are beside.
      const shown = this.appFiles![file];
      if (file === 0) {
        this.appSource = path;
      } else if (this.appSource === null && slashed(path).endsWith(shown)) {
        this.appSource =
          path.slice(0, path.length - shown.length) + this.appFiles![0];
      }
      const wanted: Record<string, any>[] =
        args.breakpoints ?? (args.lines ?? []).map((line: number) => ({line}));
      // The script tests a condition, counts the hits and logs a message at each stop of the hook's.
      const answer = await this.whileHalted(() =>
        this.erb({
          do: 'breakpoints',
          file: shown,
          breakpoints: wanted.map(entry => ({
            line: entry.line + this.lineShift,
            condition: entry.condition,
            hitCondition: entry.hitCondition,
            logMessage: entry.logMessage,
          })),
        }),
      );
      const set: Record<string, any>[] = answer.breakpoints;
      this.respond(request, {
        breakpoints: wanted.map((_entry, index) => {
          const made = set[index] ?? {verified: false};
          return {
            id: made.id === undefined ? undefined : APP_BREAKPOINT + made.id,
            verified: made.verified,
            message: made.message,
            line:
              made.line === undefined ? undefined : made.line - this.lineShift,
            source: args.source,
          };
        }),
      });
    });
  }

  /**
   * Takes the app's fault checkbox, and hands GDB the rest.
   *
   * @param request  The request.
   * @param args  Its arguments.
   */
  private setExceptionBreakpoints(
    request: Message,
    args: Record<string, any>,
  ): Promise<void> {
    const filters: string[] = args.filters ?? [];
    const options: Record<string, any>[] = args.filterOptions ?? [];
    const asked = [...filters, ...options.map(option => option.filterId)];
    return this.inLane(async () => {
      const faults = await this.whileHalted(() =>
        this.erb({do: 'faults', stop: asked.includes(FAULT_FILTER.filter)}),
      );
      const response = await this.gdbCall(request.command, {
        ...args,
        filters: filters.filter(filter => filter !== FAULT_FILTER.filter),
        ...(args.filterOptions && {
          filterOptions: options.filter(
            option => option.filterId !== FAULT_FILTER.filter,
          ),
        }),
      });
      // One answer for each filter asked for, in the order asked.
      const theirs: Record<string, any>[] = [
        ...(response.body?.breakpoints ?? []),
      ];
      this.respond(request, {
        breakpoints: asked.map(filter =>
          filter === FAULT_FILTER.filter
            ? {verified: faults.verified, message: faults.message}
            : (theirs.shift() ?? {verified: response.success}),
        ),
      });
    });
  }

  /**
   * Offers a data breakpoint on a state. GDB's adapter has none of its own.
   *
   * @param request  The request.
   * @param args  Its arguments.
   */
  private dataBreakpointInfo(
    request: Message,
    args: Record<string, any>,
  ): void {
    const state = this.container(args.variablesReference)?.states.get(
      args.name,
    );
    if (state === undefined) {
      return this.respond(request, {
        dataId: null,
        description: "Only the app's state can break on a change.",
      });
    }
    this.respond(request, {
      dataId: STATE_DATA + state.storage,
      description: state.list
        ? `${args.name} (rows added or removed)`
        : args.name,
      accessTypes: ['write'],
      canPersist: false,
    });
  }

  /**
   * Sets the editor's data breakpoints, each on a state, as `erb watch` does.
   *
   * @param request  The request.
   * @param args  Its arguments.
   */
  private setDataBreakpoints(
    request: Message,
    args: Record<string, any>,
  ): Promise<void> {
    const wanted: Record<string, any>[] = args.breakpoints ?? [];
    return this.inLane(async () => {
      let set: Record<string, any>[] = [];
      let refusal: string | undefined;
      // Nothing to set or to remove leaves a running board alone.
      if (wanted.length || this.dataBreakpoints) {
        try {
          const answer = await this.whileHalted(() =>
            this.erb({
              do: 'watch',
              states: wanted.map(entry => {
                const dataId = String(entry.dataId);
                return dataId.startsWith(STATE_DATA)
                  ? dataId.slice(STATE_DATA.length)
                  : dataId;
              }),
            }),
          );
          set = answer.breakpoints;
        } catch (error: any) {
          refusal = error.message;
        }
      }
      this.dataBreakpoints = set.filter(made => made.verified).length;
      this.respond(request, {
        breakpoints: wanted.map((_entry, index) => {
          const made = set[index] ?? {verified: false, message: refusal};
          return {
            id: made.id === undefined ? undefined : STATE_BREAKPOINT + made.id,
            verified: made.verified,
            message: made.message,
          };
        }),
      });
    });
  }

  private async exceptionInfo(request: Message): Promise<void> {
    if (!this.fault) {
      return this.forward(request);
    }
    this.respond(request, {
      exceptionId: this.fault.reason,
      description: this.fault.text,
      breakMode: 'always',
    });
  }

  /**
   * Resumes the board for a request of the editor's.
   *
   * @param request  The request.
   * @param args  Its arguments.
   */
  private async resume(
    request: Message,
    args: Record<string, any>,
  ): Promise<void> {
    await this.eraseOutline();
    this.resumed();
    const response = await this.relay(request, args);
    if (!response.success) {
      this.running = false;
    }
  }

  private async step(
    request: Message,
    args: Record<string, any>,
    kind: 'into' | 'over' | 'out',
  ): Promise<void> {
    // A faulted app has no next statement. At a state's change the app steps on when the hook can end the step.
    const inApp =
      this.status?.hookStop || (this.status?.watch?.live && this.hook);
    if (!inApp || this.fault || args.granularity === 'instruction') {
      return this.resume(request, args);
    }
    // The hook ends the step where the app's next statement starts, so the board just runs on.
    await this.eraseOutline();
    await this.erb({do: 'step', kind});
    this.resumed();
    this.appStep = true;
    try {
      await this.gdbRequest('continue', {threadId: args.threadId});
    } catch (error) {
      this.running = false;
      this.appStep = false;
      throw error;
    }
    this.respond(request);
  }

  // --------------------------------------------------------------------------------------------------------------
  // The call stack and variables
  // --------------------------------------------------------------------------------------------------------------

  /**
   * A thread's call stack with the app's frames in it, innermost first.
   *
   * @param threadId  The thread.
   * @param format  The editor's frame format.
   *
   * @returns The frames, or null when the thread isn't running the app.
   */
  private async mergedStack(
    threadId: number,
    format: unknown,
  ): Promise<Record<string, any>[] | null> {
    const known = this.stacks.get(threadId);
    if (known !== undefined) {
      return known;
    }
    const status = this.status;
    let merged: Record<string, any>[] | null = null;
    const change = status?.watch;
    // The app has moved on from the line that wrote the state, so the line gets a frame of its own.
    const writer =
      change && !change.live && change.line !== null
        ? [
            {
              id: WRITER_FRAME,
              name: `${change.name} (wrote ${change.label})`,
              line: change.line - this.lineShift,
              column: 0,
              source: this.appSourceOf(change.file ?? 0),
            },
          ]
        : [];
    if (status && (status.frames.length || writer.length)) {
      // The simulator has no C frames around the app's.
      const body = this.simulator
        ? {}
        : await this.gdbRequest('stackTrace', {
            threadId,
            startFrame: 0,
            levels: 0,
            format,
          });
      const frames: Record<string, any>[] = body.stackFrames ?? [];
      const vmFrame = this.simulator
        ? 0
        : frames.findIndex(frame => String(frame.name).startsWith(VM_FUNCTION));
      if (vmFrame >= 0) {
        const appFrames = status.frames.map((frame, index) => ({
          id: APP_FRAME + index,
          name: frame.name,
          line: frame.line === null ? 0 : frame.line - this.lineShift,
          column: 0,
          source:
            frame.line === null ? undefined : this.appSourceOf(frame.file ?? 0),
          presentationHint: frame.line === null ? 'subtle' : 'normal',
        }));
        // What the app called sits above its frames. At a stop of the hook's that is only the hook, and at a
        // state's change only what wrote it.
        const called =
          status.hookStop || change?.live ? [] : frames.slice(0, vmFrame);
        merged = [...writer, ...called, ...appFrames, ...frames.slice(vmFrame)];
      } else if (writer.length) {
        merged = [...writer, ...frames];
      }
    }
    this.stacks.set(threadId, merged);
    return merged;
  }

  private async stackTrace(
    request: Message,
    args: Record<string, any>,
  ): Promise<void> {
    const merged = await this.mergedStack(args.threadId, args.format);
    if (!merged) {
      await this.forward(request);
      return;
    }
    const start = args.startFrame ?? 0;
    const end = args.levels ? start + args.levels : merged.length;
    this.respond(request, {
      stackFrames: merged.slice(start, end),
      totalFrames: merged.length,
    });
  }

  /**
   * An app frame's place in the app's call chain.
   *
   * @param frameId  A frame's id.
   *
   * @returns The place, innermost 0, or -1 for one of GDB's frames.
   */
  private appFrame(frameId: unknown): number {
    const count = this.status?.frames.length ?? 0;
    return typeof frameId === 'number' &&
      frameId >= APP_FRAME &&
      frameId < APP_FRAME + count
      ? frameId - APP_FRAME
      : -1;
  }

  private withoutAppFrame(args: Record<string, any>): Record<string, any> {
    if (this.appFrame(args.frameId) < 0 && args.frameId !== WRITER_FRAME) {
      return args;
    }
    const {frameId: _appFrame, ...rest} = args;
    return rest;
  }

  private async scopes(
    request: Message,
    args: Record<string, any>,
  ): Promise<void> {
    const stateScope = {
      name: 'App state',
      variablesReference: STATE_REFERENCE,
      expensive: false,
    };
    const frame = this.appFrame(args.frameId);
    if (args.frameId === WRITER_FRAME) {
      // Its locals are gone with the statement.
      return this.respond(request, {scopes: [stateScope]});
    }
    if (frame >= 0) {
      return this.respond(request, {
        scopes: [
          {
            name: 'Locals',
            presentationHint: 'locals',
            variablesReference: LOCALS_REFERENCE + frame,
            expensive: false,
          },
          stateScope,
        ],
      });
    }
    const response = await this.gdbCall('scopes', args);
    if (response.success && this.status) {
      response.body.scopes = [...(response.body.scopes ?? []), stateScope];
    }
    this.toClient({
      type: 'response',
      request_seq: request.seq,
      command: request.command,
      success: response.success,
      message: response.message,
      body: response.body,
    });
  }

  private async variables(
    request: Message,
    args: Record<string, any>,
  ): Promise<void> {
    const reference: number = args.variablesReference;
    if (reference < LOCALS_REFERENCE) {
      await this.forward(request);
      return;
    }
    const container = this.container(reference);
    if (!container) {
      throw new Error('That value is from an earlier stop.');
    }
    const answer = await this.erb({
      do: 'variables',
      open: container.where,
      start: args.start,
      count: args.count,
      filter: args.filter,
    });
    this.respond(request, {
      variables: answer.variables.map((variable: Record<string, any>) => {
        if (variable.storage) {
          container.states.set(variable.name, {
            storage: variable.storage,
            list: variable.type === 'list',
          });
        }
        return {
          name: variable.name,
          value: variable.value,
          type: variable.type || undefined,
          variablesReference: this.openReference(variable.open),
          indexedVariables: variable.rows || undefined,
        };
      }),
    });
  }

  /**
   * What a variables reference of the adapter's own opens.
   *
   * @param reference  The reference.
   *
   * @returns What it opens, or undefined for a reference of GDB's or of an earlier stop.
   */
  private container(reference: number): Opened | undefined {
    if (reference === STATE_REFERENCE) {
      return this.stateScope;
    }
    if (reference >= LOCALS_REFERENCE && reference < STATE_REFERENCE) {
      return {
        where: {scope: 'locals', frame: reference - LOCALS_REFERENCE},
        states: new Map(),
      };
    }
    return this.opened.get(reference);
  }

  /**
   * The reference an editor opens a list, a row or an object with.
   *
   * @param where  Where the script says it is, if it opens.
   *
   * @returns The reference, or 0 for a value that doesn't open.
   */
  private openReference(where: Record<string, any> | undefined): number {
    if (!where) {
      return 0;
    }
    const key = JSON.stringify(where);
    let reference = this.openedAt.get(key);
    if (reference === undefined) {
      reference = OPENED_REFERENCE + this.opened.size;
      this.opened.set(reference, {where, states: new Map()});
      this.openedAt.set(key, reference);
    }
    return reference;
  }

  private async setVariable(
    request: Message,
    args: Record<string, any>,
  ): Promise<void> {
    if (args.variablesReference < LOCALS_REFERENCE) {
      await this.forward(request);
      return;
    }
    const container = this.container(args.variablesReference);
    if (!container) {
      throw new Error('That value is from an earlier stop.');
    }
    const answer = await this.erb({
      do: 'set',
      open: container.where,
      name: args.name,
      value: args.value,
    });
    this.respond(request, {value: answer.value});
    // What holds it shows it too: a row's line, a list's.
    if (
      this.opened.has(args.variablesReference) &&
      this.initializeArguments.supportsInvalidatedEvent
    ) {
      this.event('invalidated', {areas: ['variables']});
    }
  }

  private async evaluate(
    request: Message,
    args: Record<string, any>,
  ): Promise<void> {
    const frame = this.appFrame(args.frameId);
    const typed = String(args.expression).trim();
    // A command typed in the debug console is GDB's, but `erb print` reads the frame the editor has selected.
    const print = args.context === 'repl' ? PRINT_COMMAND.exec(typed) : null;
    // In the simulator everything is the app's, the debug console too.
    const inApp =
      frame >= 0 || args.frameId === WRITER_FRAME || this.simulator !== null;
    if (inApp && (print || args.context !== 'repl' || this.simulator)) {
      return this.respond(
        request,
        await this.appValue(print ? print[1] : typed, frame),
      );
    }
    const response = await this.gdbCall(
      request.command,
      this.withoutAppFrame(args),
    );
    if (!response.success && args.context === 'watch' && this.status) {
      // A watched expression of the app's still reads its state at a stop in C.
      const value = await this.appValue(typed, -1).catch(() => null);
      if (value) {
        return this.respond(request, value);
      }
    }
    this.toClient({
      type: 'response',
      request_seq: request.seq,
      command: request.command,
      success: response.success,
      message: response.message,
      body: response.body,
    });
  }

  /**
   * Answers the node tree. A running board is halted for the read and resumed.
   *
   * @param request  The request.
   */
  private nodes(request: Message): Promise<void> {
    // The debug file counts columns from 1.
    const columnShift =
      this.initializeArguments.columnsStartAt1 === false ? 1 : 0;
    const shown = (node: ScriptNode): TreeNode => ({
      id: node.tag,
      type: node.type,
      rect: {
        x: node.rect[0],
        y: node.rect[1],
        width: node.rect[2],
        height: node.rect[3],
      },
      source:
        node.line === null
          ? undefined
          : {
              ...this.appSourceOf(node.file ?? 0),
              line: node.line - this.lineShift,
              column:
                node.column === null ? undefined : node.column - columnShift,
            },
      uses: node.uses?.length
        ? node.uses.map(([file, line, column]) => ({
            ...this.appSourceOf(file),
            line: line - this.lineShift,
            column: column - columnShift,
          }))
        : undefined,
      props: node.props.map(([name, value]) => ({name, value})),
      children: node.children.map(shown),
    });
    return this.inLane(async () => {
      const answer = await this.whileHalted(() => this.erb({do: 'nodes'}));
      this.respond(request, {
        nodes: (answer.nodes as ScriptNode[]).map(shown),
      });
    });
  }

  /**
   * Outlines a node on the stopped app's panel, or removes the outline.
   *
   * @param request  The request.
   * @param args  Its arguments: `rect`, a node's, or none.
   */
  private outline(request: Message, args: Record<string, any>): Promise<void> {
    const rect: Record<string, any> | undefined = args.rect;
    return this.inLane(async () => {
      // A running app paints over it, and a running board can't be drawn on.
      if (this.running || (!rect && !this.outlined)) {
        return this.respond(request, {shown: false});
      }
      try {
        const answer = await this.erb({
          do: 'outline',
          rect: rect ? [rect.x, rect.y, rect.width, rect.height] : null,
        });
        // A node with no size on the panel has none.
        this.outlined = answer.shown === true;
      } catch (error: any) {
        // A panel that isn't scanned from memory has none: said once, not at every node.
        if (error.message !== this.outlineNote) {
          this.outlineNote = error.message;
          this.say(`embedded-react: no outline on the panel: ${error.message}`);
        }
        this.outlined = false;
      }
      this.respond(request, {shown: this.outlined});
    });
  }

  /** Before the app runs on: what the outline covers on the panel goes back. */
  private async eraseOutline(): Promise<void> {
    if (this.outlined) {
      this.outlined = false;
      await this.erb({do: 'outline', rect: null}).catch(() => {});
    }
  }

  /**
   * Evaluates an expression over an app frame's locals and the app's state.
   *
   * @param expression  The expression.
   * @param frame  The frame's place in the app's call chain, or -1 for the state alone.
   *
   * @returns The evaluate response's body.
   */
  private async appValue(
    expression: string,
    frame: number,
  ): Promise<Record<string, any>> {
    const answer = await this.erb({do: 'evaluate', expression, frame});
    return {
      result: answer.value,
      type: answer.type || undefined,
      variablesReference: this.openReference(answer.open),
      indexedVariables: answer.rows || undefined,
    };
  }
}

/**
 * What a session that is refused for its debug file says.
 *
 * @param mismatch  The script's words for it.
 *
 * @returns The message.
 */
function refusal(mismatch: string): string {
  return `The app's debug file isn't the one of the app on the board: ${mismatch}.`;
}

/**
 * Whether a TCP port takes a connection.
 *
 * @param host  The host.
 * @param port  The port.
 *
 * @returns True when it does.
 */
function portOpen(host: string, port: number): Promise<boolean> {
  return new Promise(resolve => {
    const socket = connect({host, port});
    socket.once('connect', () => {
      socket.destroy();
      resolve(true);
    });
    socket.once('error', () => resolve(false));
  });
}
