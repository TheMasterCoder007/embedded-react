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

// The Flow B simulator's thread: it runs the Flow B runtime (sim/embedded-react-aot.wasm), loads the packages the
// dev server sends, runs the frames and posts the rows of the panel that changed.
//
// The runtime's debug hook calls in here (erb_debug_break). When the debugger says the app stops, the call doesn't
// return: this thread waits inside it, serving the debugger's requests from the simulator's memory, until the
// debugger resumes the app. So a stop freezes the panel and the app counts no time, as a halted board does.

import {createRequire} from 'node:module';
import {join} from 'node:path';
import {
  parentPort,
  receiveMessageOnPort,
  workerData,
} from 'node:worker_threads';
import type {MessagePort} from 'node:worker_threads';
import type {DebugFile} from '../aot/compiler/package/debug-file.mts';
import {AppDebugger} from './app-debugger.mts';
import type {BreakpointReport} from './app-debugger.mts';

/** What the dev server starts the thread with. */
export interface WorkerStart {
  /** The folder with embedded-react-aot.cjs and its .wasm. */
  simDir: string;
  /** The dev server's messages. */
  port: MessagePort;
  /** Raised with each message, so a thread waiting at a stop wakes for it. */
  signal: SharedArrayBuffer;
}

/** A message from the dev server. */
export type ToWorker =
  | {
      kind: 'load';
      package: Uint8Array;
      debugFile: DebugFile;
      sourcePath: string;
      /** The panel's size for the first load. */
      width: number;
      height: number;
    }
  | {kind: 'touch'; phase: number; x: number; y: number}
  | {kind: 'resize'; width: number; height: number}
  | {kind: 'attach'; attached: boolean}
  | {kind: 'request'; id: number; request: Record<string, any>};

/** A message to the dev server. */
export type FromWorker =
  | {kind: 'ready'}
  | {
      kind: 'frame';
      width: number;
      height: number;
      top: number;
      rows: number;
      pixels: Uint8Array;
    }
  | {
      kind: 'loaded';
      ok: boolean;
      error?: string;
      build?: string;
      files?: string[];
      source?: string;
      breakpoints?: BreakpointReport[];
    }
  | {kind: 'log'; line: string}
  | {kind: 'state'; stopped: boolean}
  | {kind: 'outline'; rect: [number, number, number, number] | null}
  | {kind: 'answer'; id: number; answer: Record<string, any>}
  | {kind: 'event'; event: string; body: Record<string, any>};

/** A rect cut to the panel: its left, top, width and height, or null when none of it is on the panel. */
function onPanel([x, y, rectWidth, rectHeight]: number[]):
  [number, number, number, number] | null {
  const left = Math.max(x, 0);
  const top = Math.max(y, 0);
  const right = Math.min(x + rectWidth, width);
  const bottom = Math.min(y + rectHeight, height);
  return right > left && bottom > top
    ? [left, top, right - left, bottom - top]
    : null;
}

/** The simulator's module, as Emscripten makes it. */
interface SimulatorModule {
  HEAPU8: Uint8Array;
  onDebugBreak?: () => void;
  onAppLog?: (line: string) => void;
  _malloc(bytes: number): number;
  _free(address: number): void;
  _er_web_aot_init(width: number, height: number): number;
  _er_web_aot_load(address: number, length: number, clockMs: number): number;
  _er_web_aot_error(): number;
  _er_web_aot_running(): number;
  _er_web_aot_resize(width: number, height: number): number;
  _er_web_aot_pump(deltaMs: number): number;
  _er_web_aot_touch(phase: number, x: number, y: number): void;
  _er_web_aot_framebuffer(): number;
  _er_web_aot_dirty_top(): number;
  _er_web_aot_debug(): number;
  UTF8ToString(address: number): string;
}

const FRAME_MS = 16;
/** The most time one frame counts, so a stalled host doesn't jump the app ahead. */
const LONGEST_FRAME_MS = 100;
const RGBA_BYTES = 4;

const start = workerData as WorkerStart;
const signal = new Int32Array(start.signal);
const post = (message: FromWorker, transfer: ArrayBuffer[] = []) =>
  parentPort!.postMessage(message, transfer);

const require = createRequire(import.meta.url);
const createModule: (
  options: Record<string, unknown>,
) => Promise<SimulatorModule> = require(
  join(start.simDir, 'embedded-react-aot.cjs'),
);
const simulator = await createModule({
  print: (line: string) => post({kind: 'log', line}),
  printErr: (line: string) => post({kind: 'log', line}),
});

const appDebugger = new AppDebugger(
  {bytes: () => simulator.HEAPU8},
  simulator._er_web_aot_debug(),
  line => output(line),
);

let width = 0;
let height = 0;
/** A debugger is attached. Without one the hook never stops the app. */
let attached = false;
/** The app waits inside the hook. */
let stopped = false;
/** The app waits between its events, for a pause. */
let paused = false;
let lastFrameAt = performance.now();
/** What arrived while the app waited inside the hook, done once it runs on. */
let pendingLoad: Extract<ToWorker, {kind: 'load'}> | null = null;
let pendingResize: Extract<ToWorker, {kind: 'resize'}> | null = null;
/** The entry of the app that runs. */
let sourcePath = '';

/** Prints a line in the editor's debug console. */
function output(line: string): void {
  post({
    kind: 'event',
    event: 'output',
    body: {category: 'console', output: `${line}\n`},
  });
}

simulator.onAppLog = line => {
  post({kind: 'log', line});
  if (attached) {
    post({
      kind: 'event',
      event: 'output',
      body: {category: 'stdout', output: `${line}\n`},
    });
  }
};

simulator.onDebugBreak = () => {
  if (!attached || !appDebugger.atBreak()) {
    return;
  }
  stopped = true;
  post({kind: 'state', stopped: true});
  post({kind: 'event', event: 'stopped', body: {reason: 'breakpoint'}});
  // Wait here, inside the runtime's call, until the debugger resumes the app.
  while (stopped) {
    const seen = Atomics.load(signal, 0);
    let received = false;
    for (;;) {
      const next = receiveMessageOnPort(start.port);
      if (!next) break;
      received = true;
      handle(next.message as ToWorker);
    }
    if (stopped && !received) {
      Atomics.wait(signal, 0, seen);
    }
  }
  appDebugger.resumed();
  post({kind: 'state', stopped: false});
  // The time spent stopped is the debugger's, not the app's.
  lastFrameAt = performance.now();
};

/** Posts the rows of the panel the last frame changed. */
function postFrame(top: number, rows: number): void {
  if (rows <= 0 || top < 0) {
    return;
  }
  const from = simulator._er_web_aot_framebuffer() + top * width * RGBA_BYTES;
  const pixels = simulator.HEAPU8.slice(from, from + rows * width * RGBA_BYTES);
  post({kind: 'frame', width, height, top, rows, pixels}, [
    pixels.buffer as ArrayBuffer,
  ]);
}

/** Runs one frame and posts what it changed. */
function frame(deltaMs: number): void {
  const rows = simulator._er_web_aot_pump(deltaMs);
  postFrame(simulator._er_web_aot_dirty_top(), rows);
}

function load(message: Extract<ToWorker, {kind: 'load'}>): void {
  if (width === 0) {
    if (!simulator._er_web_aot_init(message.width, message.height)) {
      post({
        kind: 'loaded',
        ok: false,
        error: `the simulator can't make a ${message.width}x${message.height} panel`,
      });
      return;
    }
    width = message.width;
    height = message.height;
  }
  const ranBefore = width > 0 && simulator._er_web_aot_running() !== 0;
  const previous = appDebugger.file;
  const previousSource = previous ? sourcePath : '';
  // The hook writes the breakpoints for the new build as it loads, so the debugger has its debug file first.
  appDebugger.setDebugFile(message.debugFile, message.sourcePath);
  const address = simulator._malloc(message.package.length);
  simulator.HEAPU8.set(message.package, address);
  const ok = simulator._er_web_aot_load(
    address,
    message.package.length,
    Date.now(),
  );
  simulator._free(address);
  if (!ok) {
    const why = simulator.UTF8ToString(simulator._er_web_aot_error());
    if (simulator._er_web_aot_running()) {
      // A refused package leaves the app that was running as it was.
      if (previous) {
        appDebugger.setDebugFile(previous, previousSource);
        appDebugger.layoutChanged();
      }
      post({kind: 'loaded', ok: false, error: why});
    } else {
      // The new build took the old app's place and stopped as it started, so nothing runs.
      sourcePath = message.sourcePath;
      post({
        kind: 'loaded',
        ok: false,
        error: ranBefore ? `${why} (the app that was running has ended)` : why,
      });
    }
  } else {
    sourcePath = message.sourcePath;
    post({
      kind: 'loaded',
      ok: true,
      build: appDebugger.buildName(),
      files: message.debugFile.files,
      source: message.sourcePath,
      breakpoints: appDebugger.breakpointReports(),
    });
  }
  lastFrameAt = performance.now();
  frame(0);
}

function resize(message: Extract<ToWorker, {kind: 'resize'}>): void {
  if (width === 0) {
    return;
  }
  if (!simulator._er_web_aot_resize(message.width, message.height)) {
    const why = simulator.UTF8ToString(simulator._er_web_aot_error());
    post({
      kind: 'log',
      line: `the panel stays ${width}x${height}${why ? `: ${why}` : ''}`,
    });
    // The page shows the size the panel kept.
    post({kind: 'state', stopped: false});
    return;
  }
  width = message.width;
  height = message.height;
  appDebugger.layoutChanged();
  frame(0);
}

/** Does what waited for the app to leave the hook. */
function runPending(): void {
  if (stopped) {
    return;
  }
  const waitingLoad = pendingLoad;
  const waitingResize = pendingResize;
  pendingLoad = null;
  pendingResize = null;
  if (waitingResize) resize(waitingResize);
  if (waitingLoad) load(waitingLoad);
}

function request(message: Extract<ToWorker, {kind: 'request'}>): void {
  const asked = message.request;
  let answer: Record<string, any>;
  if (asked.do === 'resume') {
    if (paused) {
      appDebugger.resumed();
      lastFrameAt = performance.now();
    }
    paused = false;
    stopped = false;
    answer = {};
  } else if (asked.do === 'pause') {
    if (!stopped && !paused) {
      paused = true;
      appDebugger.halted();
      post({kind: 'state', stopped: true});
      post({kind: 'event', event: 'stopped', body: {reason: 'pause'}});
    }
    answer = {};
  } else if (asked.do === 'outline') {
    // The page draws it over the panel.
    const askedRect: unknown = asked.rect;
    const rect =
      Array.isArray(askedRect) && askedRect.length === 4
        ? (askedRect.map(Number) as [number, number, number, number])
        : null;
    // Cut to the panel, as a board's is. A node with no size there has none.
    const outline = rect && onPanel(rect);
    post({kind: 'outline', rect: outline});
    answer = {shown: outline !== null};
  } else {
    answer = appDebugger.request(asked);
  }
  post({kind: 'answer', id: message.id, answer});
  if (asked.do === 'resume' && !stopped) {
    post({kind: 'state', stopped: false});
  }
}

function handle(message: ToWorker): void {
  switch (message.kind) {
    case 'load':
      if (stopped) {
        pendingLoad = message;
        output(
          'embedded-react: the app was rebuilt. The new build loads when the app runs on.',
        );
      } else {
        load(message);
      }
      break;
    case 'resize':
      if (stopped) {
        pendingResize = message;
      } else {
        resize(message);
      }
      break;
    case 'touch':
      // A stopped app's panel is frozen.
      if (!stopped && !paused && width > 0) {
        simulator._er_web_aot_touch(message.phase, message.x, message.y);
      }
      break;
    case 'attach':
      attached = message.attached;
      if (!attached) {
        appDebugger.detach();
        if (paused) {
          lastFrameAt = performance.now();
          post({kind: 'state', stopped: false});
        }
        paused = false;
        stopped = false;
      }
      break;
    case 'request':
      request(message);
      break;
  }
}

start.port.on('message', (message: ToWorker) => {
  handle(message);
  runPending();
});

setInterval(() => {
  if (width === 0 || paused) {
    return;
  }
  const now = performance.now();
  const deltaMs = Math.min(Math.round(now - lastFrameAt), LONGEST_FRAME_MS);
  lastFrameAt = now;
  frame(deltaMs);
  runPending();
}, FRAME_MS);

post({kind: 'ready'});
