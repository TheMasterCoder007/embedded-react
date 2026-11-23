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
//   - an app frame's variables are its locals and the app's state
//   - hover and Watch evaluate an expression over them, as `erb print` does
//
// The app's commands only work on a halted board, so one that arrives while it runs halts it and resumes it.

import {spawn} from 'node:child_process';
import {connect} from 'node:net';
import {fileURLToPath} from 'node:url';
import type {Readable, Writable} from 'node:stream';

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
  kill(): void;
}

type StartGdb = (
  command: string,
  cwd: string | undefined,
  environment: NodeJS.ProcessEnv,
) => GdbProcess;

export interface AdapterOptions {
  input: Readable;
  output: Writable;
  /** Starts GDB. Tests pass a stand-in. */
  startGdb?: StartGdb;
  /** gdb/erb.py, when it isn't beside this file. */
  script?: string;
}

/** What `erb dap status` says of a stop. */
interface Status {
  loaded: boolean;
  note?: string;
  hookStop: boolean;
  frames: {name: string; line: number | null}[];
  hit: number[];
  description?: string;
  fault?: Fault | null;
}

/** A fault or a refused load the board stopped at. */
interface Fault {
  reason: string;
  /** What `erb error` prints. */
  text: string;
  refused: boolean;
}

interface Waiter {
  resolve(response: Message): void;
  reject(error: Error): void;
}

// The adapter's own ids, above any GDB hands out.
const APP_BREAKPOINT = 0x10000000;
const APP_FRAME = 0x10000000;
const LOCALS_REFERENCE = 0x20000000;
const STATE_REFERENCE = 0x30000000;
const ROWS_REFERENCE = 0x30000001;

const VM_FUNCTION = 'erb_vm_run';
const PAUSE_TIMEOUT_MS = 5000;
const SERVER_TIMEOUT_MS = 10000;
const SERVER_POLL_MS = 200;
const GDB_EXIT_MS = 1000;
const ERRORS_KEPT = 2000;
const ANSWER_MARK = '@erb ';
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
  supportsSteppingGranularity: true,
  supportsExceptionInfoRequest: true,
  exceptionBreakpointFilters: [FAULT_FILTER],
  supportsTerminateRequest: true,
  supportTerminateDebuggee: true,
};

/** The fields of a launch configuration the adapter reads itself. The rest go to GDB. */
const OWN_FIELDS = [
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
    kill: () => child.kill(),
  };
}

/** One debug session: the editor on one side, GDB on the other. */
export class DebugAdapter {
  private readonly output: Writable;
  private readonly startGdb: StartGdb;
  private readonly script: string;

  private gdb: GdbProcess | null = null;
  private server: ReturnType<typeof spawn> | null = null;
  private clientSeq = 1;
  private gdbSeq = 1;
  private readonly waiting = new Map<number, Waiter>();
  private initializeArguments: Record<string, any> = {};
  private config: Record<string, any> = {};
  private session: 'launch' | 'attach' | null = null;
  private ended = false;

  /** The app's source file: its name in the debug file, and its path once known. */
  private appFile: string | null = null;
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
  private readonly rowStates = new Map<number, string>();
  private readonly stateStorage = new Map<string, string>();

  /** Requests that must not overlap: the app's breakpoints, and the start of the run. */
  private lane: Promise<void> = Promise.resolve();
  /** GDB's events, handled in the order they came. */
  private events: Promise<void> = Promise.resolve();

  constructor(options: AdapterOptions) {
    this.output = options.output;
    this.startGdb = options.startGdb ?? startGdbProcess;
    this.script =
      options.script ?? fileURLToPath(new URL('./gdb/erb.py', import.meta.url));
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
        const started = this.start(request, request.command);
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
      case 'exceptionInfo':
        return this.exceptionInfo(request);
      case 'configurationDone':
        return this.inLane(() => {
          this.running = this.session === 'launch';
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
    this.status = null;
    this.fault = null;
    this.stacks.clear();
    this.rowStates.clear();
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
    if (!this.running) {
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
    if (!config.debugFile) {
      throw new Error(
        'The configuration needs "debugFile": the app.erdbg the build writes beside the package.',
      );
    }
    this.config = config;
    this.session = kind;
    try {
      await this.prepare(config);
    } catch (error) {
      this.end();
      throw error;
    }
    this.event('initialized');

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
        await this.erb({do: 'sync'}).catch(() => {});
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
      await this.erb({do: 'breakpoints', breakpoints: []}).catch(() => {});
      for (const command of this.config.disconnectCommands ?? []) {
        await this.console(command);
      }
      if (this.config.target) {
        await this.console('disconnect');
      }
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
    if (config.server) {
      await this.startServer(config);
    }
    const gdbCommand = config.gdb ?? 'gdb';
    const gdb = this.startGdb(gdbCommand, config.cwd, this.environment(config));
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
    this.appFile = info.file;
    this.appSource = config.source ?? (info.source || null);
    if (config.program && !info.hook) {
      this.say(
        'embedded-react: the firmware has no breakpoint hook, so breakpoints and stepping in the app are off. Build it with ERB_DEBUG_HOOK=1.',
      );
    }
  }

  /**
   * The environment GDB and the GDB server run in: the adapter's, with the configuration's over it.
   *
   * @param config  The launch configuration.
   *
   * @returns The environment.
   */
  private environment(config: Record<string, any>): NodeJS.ProcessEnv {
    return {...process.env, ...config.environment};
  }

  /**
   * Starts the configuration's GDB server and waits until GDB can connect to it.
   *
   * @param config  The launch configuration.
   */
  private async startServer(config: Record<string, any>): Promise<void> {
    const [command, ...args] = config.server as string[];
    const port = /^(?:tcp:)?([^:]*):(\d+)$/.exec(config.target ?? '');
    const host = port?.[1] || 'localhost';
    // A server left from another session would be the one GDB connects to.
    if (port && (await portOpen(host, Number(port[2])))) {
      throw new Error(
        `Port ${port[2]} is already in use, so another GDB server is running. End it, or leave "server" out to use it.`,
      );
    }
    const server = spawn(command, args, {
      cwd: config.cwd,
      env: this.environment(config),
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    this.server = server;
    let printed = '';
    let failure = '';
    const onOutput = (chunk: Buffer) => {
      const text = chunk.toString('utf8');
      printed = (printed + text).slice(-ERRORS_KEPT);
      this.event('output', {category: 'console', output: text});
    };
    server.stdout?.on('data', onOutput);
    server.stderr?.on('data', onOutput);
    server.on('error', error => {
      failure = `${command} could not start: ${error.message}`;
    });
    server.on('exit', code => {
      failure ||= `${command} ended with code ${code}`;
      this.server = null;
    });

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
    const reason = errors
      ? `${command} ended: ${errors}. The adapter needs GDB 14 or later, built with Python.`
      : `${command} ended.`;
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
    await this.leaveBoard();
    if (this.gdb) {
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
    this.server?.kill();
    this.server = null;
    const gdb = this.gdb;
    if (gdb && now) {
      gdb.kill();
    } else if (gdb) {
      gdb.stdin.end();
      setTimeout(() => gdb.kill(), GDB_EXIT_MS).unref();
    }
  }

  // --------------------------------------------------------------------------------------------------------------
  // Breakpoints and stepping
  // --------------------------------------------------------------------------------------------------------------

  private isAppSource(path: unknown): path is string {
    return (
      typeof path === 'string' &&
      this.appFile !== null &&
      fileName(path) === this.appFile
    );
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
    if (!this.isAppSource(path)) {
      await this.forward(request);
      return;
    }
    return this.inLane(async () => {
      this.appSource = path;
      const wanted: Record<string, any>[] =
        args.breakpoints ?? (args.lines ?? []).map((line: number) => ({line}));
      // The script tests a condition, counts the hits and logs a message at each stop of the hook's.
      const answer = await this.whileHalted(() =>
        this.erb({
          do: 'breakpoints',
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
    // A faulted app has no next statement.
    if (
      !this.status?.hookStop ||
      this.fault ||
      args.granularity === 'instruction'
    ) {
      return this.resume(request, args);
    }
    // The hook ends the step where the app's next statement starts, so the board just runs on.
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
    if (status?.frames.length) {
      const body = await this.gdbRequest('stackTrace', {
        threadId,
        startFrame: 0,
        levels: 0,
        format,
      });
      const frames: Record<string, any>[] = body.stackFrames ?? [];
      const vmFrame = frames.findIndex(frame =>
        String(frame.name).startsWith(VM_FUNCTION),
      );
      if (vmFrame >= 0) {
        const appFrames = status.frames.map((frame, index) => ({
          id: APP_FRAME + index,
          name: frame.name,
          line: frame.line === null ? 0 : frame.line - this.lineShift,
          column: 0,
          source:
            frame.line === null
              ? undefined
              : {
                  name: this.appFile,
                  path: this.appSource ?? undefined,
                },
          presentationHint: frame.line === null ? 'subtle' : 'normal',
        }));
        // What the app called sits above its frames. At a stop of the hook's that is only the hook.
        const called = status.hookStop ? [] : frames.slice(0, vmFrame);
        merged = [...called, ...appFrames, ...frames.slice(vmFrame)];
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
    if (this.appFrame(args.frameId) < 0) {
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
    let answer;
    if (reference === STATE_REFERENCE) {
      answer = await this.erb({do: 'variables', scope: 'state'});
    } else if (reference > STATE_REFERENCE) {
      answer = await this.erb({
        do: 'variables',
        scope: 'rows',
        state: this.rowStates.get(reference),
      });
    } else {
      answer = await this.erb({
        do: 'variables',
        scope: 'locals',
        frame: reference - LOCALS_REFERENCE,
      });
    }
    this.respond(request, {
      variables: answer.variables.map((variable: Record<string, any>) => {
        const rows = this.rowsReference(variable);
        if (variable.storage) {
          this.stateStorage.set(variable.name, variable.storage);
        }
        return {
          name: variable.name,
          value: variable.value,
          type: variable.type || undefined,
          variablesReference: rows,
          indexedVariables: variable.rows || undefined,
        };
      }),
    });
  }

  /**
   * The reference an editor opens a list's rows with.
   *
   * @param variable  A variable or a value, as the script gives it.
   *
   * @returns The reference, or 0 for what has no rows.
   */
  private rowsReference(variable: Record<string, any>): number {
    if (!variable.rows) {
      return 0;
    }
    const reference = ROWS_REFERENCE + this.rowStates.size;
    this.rowStates.set(reference, variable.storage);
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
    if (args.variablesReference !== STATE_REFERENCE) {
      throw new Error("Only the app's state can be set from the debugger.");
    }
    const answer = await this.erb({
      do: 'set',
      state: this.stateStorage.get(args.name) ?? args.name,
      value: args.value,
    });
    this.respond(request, {value: answer.value});
  }

  private async evaluate(
    request: Message,
    args: Record<string, any>,
  ): Promise<void> {
    const frame = this.appFrame(args.frameId);
    const typed = String(args.expression).trim();
    // A command typed in the debug console is GDB's, but `erb print` reads the frame the editor has selected.
    const print = args.context === 'repl' ? PRINT_COMMAND.exec(typed) : null;
    if (frame >= 0 && (print || args.context !== 'repl')) {
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
      variablesReference: this.rowsReference(answer),
      indexedVariables: answer.rows || undefined,
    };
  }
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
