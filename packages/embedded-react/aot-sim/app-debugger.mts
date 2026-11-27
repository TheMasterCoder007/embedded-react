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

// The simulator's debugger: it serves the debug adapter's requests about the app from the simulator's own memory,
// where gdb/erb.py serves them through GDB on a board. The requests and their answers are the same, so the adapter
// shows the same breakpoints, call stack and variables for both.
//
// The runtime's hook (ErbDebugControl in erb_debug.h) calls the simulator at a breakpoint's place, where a step ends,
// after a load, and at a fault. atBreak() decides there whether the app stops.

import type {
  DebugFile,
  DebugPanel,
  DebugState,
} from '../aot/compiler/package/debug-file.mts';
import {
  evaluate,
  findText,
  parseExpression,
  Scope,
  toText,
  truthy,
} from './expressions.mts';
import type {Node} from './expressions.mts';
import {nodeTree} from './nodes.mts';
import {
  App,
  DebugError,
  Fields,
  Place,
  Row,
  Rows,
  Runtime,
  StatePlace,
  Target,
  buildHex,
  isStructured,
  localsTree,
  sameLine,
  settled,
  show,
  stateTree,
  stateValue,
  statementLine,
  statements,
  typeName,
} from './values.mts';
import type {Found, Frame, Line, Memory, Value} from './values.mts';

const STEP_NONE = 0;
const STEP_INTO = 1;
const STEP_OVER = 2;
const STEP_OUT = 3;
const STEP_ANY = 4;
const AT_RETURN = 1;
const AT_LOAD = 2;
const AT_FAULT = 3;
const AT_REFUSED = 4;
const END_OF_FUNCTION = 0xffffffff;

/** ErbDebugControl's fields. */
const CONTROL = {
  armed: 0,
  step: 1,
  event: 2,
  stepFunction: 4,
  stopCount: 6,
  stepFrom: 8,
  stepTo: 12,
  stepBase: 16,
  stops: 20,
};
const STOP_BYTES = 8;

const STEP_KINDS: Record<string, number> = {
  into: STEP_INTO,
  over: STEP_OVER,
  out: STEP_OUT,
};
const HIT_FORM = /^(>=|<=|==|=|>|<|%)?\s*(\d+)$/;
const ROW_NAME = /^\[(\d+)\]$/;

/** A step: when it ends, as ErbDebugStep says. */
interface Step {
  kind: number;
  function: number;
  from: number;
  to: number;
  base: number;
}

const NO_STEP: Step = {kind: STEP_NONE, function: 0, from: 0, to: 0, base: 0};

/** A breakpoint at a source line, with its condition, hit count and log message. */
interface Breakpoint {
  number: number;
  /** The file as the debug file names it, which a later build may not have. */
  file: string;
  /** The line the editor asked for, and the line with code it is at in the build that runs. */
  wanted: number;
  line: number | null;
  terms: [string | null, string | null, string | null];
  tree: Node | null;
  test: [string, number] | null;
  /** A log message's text and expressions, by turns. */
  parts: (string | Node)[] | null;
  passes: number;
}

/** A state the editor watches for a change. */
interface Watch {
  id: number;
  storage: string;
  label: string;
  previous: string;
}

/** A watched state's change the app stopped at. */
interface Change {
  id: number;
  label: string;
  previous: string;
  value: string;
  where: string;
  file: number | null;
  line: number | null;
  name: string | null;
  live: boolean;
}

/** Where a path of names and row numbers leads from a scope or an expression. */
interface Where {
  scope: 'state' | 'locals' | 'value';
  frame?: number;
  expression?: string;
  path?: (string | number)[];
}

/** A breakpoint as the editor is told of it. */
export interface BreakpointReport {
  id?: number;
  line?: number;
  verified: boolean;
  message?: string;
}

/** The VM's breakpoint hook. With a state watched, the VM calls at every instruction, and a step is tested here. */
class Hook {
  private readonly target: Target;
  private readonly address: number;
  readonly capacity: number;
  private step: Step = NO_STEP;
  private places: {function: number; offset: number}[] = [];
  private everyInstruction = false;

  constructor(runtime: Runtime) {
    this.target = runtime.target;
    this.address = runtime.control;
    this.capacity = runtime.stopCapacity;
  }

  get event(): number {
    return this.target.u8(this.address + CONTROL.event);
  }

  get stepKind(): number {
    return this.step.kind;
  }

  setStep(step: Step): void {
    this.step = step;
    this.flush();
  }

  setStops(places: {function: number; offset: number}[]): void {
    if (places.length > this.capacity) {
      throw new DebugError(
        `the breakpoints need ${places.length} places, but the simulator's hook holds ${this.capacity}: remove some`,
      );
    }
    this.places = places;
    this.flush();
  }

  /** Has the VM call at every instruction, for a watched state's change to be seen. */
  watchEveryInstruction(on: boolean): void {
    this.everyInstruction = on;
    this.flush();
  }

  /** Whether the VM would have called here with no state watched: at a breakpoint's place, or where the step ends. */
  wouldStop(frame: Frame): boolean {
    return (
      this.stepEnds(frame) ||
      this.places.some(
        place =>
          place.function === frame.function && place.offset === frame.offset,
      )
    );
  }

  /** Whether the step under way ends at a place, as the VM tests it. */
  stepEnds(frame: Frame): boolean {
    const step = this.step;
    if (step.kind === STEP_NONE || step.kind === STEP_ANY) {
      return step.kind === STEP_ANY;
    }
    const outside = frame.offset < step.from || frame.offset >= step.to;
    if (step.kind === STEP_INTO) {
      return frame.function !== step.function || outside;
    }
    return (
      frame.base < step.base ||
      (step.kind === STEP_OVER && frame.base === step.base && outside)
    );
  }

  private flush(): void {
    const {target, address} = this;
    const step = this.everyInstruction
      ? {...NO_STEP, kind: STEP_ANY}
      : this.step;
    target.setU16(address + CONTROL.stepFunction, step.function);
    target.setU32(address + CONTROL.stepFrom, step.from);
    target.setU32(address + CONTROL.stepTo, step.to);
    target.setU32(address + CONTROL.stepBase, step.base);
    target.setU8(address + CONTROL.step, step.kind);
    this.places.forEach((place, index) => {
      const at = address + CONTROL.stops + index * STOP_BYTES;
      target.setU16(at, place.function);
      target.setU16(at + 2, 0);
      target.setU32(at + 4, place.offset);
    });
    target.setU16(address + CONTROL.stopCount, this.places.length);
    target.setU8(
      address + CONTROL.armed,
      this.places.length > 0 || step.kind !== STEP_NONE ? 1 : 0,
    );
  }
}

/** Each function and offset a source line's statements start at. */
function linePlaces(
  panel: DebugPanel,
  line: Line,
): {function: number; offset: number}[] {
  const places: {function: number; offset: number}[] = [];
  panel.functions.forEach((_entry, functionIndex) => {
    for (const statement of statements(panel, functionIndex)) {
      if (sameLine(statement.line, line)) {
        places.push({function: functionIndex, offset: statement.at});
      }
    }
  });
  return places;
}

/** The instructions of the statement that holds an offset: [start, end). Before the first, from 0. */
function statementRange(
  panel: DebugPanel,
  functionIndex: number,
  offset: number,
): {start: number; end: number} {
  let start = 0;
  let end = END_OF_FUNCTION;
  for (const statement of statements(panel, functionIndex)) {
    if (statement.at <= offset) {
      start = statement.at;
    } else {
      end = statement.at;
      break;
    }
  }
  return {start, end};
}

/** Whether a hit count takes a pass. A bare number takes that pass and each after it. */
function hitCounts(
  [operator, number]: [string, number],
  passes: number,
): boolean {
  switch (operator) {
    case '%':
      return passes % number === 0;
    case '=':
    case '==':
      return passes === number;
    case '<':
      return passes < number;
    case '<=':
      return passes <= number;
    case '>':
      return passes > number;
    default:
      return passes >= number;
  }
}

/** The simulator's debugger for one debug session. */
export class AppDebugger {
  private readonly target: Target;
  private readonly debugAddress: number;
  /** Prints a line in the editor's debug console. */
  private readonly log: (line: string) => void;

  private debugFile: DebugFile | null = null;
  private sourcePath = '';
  private breakpoints: Breakpoint[] = [];
  private breakpointNumbers = 0;
  private watches: Watch[] = [];
  private watchNumbers = 0;
  /** Where the app was at the VM's last call, with a state watched: what wrote a state that then reads changed. */
  private lastFrame: Frame | null = null;
  /** Set by a load: the watched states' values are taken afresh once the app has set its state up. */
  private settlePending = false;
  private faultStops = true;

  /** The stop the app is at. */
  private hookStopped = false;
  private hit: number[] = [];
  private returned = false;
  private fault: {reason: string; text: string; refused: boolean} | null = null;
  private change: Change | null = null;

  /**
   * @param memory  The simulator's memory.
   * @param debugAddress  The runtime's descriptor in it (erb_debug).
   * @param log  Prints a line in the editor's debug console.
   */
  constructor(
    memory: Memory,
    debugAddress: number,
    log: (line: string) => void,
  ) {
    this.target = new Target(memory);
    this.debugAddress = debugAddress;
    this.log = log;
  }

  private runtime(): Runtime {
    return new Runtime(this.target, this.debugAddress);
  }

  private app(needRunning = true): App {
    return new App(this.runtime(), this.debugFile, needRunning);
  }

  // ------------------------------------------------------------------------------------------------------------------
  // The build that runs
  // ------------------------------------------------------------------------------------------------------------------

  /**
   * Takes the debug file of the package about to load. The breakpoints move to it by their files and lines.
   *
   * @param debugFile  The debug file.
   * @param sourcePath  The path of the app's entry.
   */
  setDebugFile(debugFile: DebugFile, sourcePath: string): void {
    this.debugFile = debugFile;
    this.sourcePath = sourcePath;
    for (const entry of this.breakpoints) {
      entry.line = this.codedLine(entry.file, entry.wanted);
      entry.passes = 0;
    }
  }

  get file(): DebugFile | null {
    return this.debugFile;
  }

  /** The breakpoints as the build that runs has them, for the editor to show after a reload. */
  breakpointReports(): BreakpointReport[] {
    return this.breakpoints.map(entry =>
      entry.line === null
        ? {
            id: entry.number,
            verified: false,
            message: `${entry.file}:${entry.wanted} has no code`,
          }
        : {id: entry.number, line: entry.line, verified: true},
    );
  }

  private fileIndex(name: string): number {
    const files = this.debugFile?.files ?? [];
    const wanted = name.replace(/\\/g, '/');
    const base = (path: string) => path.slice(path.lastIndexOf('/') + 1);
    let found = files
      .map((path, index) => ({path, index}))
      .filter(({path}) => path === wanted || wanted.endsWith(`/${path}`));
    if (!found.length) {
      found = files
        .map((path, index) => ({path, index}))
        .filter(
          ({path}) =>
            path.endsWith(`/${wanted}`) || base(path) === base(wanted),
        );
    }
    return found.length === 1 ? found[0].index : -1;
  }

  /** The first line with code at or after a line of a file, or null when the build has none. */
  private codedLine(file: string, wanted: number): number | null {
    const index = this.fileIndex(file);
    if (index < 0 || this.debugFile === null) {
      return null;
    }
    let found: number | null = null;
    for (const panel of this.debugFile.panels) {
      for (const entry of panel.functions) {
        for (const line of entry.lines) {
          if (
            (line.file ?? 0) === index &&
            line.line >= wanted &&
            (found === null || line.line < found)
          ) {
            found = line.line;
          }
        }
      }
    }
    return found;
  }

  private lineOfBreakpoint(entry: Breakpoint): Line | null {
    const file = this.fileIndex(entry.file);
    return entry.line === null || file < 0 ? null : {file, line: entry.line};
  }

  /** Writes the breakpoints' places for the layout the app runs into the hook. */
  private writeStops(): void {
    const app = this.app();
    const hook = this.hook(app.runtime);
    const places = new Map<string, {function: number; offset: number}>();
    for (const entry of this.breakpoints) {
      const line = this.lineOfBreakpoint(entry);
      for (const place of line ? linePlaces(app.panel, line) : []) {
        places.set(`${place.function}:${place.offset}`, place);
      }
    }
    hook.setStops([...places.values()]);
  }

  private hookState: Hook | null = null;

  private hook(runtime: Runtime): Hook {
    this.hookState ??= new Hook(runtime);
    return this.hookState;
  }

  /** The app's layout may have changed with its size: the breakpoints' places are written for the one it runs. */
  layoutChanged(): void {
    try {
      this.writeStops();
    } catch {
      // Not running: the next load writes them.
    }
  }

  /** The session is over: the app runs on with no breakpoints. */
  detach(): void {
    this.breakpoints = [];
    this.watches = [];
    try {
      const hook = this.hook(this.runtime());
      hook.watchEveryInstruction(false);
      hook.setStep(NO_STEP);
      hook.setStops([]);
    } catch {
      // Nothing to clear.
    }
    this.resumed();
  }

  // ------------------------------------------------------------------------------------------------------------------
  // Stops
  // ------------------------------------------------------------------------------------------------------------------

  /**
   * The runtime's hook called: decides whether the app stops here.
   *
   * @returns True when it stops, and the simulator waits for the debugger.
   */
  atBreak(): boolean {
    try {
      return this.decide();
    } catch (error) {
      if (!(error instanceof DebugError)) {
        throw error;
      }
      this.log(`embedded-react: ${error.message}`);
      this.hookStopped = true;
      return true;
    }
  }

  private decide(): boolean {
    const runtime = this.runtime();
    const hook = this.hook(runtime);
    const event = hook.event;
    if (event === AT_LOAD) {
      hook.setStep(NO_STEP);
      this.lastFrame = null;
      for (const entry of this.breakpoints) {
        entry.passes = 0;
      }
      this.settlePending = true;
      try {
        this.writeStops();
      } catch (error) {
        if (!(error instanceof DebugError)) throw error;
        this.log(
          `embedded-react: the app loaded, but its breakpoints aren't set: ${error.message}`,
        );
      }
      return false;
    }
    if (event === AT_FAULT || event === AT_REFUSED) {
      hook.setStep(NO_STEP);
      return this.atFault(runtime, event === AT_REFUSED);
    }
    const app = new App(runtime, this.debugFile);
    const frame = app.frameChain()[0];
    if (this.settlePending) {
      this.settlePending = false;
      this.settleWatches();
    }
    const changed = this.watches.length ? this.watchedChange(app, frame) : null;
    if (changed) {
      hook.setStep(NO_STEP);
      this.change = changed;
      this.hookStopped = true;
      return true;
    }
    const hasLines = statements(app.panel, frame.function).length > 0;
    if (event === AT_RETURN) {
      if (hook.stepKind === STEP_NONE) {
        return false;
      }
      if (!hasLines) {
        // Code the compiler made has no lines: on to the app's next statement.
        hook.setStep({...NO_STEP, kind: STEP_ANY});
        return false;
      }
      hook.setStep(NO_STEP);
      this.returned = true;
      this.hookStopped = true;
      return true;
    }
    if (this.watches.length && !hook.wouldStop(frame)) {
      return false;
    }
    const step = hook.stepKind;
    const {start} = statementLine(app.panel, frame.function, frame.offset);
    if (
      [STEP_INTO, STEP_OVER, STEP_ANY].includes(step) &&
      (!hasLines || start !== frame.offset)
    ) {
      // Partway through a statement, back from a call, or in code with no lines: on to the next statement.
      const range = hasLines
        ? statementRange(app.panel, frame.function, frame.offset)
        : {start: 0, end: END_OF_FUNCTION};
      hook.setStep({
        kind: step === STEP_OVER ? STEP_OVER : STEP_INTO,
        function: frame.function,
        from: range.start,
        to: range.end,
        base: frame.base,
      });
      return false;
    }
    const reached = this.breakpoints.filter(entry => {
      const line = this.lineOfBreakpoint(entry);
      return (
        line !== null &&
        linePlaces(app.panel, line).some(
          place =>
            place.function === frame.function && place.offset === frame.offset,
        )
      );
    });
    const scope = reached.length ? this.scope(app, 0) : null;
    const stopping = reached.filter(entry => this.stopsAt(entry, scope!));
    if (reached.length && !stopping.length && !hook.stepEnds(frame)) {
      // The step under way, if any, goes on.
      return false;
    }
    hook.setStep(NO_STEP);
    this.hit = stopping.map(entry => entry.number);
    this.hookStopped = true;
    return true;
  }

  /** Stops at a fault or a refused load, with what the runtime says of it. */
  private atFault(runtime: Runtime, refused: boolean): boolean {
    if (!this.faultStops) {
      return false;
    }
    const error = runtime.lastError((buildId, functionIndex, offset) =>
      this.debugFile !== null && buildId === this.debugFile.buildId >>> 0
        ? new App(runtime, this.debugFile, false).place(functionIndex, offset)
        : null,
    );
    if (error === null) {
      return false;
    }
    this.fault = {
      reason: error.reason,
      text: error.lines.join('\n'),
      refused,
    };
    this.hookStopped = true;
    return true;
  }

  /**
   * Whether a breakpoint the app reached stops it. It doesn't when its condition is false or its hit count leaves
   * the pass out, and one with a message logs it instead. A condition that can't be worked out stops.
   */
  private stopsAt(entry: Breakpoint, scope: Scope): boolean {
    try {
      if (entry.tree !== null && !truthy(evaluate(entry.tree, scope))) {
        return false;
      }
    } catch (error) {
      if (!(error instanceof DebugError)) throw error;
      this.log(`breakpoint ${entry.number}'s condition: ${error.message}`);
      return true;
    }
    entry.passes++;
    if (entry.test && !hitCounts(entry.test, entry.passes)) {
      return false;
    }
    if (entry.parts === null) {
      return true;
    }
    let message = '';
    for (const part of entry.parts) {
      if (typeof part === 'string') {
        message += part;
        continue;
      }
      try {
        message += toText(evaluate(part, scope));
      } catch (error) {
        if (!(error instanceof DebugError)) throw error;
        message += `<${error.message}>`;
      }
    }
    this.log(message);
    return false;
  }

  /** The watched state that reads changed since the VM's last call, if one does. */
  private watchedChange(app: App, frame: Frame): Change | null {
    const writer = this.lastFrame;
    this.lastFrame = frame;
    for (const watch of this.watches) {
      const state = app.panel.states.find(
        candidate => candidate.storage === watch.storage,
      );
      if (!state) {
        continue;
      }
      const value = this.watchedValue(app, state);
      if (value === watch.previous) {
        continue;
      }
      const previous = watch.previous;
      watch.previous = value;
      const written = writer
        ? statementLine(app.panel, writer.function, writer.offset).line
        : null;
      const now = statementLine(app.panel, frame.function, frame.offset).line;
      return {
        id: -watch.id,
        label: watch.label,
        previous,
        value,
        where: writer
          ? app.place(writer.function, writer.offset)
          : 'the runtime (a load, or a reset)',
        file: written?.file ?? null,
        line: written?.line ?? null,
        name: writer ? app.functionName(writer.function) : null,
        // Whether the app is still on the statement that wrote it.
        live:
          writer !== null &&
          writer.function === frame.function &&
          writer.base === frame.base &&
          sameLine(written, now),
      };
    }
    return null;
  }

  /** What a watched state is compared by: a list by its row count, as on a board. */
  private watchedValue(app: App, state: DebugState): string {
    return state.type === 'list'
      ? `${app.target.u32(app.arena + state.offset)} rows`
      : stateValue(app, state);
  }

  /** Takes each watched state's value as the one a change is measured from. */
  private settleWatches(): void {
    if (!this.watches.length) {
      return;
    }
    const app = this.app();
    for (const watch of this.watches) {
      const state = app.panel.states.find(
        candidate => candidate.storage === watch.storage,
      );
      if (state) {
        watch.previous = this.watchedValue(app, state);
      }
    }
  }

  /** The simulator stopped the app between its events, for a pause: a step another stop cut short ends. */
  halted(): void {
    try {
      const hook = this.hook(this.runtime());
      if (!this.hookStopped && hook.stepKind !== STEP_NONE) {
        hook.setStep(NO_STEP);
      }
    } catch {
      // No hook to settle.
    }
  }

  /** The app runs on. */
  resumed(): void {
    this.hookStopped = false;
    this.hit = [];
    this.returned = false;
    this.fault = null;
    this.change = null;
  }

  private scope(app: App, frame: number): Scope {
    return new Scope(app, frame, () => this.settleWatches());
  }

  // ------------------------------------------------------------------------------------------------------------------
  // The debug adapter's requests
  // ------------------------------------------------------------------------------------------------------------------

  /**
   * Answers a request of the debug adapter's.
   *
   * @param request  The request, named by its `do`.
   *
   * @returns Its answer, with `error` set when it failed.
   */
  request(request: Record<string, any>): Record<string, any> {
    try {
      switch (request.do) {
        case 'info':
          return {
            files: this.debugFile?.files ?? null,
            source: this.sourcePath,
            hook: true,
          };
        case 'status':
          return this.status();
        case 'breakpoints':
          return this.setBreakpoints(request);
        case 'faults':
          this.faultStops = Boolean(request.stop);
          return {verified: true};
        case 'step':
          return this.armStep(request.kind);
        case 'variables':
          return this.variables(request);
        case 'set':
          return this.set(request);
        case 'watch':
          return this.watch(request);
        case 'evaluate':
          return this.evaluateRequest(request);
        case 'nodes': {
          const app = this.app();
          return {nodes: nodeTree(app, app.runtime.engineAddress)};
        }
        default:
          return {
            error: `the simulator doesn't know the request ${request.do}`,
          };
      }
    } catch (error) {
      // Whatever a request trips over is its own failure: the simulator runs on.
      return {error: error instanceof Error ? error.message : String(error)};
    }
  }

  /** Where the app stopped: its frames, and whether the hook made the stop. */
  private status(): Record<string, any> {
    const fault = this.hookStopped ? this.fault : null;
    let app: App;
    try {
      app = this.app();
    } catch (error) {
      if (!(error instanceof DebugError)) throw error;
      return {loaded: false, note: error.message, fault};
    }
    const chain = app.inVm ? app.frameChain() : [];
    const frames = chain.map(frame => {
      const line = statementLine(app.panel, frame.function, frame.offset).line;
      return {
        name: app.functionName(frame.function),
        file: line?.file ?? null,
        line: line?.line ?? null,
      };
    });
    const status: Record<string, any> = {
      loaded: true,
      hookStop: this.hookStopped && frames.length > 0,
      frames,
      hit: [],
      fault,
      watch: this.change,
    };
    if (fault) {
      return status;
    }
    if (status.hookStop && this.returned) {
      status.description = `${frames[0].name} returns to the runtime`;
    } else if (status.hookStop) {
      status.hit = this.hit;
    }
    return status;
  }

  /**
   * Replaces the breakpoints in one of the app's files with the editor's. A line with no code moves to the next
   * line that has some.
   */
  private setBreakpoints(request: Record<string, any>): Record<string, any> {
    if (this.debugFile === null) {
      throw new DebugError('no app has been built yet');
    }
    const wantedFile: string = request.file ?? this.debugFile.files[0];
    const index = this.fileIndex(wantedFile);
    if (index < 0) {
      throw new DebugError(
        `${wantedFile} isn't one of the app's files: ${this.debugFile.files.join(', ')}`,
      );
    }
    const file = this.debugFile.files[index];
    // A line that stays keeps its number, so the editor's breakpoint keeps its id.
    const kept = new Map(
      this.breakpoints
        .filter(entry => entry.file === file)
        .map(entry => [entry.line, entry]),
    );
    this.breakpoints = this.breakpoints.filter(entry => entry.file !== file);
    const answers: BreakpointReport[] = [];
    for (const wanted of request.breakpoints as Record<string, any>[]) {
      const line = this.codedLine(file, wanted.line);
      if (line === null) {
        answers.push({
          verified: false,
          message: `${file}:${wanted.line} has no code`,
        });
        continue;
      }
      let entry = this.breakpoints.find(
        candidate => candidate.file === file && candidate.line === line,
      );
      if (!entry) {
        entry = kept.get(line) ?? {
          number: this.breakpointNumbers + 1,
          file,
          wanted: wanted.line,
          line,
          terms: [null, null, null],
          tree: null,
          test: null,
          parts: null,
          passes: -1,
        };
        try {
          this.setTerms(
            entry,
            wanted.condition,
            wanted.hitCondition,
            wanted.logMessage,
          );
        } catch (error) {
          if (!(error instanceof DebugError)) throw error;
          answers.push({line, verified: false, message: error.message});
          continue;
        }
        entry.wanted = wanted.line;
        this.breakpointNumbers = Math.max(this.breakpointNumbers, entry.number);
        this.breakpoints.push(entry);
      }
      answers.push({id: entry.number, line, verified: true});
    }
    // The hook holds so many places: the last breakpoints that don't fit are refused.
    for (;;) {
      try {
        this.writeStops();
        break;
      } catch (error) {
        if (!(error instanceof DebugError)) throw error;
        if (!error.message.includes('hook holds') || !this.breakpoints.length) {
          // Not running: the load writes them.
          break;
        }
        const refused = this.breakpoints.pop()!;
        for (const answer of answers) {
          if (answer.id === refused.number) {
            answer.verified = false;
            answer.message = error.message;
          }
        }
      }
    }
    return {breakpoints: answers};
  }

  /** Gives a breakpoint its condition, hit count and log message. Each is parsed here, so a mistake is said at once. */
  private setTerms(
    entry: Breakpoint,
    condition?: string,
    hits?: string,
    log?: string,
  ): void {
    const terms = [condition, hits, log].map(
      term => (term ?? '').trim() || null,
    ) as Breakpoint['terms'];
    if (
      entry.passes >= 0 &&
      terms.every((term, index) => term === entry.terms[index])
    ) {
      return;
    }
    const [conditionText, hitsText, logText] = terms;
    let test: Breakpoint['test'] = null;
    if (hitsText) {
      const match = HIT_FORM.exec(hitsText);
      if (!match || (match[1] === '%' && Number(match[2]) === 0)) {
        throw new DebugError(
          `a hit count is a number, or one after >, >=, ==, <, <= or %, not \`${hitsText}\``,
        );
      }
      test = [match[1] ?? '>=', Number(match[2])];
    }
    entry.tree = conditionText ? parseExpression(conditionText) : null;
    entry.test = test;
    // Text and {expression} parts by turns.
    entry.parts = logText
      ? logText
          .split(/\{(.*?)\}/)
          .map((part, index) =>
            index % 2 === 0 ? part : parseExpression(part),
          )
      : null;
    entry.terms = terms;
    entry.passes = 0;
  }

  /** Writes a step into the hook, for the app's next resume. */
  private armStep(kindName: string): Record<string, any> {
    if (this.fault) {
      throw new DebugError(
        `the app ${this.fault.refused ? "wasn't loaded" : 'faulted'}, so it has no next statement`,
      );
    }
    const app = this.app();
    const hook = this.hook(app.runtime);
    let kind = STEP_KINDS[kindName];
    const idle = this.returned || !app.inVm;
    if (kind === STEP_OUT && idle) {
      // Nothing to return from: on to the app's next statement.
      kind = STEP_INTO;
    }
    if (idle) {
      // Stops at the next statement the app runs, whatever runs it.
      hook.setStep({...NO_STEP, kind: STEP_ANY});
      return {};
    }
    const frame = app.frameChain()[0];
    if (kind === STEP_OUT) {
      hook.setStep({
        ...NO_STEP,
        kind,
        function: frame.function,
        base: frame.base,
      });
      return {};
    }
    const range = statementRange(app.panel, frame.function, frame.offset);
    hook.setStep({
      kind,
      function: frame.function,
      from: range.start,
      to: range.end,
      base: frame.base,
    });
    return {};
  }

  /** What the editor opened: a scope, or an expression's value, and what a path of names and row numbers leads to. */
  private opened(app: App, where: Where): Found {
    const written = () => this.settleWatches();
    let found: Found;
    if (where.scope === 'state') {
      found = stateTree(app, written);
    } else if (where.scope === 'locals') {
      const chain = app.inVm ? app.frameChain() : [];
      const frame = where.frame ?? 0;
      found =
        frame >= 0 && frame < chain.length
          ? localsTree(app, chain[frame])
          : new Fields();
    } else {
      found = findText(
        this.scope(app, where.frame ?? -1),
        where.expression ?? '',
      );
    }
    for (const step of where.path ?? []) {
      found = childOf(settled(found), step);
    }
    return found;
  }

  /** What the editor opened holds: a scope's values, an object's or a row's fields, or a list's rows. */
  private variables(request: Record<string, any>): Record<string, any> {
    const app = this.app();
    const where: Where = request.open;
    const value = settled(this.opened(app, where));
    if (value instanceof Rows) {
      if (request.filter === 'named') {
        return {variables: []};
      }
      const start: number = request.start || 0;
      const rows = value.children(start, request.count || value.count);
      return {
        variables: rows.map((row, number) =>
          variable(`[${start + number}]`, row, where, start + number),
        ),
      };
    }
    if (
      request.filter === 'indexed' ||
      !(value instanceof Row || value instanceof Fields)
    ) {
      return {variables: []};
    }
    const members = (value instanceof Row ? value.tree() : value).members;
    return {
      variables: [...members].map(([name, found]) =>
        variable(name, found, where, name),
      ),
    };
  }

  /** Sets a value the editor edited, by its name in what was opened. */
  private set(request: Record<string, any>): Record<string, any> {
    const app = this.app();
    const name: string = request.name;
    const value = settled(this.opened(app, request.open));
    const row = ROW_NAME.exec(name);
    const place = settable(
      childOf(value, row && value instanceof Rows ? Number(row[1]) : name),
      name,
    );
    place.write(String(request.value));
    return {value: show(place.read())};
  }

  /** Replaces the editor's data breakpoints with the states it names, each by its storage name. */
  private watch(request: Record<string, any>): Record<string, any> {
    const wanted: string[] = request.states;
    const app = wanted.length ? this.app() : null;
    const kept = new Map(this.watches.map(watch => [watch.storage, watch]));
    const answers: BreakpointReport[] = [];
    const made: Watch[] = [];
    for (const name of wanted) {
      try {
        const state = app!.state(name);
        let watch = kept.get(state.storage);
        if (!watch) {
          watch = {
            id: ++this.watchNumbers,
            storage: state.storage,
            label: app!.label(state),
            previous: this.watchedValue(app!, state),
          };
          kept.set(state.storage, watch);
        }
        made.push(watch);
        answers.push({id: -watch.id, verified: true});
      } catch (error) {
        if (!(error instanceof DebugError)) throw error;
        answers.push({verified: false, message: error.message});
      }
    }
    this.watches = [...new Set(made)];
    this.lastFrame = null;
    this.hook(this.runtime()).watchEveryInstruction(this.watches.length > 0);
    return {breakpoints: answers};
  }

  /** An expression's value, over a frame's locals and the app's state. */
  private evaluateRequest(request: Record<string, any>): Record<string, any> {
    const app = this.app();
    const expression = String(request.expression).trim();
    const frame: number = request.frame ?? -1;
    const value = settled(findText(this.scope(app, frame), expression));
    const answer: Record<string, any> = {
      value: show(value),
      type: typeName(value),
    };
    if (isStructured(value)) {
      answer.open = {scope: 'value', expression, frame, path: []};
    }
    if (value instanceof Rows) {
      answer.rows = value.count;
    }
    return answer;
  }

  /** The build the simulator runs, for a line in the debug console. */
  buildName(): string {
    return this.debugFile ? buildHex(this.debugFile.buildId) : 'none';
  }
}

/** A row of a list by its number, or a field of a row or an object by its name. */
function childOf(value: Value, step: string | number): Found {
  if (value instanceof Rows && typeof step === 'number') {
    const found = value.child(step);
    if (found === undefined) {
      throw new DebugError(`${value.state.name} has no row ${step}`);
    }
    return found;
  }
  if (value instanceof Row && typeof step === 'string') {
    return value.child(step);
  }
  if (value instanceof Fields && typeof step === 'string') {
    const found = value.members.get(step);
    if (found !== undefined) {
      return found;
    }
  }
  throw new DebugError(`the value changed: it has no ${step} now`);
}

/** A place to set, from what a name finds. A value that is kept nowhere, or is made of others, has none. */
function settable(found: Found, name: string): Place {
  if (found instanceof Place) {
    return found;
  }
  if (isStructured(found)) {
    const kind =
      found instanceof Row
        ? 'a row'
        : found instanceof Rows
          ? 'a list'
          : 'an object';
    throw new DebugError(
      `${name} is ${kind}: set one of its ${found instanceof Rows ? 'rows' : 'fields'}`,
    );
  }
  throw new DebugError(`${name} is worked out, not kept, so it can't be set`);
}

/** A value's type, as the Variables pane shows it. An enum's is the values it takes. */
function kindText(found: Found, value: Value): string {
  if (found instanceof Place && found.kind === 'enum' && found.values) {
    return found.values.map(choice => JSON.stringify(choice)).join(' | ');
  }
  if (found instanceof Place && found.kind) {
    return found.kind;
  }
  return typeName(value);
}

/** A row of the Variables pane: a value, and for one that opens, where it is. */
function variable(
  name: string,
  found: Found,
  where: Where,
  step: string | number,
): Record<string, any> {
  const value = settled(found);
  const shown: Record<string, any> = {
    name,
    value: show(value),
    type: kindText(found, value),
  };
  if (isStructured(value)) {
    shown.open = {...where, path: [...(where.path ?? []), step]};
  }
  if (value instanceof Rows) {
    shown.rows = value.count;
  }
  if (found instanceof StatePlace) {
    shown.storage = found.state.storage;
  }
  return shown;
}
