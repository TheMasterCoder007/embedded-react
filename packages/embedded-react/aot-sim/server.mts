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

// The Flow B simulator's dev server (`embedded-react dev --aot`).
//
// It compiles the app to a package on every save and loads it into the Flow B runtime, which runs in a thread of this
// process (worker.mts). A page shows the panel and sends its touches, over a WebSocket at /ws. A debug adapter
// attaches over another at /debug (`embedded-react debug-adapter` with "simulator" in its configuration): its
// requests about the app go to the thread, which answers them from the simulator's memory, at a stop too.

import {readFile} from 'node:fs/promises';
import {createServer} from 'node:http';
import type {IncomingMessage} from 'node:http';
import type {AddressInfo} from 'node:net';
import type {Duplex} from 'node:stream';
import {fileURLToPath} from 'node:url';
import {MessageChannel, Worker} from 'node:worker_threads';
import type {DebugFile} from '../aot/compiler/package/debug-file.mts';
import {watchTree} from '../hotreload/watch-tree.mts';
import {acceptWebSocket} from './websocket.mts';
import type {WebSocketConnection} from './websocket.mts';
import type {FromWorker, ToWorker, WorkerStart} from './worker.mts';

const WATCH_DEBOUNCE_MS = 120;
const RGBA_BYTES = 4;
/** A frame message's header: the panel's width and height, the first row and how many, each 16 bits. */
const FRAME_HEADER_BYTES = 8;
const TOUCH_PHASES = 3;
/** Where the server listens: this machine alone, at the address every `localhost` reaches. */
const LOOPBACK = '127.0.0.1';
/** The longest side a panel may have. */
export const LARGEST_SIDE = 4096;

/** What a build gives the simulator. */
export interface BuiltPackage {
  package: Uint8Array;
  /** The debug file's text. */
  debugFile: string;
}

export interface AotDevOptions {
  /** The app's entry, which the debugger opens. */
  entry: string;
  /** The folder watched for saves. */
  projectRoot: string;
  /** The folder with the prebuilt simulator (embedded-react-aot.cjs and its .wasm). */
  simDir: string;
  /** 0 picks a free port. */
  port: number;
  /** The panel's size, or null for the size the app's first layout was built at. */
  screen: {width: number; height: number} | null;
  /** Builds the package. Throws to report a build error. */
  compile: () => Promise<BuiltPackage>;
  /** Prints a line; console.log when left out. */
  log?: (line: string) => void;
}

export interface AotDevServer {
  port: number;
  close(): Promise<void>;
}

/** What the page shows beside the panel. */
interface Status {
  type: 'status';
  build: string | null;
  /** Why the last build or load failed, or null. */
  error: string | null;
  stopped: boolean;
  debugging: boolean;
  /** The sizes the app's layouts were built at. */
  sizes: {width: number; height: number}[];
}

/** The worker's file beside this one: .mts in the repository, .mjs once published. */
function workerUrl(): URL {
  const extension = import.meta.url.endsWith('.mts') ? '.mts' : '.mjs';
  return new URL(`./worker${extension}`, import.meta.url);
}

/**
 * Runs the Flow B simulator until closed.
 *
 * @param options  The app, where to serve it and how to build it.
 *
 * @returns The server, once the first build has loaded or failed.
 */
export async function runAotDevServer(
  options: AotDevOptions,
): Promise<AotDevServer> {
  const log = options.log ?? console.log;
  const pages = new Set<WebSocketConnection>();
  let adapter: WebSocketConnection | null = null;
  let closing = false;

  const status: Status = {
    type: 'status',
    build: null,
    error: null,
    stopped: false,
    debugging: false,
    sizes: [],
  };
  const sendStatus = () => {
    const text = JSON.stringify(status);
    for (const page of pages) page.send(text);
  };
  const toAdapter = (message: Record<string, any>) =>
    adapter?.send(JSON.stringify(message));
  const say = (line: string) =>
    toAdapter({
      event: 'output',
      body: {category: 'console', output: `embedded-react: ${line}\n`},
    });

  // The whole panel as it was last shown, for a page that connects later.
  let panel = {width: 0, height: 0, pixels: new Uint8Array(0)};
  const frameMessage = (
    top: number,
    rows: number,
    pixels: Uint8Array,
  ): Uint8Array => {
    const message = new Uint8Array(FRAME_HEADER_BYTES + pixels.length);
    const header = new DataView(message.buffer);
    header.setUint16(0, panel.width, true);
    header.setUint16(2, panel.height, true);
    header.setUint16(4, top, true);
    header.setUint16(6, rows, true);
    message.set(pixels, FRAME_HEADER_BYTES);
    return message;
  };

  // ------------------------------------------------------------------------------------------------------------------
  // The simulator's thread
  // ------------------------------------------------------------------------------------------------------------------

  const channel = new MessageChannel();
  const signal = new SharedArrayBuffer(Int32Array.BYTES_PER_ELEMENT);
  const wake = new Int32Array(signal);
  const start: WorkerStart = {
    simDir: options.simDir,
    port: channel.port2,
    signal,
  };
  const worker = new Worker(workerUrl(), {
    workerData: start,
    transferList: [channel.port2],
  });
  const toWorker = (message: ToWorker) => {
    channel.port1.postMessage(message);
    // A thread waiting at a stop sleeps on this.
    Atomics.add(wake, 0, 1);
    Atomics.notify(wake, 0);
  };

  let onLoaded: ((ok: boolean) => void) | null = null;
  const fromWorker = (message: FromWorker) => {
    switch (message.kind) {
      case 'frame': {
        const {width, height, top, rows, pixels} = message;
        if (width !== panel.width || height !== panel.height) {
          panel = {
            width,
            height,
            pixels: new Uint8Array(width * height * RGBA_BYTES),
          };
        }
        panel.pixels.set(pixels, top * width * RGBA_BYTES);
        const frame = frameMessage(top, rows, pixels);
        for (const page of pages) page.send(frame);
        break;
      }
      case 'loaded':
        if (message.ok) {
          status.build = message.build ?? null;
          status.error = null;
          log(`↻ build ${message.build} loaded`);
          toAdapter({
            event: 'reloaded',
            body: {
              build: message.build,
              files: message.files,
              source: message.source,
              breakpoints: message.breakpoints,
            },
          });
        } else {
          status.error = message.error ?? 'the package did not load';
          log(`✗ the package did not load: ${status.error}`);
          say(`the new build did not load: ${status.error}`);
        }
        sendStatus();
        onLoaded?.(message.ok);
        onLoaded = null;
        break;
      case 'log':
        log(`  ▸ ${message.line}`);
        break;
      case 'state':
        status.stopped = message.stopped;
        sendStatus();
        break;
      case 'answer':
        toAdapter({id: message.id, answer: message.answer});
        break;
      case 'event':
        toAdapter({event: message.event, body: message.body});
        break;
      case 'ready':
        break;
    }
  };

  await new Promise<void>((ready, failed) => {
    worker.once('error', failed);
    worker.on('message', (message: FromWorker) => {
      if (message.kind === 'ready') ready();
      fromWorker(message);
    });
  });
  worker.on('error', error => {
    log(`✗ the simulator stopped: ${error.message}`);
    status.error = `the simulator stopped: ${error.message}`;
    sendStatus();
  });

  // ------------------------------------------------------------------------------------------------------------------
  // Builds
  // ------------------------------------------------------------------------------------------------------------------

  let building = false;
  let buildAgain = false;
  const build = async (): Promise<void> => {
    if (building) {
      buildAgain = true;
      return;
    }
    building = true;
    try {
      const built = await options.compile();
      const debugFile: DebugFile = JSON.parse(built.debugFile);
      status.sizes = debugFile.panels.map(({width, height}) => ({
        width,
        height,
      }));
      const first = options.screen ??
        status.sizes[0] ?? {width: 800, height: 480};
      const loaded = new Promise<boolean>(resolve => {
        onLoaded = resolve;
      });
      toWorker({
        kind: 'load',
        package: built.package,
        debugFile,
        sourcePath: options.entry,
        width: first.width,
        height: first.height,
      });
      // A load that arrives at a stop waits for the app to run on, so the next save doesn't wait for it.
      await Promise.race([
        loaded,
        new Promise(resolve => setTimeout(resolve, 1000).unref()),
      ]);
    } catch (error: any) {
      status.error = error?.message || String(error);
      log(`✗ build failed — fix and save to retry\n${status.error}`);
      say(`the build failed:\n${status.error}`);
      sendStatus();
    } finally {
      building = false;
      if (buildAgain && !closing) {
        buildAgain = false;
        await build();
      }
    }
  };

  // ------------------------------------------------------------------------------------------------------------------
  // The page and the debugger
  // ------------------------------------------------------------------------------------------------------------------

  const pageUrl = new URL('./index.html', import.meta.url);
  const server = createServer(async (request, response) => {
    const path = new URL(request.url ?? '/', 'http://localhost').pathname;
    if (path !== '/' && path !== '/index.html') {
      response.writeHead(404).end('not found');
      return;
    }
    try {
      response
        .writeHead(200, {
          'content-type': 'text/html; charset=utf-8',
          'cache-control': 'no-store',
        })
        .end(await readFile(fileURLToPath(pageUrl)));
    } catch {
      response.writeHead(500).end('the simulator page is missing');
    }
  });

  const acceptPage = (connection: WebSocketConnection) => {
    pages.add(connection);
    connection.send(JSON.stringify(status));
    if (panel.width) {
      connection.send(frameMessage(0, panel.height, panel.pixels));
    }
    connection.onClose = () => pages.delete(connection);
    connection.onText = text => {
      let message: Record<string, any>;
      try {
        message = JSON.parse(text);
      } catch {
        return;
      }
      const whole = (value: unknown, most: number) =>
        Number.isInteger(value) &&
        (value as number) >= 0 &&
        (value as number) <= most;
      if (
        message.type === 'touch' &&
        whole(message.phase, TOUCH_PHASES - 1) &&
        Number.isFinite(message.x) &&
        Number.isFinite(message.y)
      ) {
        toWorker({
          kind: 'touch',
          phase: message.phase,
          x: Math.round(message.x),
          y: Math.round(message.y),
        });
      } else if (
        message.type === 'resize' &&
        whole(message.width, LARGEST_SIDE) &&
        whole(message.height, LARGEST_SIDE) &&
        message.width > 0 &&
        message.height > 0
      ) {
        toWorker({
          kind: 'resize',
          width: message.width,
          height: message.height,
        });
      }
    };
  };

  const acceptAdapter = (connection: WebSocketConnection) => {
    if (adapter) {
      connection.send(
        JSON.stringify({
          event: 'refused',
          body: {message: 'Another debugger is attached to the simulator.'},
        }),
      );
      connection.close();
      return;
    }
    adapter = connection;
    status.debugging = true;
    toWorker({kind: 'attach', attached: true});
    sendStatus();
    log('debugger attached');
    connection.onClose = () => {
      adapter = null;
      status.debugging = false;
      toWorker({kind: 'attach', attached: false});
      sendStatus();
      log('debugger detached');
    };
    connection.onText = text => {
      let message: Record<string, any>;
      try {
        message = JSON.parse(text);
      } catch {
        return;
      }
      if (
        typeof message.id === 'number' &&
        typeof message.request?.do === 'string'
      ) {
        toWorker({kind: 'request', id: message.id, request: message.request});
      }
    };
  };

  server.on(
    'upgrade',
    (request: IncomingMessage, socket: Duplex, head: Buffer) => {
      const path = new URL(request.url ?? '/', 'http://localhost').pathname;
      const origin = request.headers.origin;
      // A browser names the page a connection comes from. The debugger's is no page's, and the panel's is this
      // server's own page, so another site's script reaches neither.
      const ownPage =
        typeof origin === 'string' &&
        (() => {
          try {
            return new URL(origin).host === request.headers.host;
          } catch {
            return false;
          }
        })();
      if (path === '/debug' && origin === undefined) {
        const connection = acceptWebSocket(request, socket, head);
        if (connection) acceptAdapter(connection);
      } else if (path === '/ws' && (ownPage || origin === undefined)) {
        const connection = acceptWebSocket(request, socket, head);
        if (connection) acceptPage(connection);
      } else {
        socket.end('HTTP/1.1 403 Forbidden\r\n\r\n');
      }
    },
  );

  // The first build is loaded before anything connects, so a debugger finds the app's files at once.
  await build();
  await new Promise<void>((listening, failed) => {
    server.once('error', failed);
    // This machine alone: the debugger's connection reads and writes the app with no sign-in.
    server.listen(options.port, LOOPBACK, () => listening());
  });
  const port = (server.address() as AddressInfo).port;

  let timer: NodeJS.Timeout | null = null;
  const watcher = watchTree(options.projectRoot, (_event, file) => {
    if (!file) return;
    const changed = file.replace(/\\/g, '/');
    // Churn that can't change the package: build output, dependencies, editors' temporary files.
    if (/(^|\/)(node_modules|dist|\.git)(\/|$)/.test(changed)) return;
    if (/\.(erpkg|erdbg|map)$|~$|\.swp$/.test(changed)) return;
    if (timer) clearTimeout(timer);
    timer = setTimeout(() => {
      timer = null;
      void build();
    }, WATCH_DEBOUNCE_MS);
  });

  return {
    port,
    async close() {
      closing = true;
      if (timer) clearTimeout(timer);
      watcher.close();
      for (const page of pages) page.close();
      adapter?.close();
      await worker.terminate();
      await new Promise<void>(closed => server.close(() => closed()));
    },
  };
}
