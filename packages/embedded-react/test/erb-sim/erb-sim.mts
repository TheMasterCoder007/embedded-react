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

// erb-sim — plays an editor's debug session against the Flow B simulator, with no board and no GDB (`npm run erb:sim`).
//
// It copies the counter fixture to a scratch project and launches `embedded-react debug-adapter` with a simulator
// configuration, which starts `embedded-react dev --aot` there. Then it sets a breakpoint in a handler, taps the
// button through the simulator's page connection as a browser would, and checks the stop, the call stack, the locals
// and state, stepping, setting state, that the panel is frozen at a stop, a save's reload keeping the breakpoint, a
// pause, and a break on a state's change. A second session attaches to a simulator that already runs the list-fault
// fixture, whose handler reads a row its list doesn't have, and checks the stop at the fault.
//
// It needs the prebuilt simulator in sim/ (node tools/web-sim/build.mts).

import {spawn} from 'node:child_process';
import {
  copyFileSync,
  existsSync,
  mkdtempSync,
  readFileSync,
  realpathSync,
  rmSync,
  writeFileSync,
} from 'node:fs';
import {createServer} from 'node:net';
import {tmpdir} from 'node:os';
import {dirname, join, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {readMessages, writeMessage} from '../../debug-adapter.mts';
import type {Message} from '../../debug-adapter.mts';

type Body = Record<string, any>;

const PACKAGE_DIR = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const FIXTURE = join(PACKAGE_DIR, 'test/erb-frames/fixtures/counter.tsx');
const FAULT_FIXTURE = join(
  PACKAGE_DIR,
  'test/erb-frames/fixtures/list-fault.tsx',
);
const WAIT_MS = Number(process.env.ERB_SIM_WAIT_MS) || 60000;
/** The counter's "+" button on its 240x320 layout. */
const PLUS = {x: 122, y: 142};
const TOUCH_DOWN = 0;
const TOUCH_UP = 2;
/** The fault fixture's button. */
const READ = {x: 32, y: 20};
/** How long the panel is watched for a frame it shouldn't draw. */
const FROZEN_MS = 400;
const STATE_REFERENCE = 0x30000000;

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

/** A port nothing listens on. */
function freePort(): Promise<number> {
  return new Promise((found, refused) => {
    const server = createServer();
    server.once('error', refused);
    server.listen(0, () => {
      const address = server.address();
      server.close(() =>
        found(typeof address === 'object' ? address!.port : 0),
      );
    });
  });
}

/** The editor's end of a session with the adapter. */
class Editor {
  readonly transcript: string[] = [];
  /** What the adapter has printed to the debug console. */
  printed = '';
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
        if (message.event === 'output') {
          this.printed += message.body?.output ?? '';
        }
        this.events.push(message);
        this.eventWaiter?.();
      }
    });
  }

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

  /** Waits for the next event that passes a test, taking it and the ones before it. */
  async event(
    name: string,
    passes: (event: Message) => boolean = () => true,
  ): Promise<Message> {
    const deadline = Date.now() + WAIT_MS;
    for (;;) {
      const index = this.events.findIndex(
        event => event.event === name && passes(event),
      );
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

  async stack(): Promise<Body[]> {
    const response = await this.request('stackTrace', {threadId: 1});
    return response.body?.stackFrames ?? [];
  }

  /** A frame's variables, by scope. */
  async variables(frameId: number): Promise<Record<string, Body[]>> {
    const scopes = await this.request('scopes', {frameId});
    const found: Record<string, Body[]> = {};
    for (const scope of scopes.body?.scopes ?? []) {
      const response = await this.request('variables', {
        variablesReference: scope.variablesReference,
      });
      found[scope.name] = response.body?.variables ?? [];
    }
    return found;
  }

  kill(): void {
    this.adapter.kill();
  }
}

/** The simulator's page, as a browser connects. */
class Page {
  frames = 0;
  /** What the server last said the page shows beside the panel. */
  status: Body = {};
  private readonly socket: WebSocket;

  constructor(port: number) {
    this.socket = new WebSocket(`ws://localhost:${port}/ws`);
    this.socket.binaryType = 'arraybuffer';
    this.socket.onmessage = event => {
      if (typeof event.data !== 'string') {
        this.frames++;
      } else {
        const message: Body = JSON.parse(event.data);
        if (message.type === 'status') {
          this.status = message;
        }
      }
    };
  }

  /** Waits until the page's status holds what a test says, or a second has passed. */
  async shows(holds: (status: Body) => boolean): Promise<boolean> {
    for (let waited = 0; waited < 1000 && !holds(this.status); waited += 20) {
      await new Promise(resolveWait => setTimeout(resolveWait, 20));
    }
    return holds(this.status);
  }

  opened(): Promise<void> {
    return new Promise((opened, refused) => {
      this.socket.onopen = () => opened();
      this.socket.onerror = () =>
        refused(new Error('the page did not connect'));
    });
  }

  /** A press and release. */
  tap(at: {x: number; y: number}): void {
    for (const phase of [TOUCH_DOWN, TOUCH_UP]) {
      this.socket.send(JSON.stringify({type: 'touch', phase, ...at}));
    }
  }

  close(): void {
    this.socket.close();
  }
}

const value = (variables: Body[] | undefined, name: string): unknown =>
  variables?.find(variable => variable.name === name)?.value;

async function session(project: string, port: number): Promise<void> {
  const source = join(project, 'App.tsx');
  const lines = readFileSync(source, 'utf8').split('\n');
  const line = (text: string) =>
    lines.findIndex(candidate => candidate.includes(text)) + 1;
  const pressesLine = line('presses.current += 1');
  const setCountLine = line('setCount(count + step)');

  const editor = new Editor();
  let page: Page | null = null;
  try {
    await editor.request('initialize', {
      adapterID: 'embedded-react',
      linesStartAt1: true,
      columnsStartAt1: true,
    });
    const launched = editor.request('launch', {
      simulator: true,
      port,
      cwd: project,
    });
    const located = await editor.event('embedded-react.simulator');
    await editor.event('initialized');
    const launch = await launched;
    check('the launch starts the simulator', launch.success, launch);
    check(
      "the adapter says where the simulator's page is",
      located.body?.url === `http://localhost:${port}/`,
      located.body,
    );

    const set = await editor.request('setBreakpoints', {
      source: {path: source},
      breakpoints: [{line: pressesLine}],
    });
    const breakpoint = set.body?.breakpoints?.[0];
    check(
      'a breakpoint in the handler is verified',
      breakpoint?.verified === true && breakpoint.line === pressesLine,
      set.body,
    );
    await editor.request('setExceptionBreakpoints', {filters: ['erb-faults']});
    await editor.request('configurationDone');
    const threads = await editor.request('threads');
    check(
      'the app is the one thread',
      threads.body?.threads?.length === 1,
      threads.body,
    );

    page = new Page(port);
    await page.opened();
    page.tap(PLUS);
    const stop = await editor.event('stopped');
    check(
      'a tap stops at the breakpoint',
      stop.body?.reason === 'breakpoint' &&
        stop.body.hitBreakpointIds?.[0] === breakpoint?.id,
      stop.body,
    );
    let frames = await editor.stack();
    // The compiler inlines bump() into the handler, so the handler is the one frame.
    check(
      "the stack has the app's frames alone, on the handler's line",
      frames.length === 1 &&
        frames[0].name === 'onPress' &&
        frames[0].line === pressesLine &&
        frames[0].source?.path === source,
      frames,
    );
    let shown = await editor.variables(frames[0].id);
    check(
      'the locals and the state show',
      shown.Locals?.some(local => local.name === 'step') === true &&
        value(shown['App state'], 'count') === '0' &&
        value(shown['App state'], 'label') === '"none yet"',
      shown,
    );
    const hover = await editor.request('evaluate', {
      expression: 'label.length + count * 2',
      frameId: frames[0].id,
      context: 'hover',
    });
    check('an expression works out', hover.body?.result === '8', hover);

    const tree = await editor.request('embedded-react.nodes');
    // The engine's root holds the app's own.
    const root: Body | undefined = tree.body?.nodes?.[0];
    const appRoot: Body | undefined = root?.children[0];
    const plusLine =
      lines.findIndex(text => text.includes('onPress={() => bump(1)}')) - 1;
    const plus: Body | undefined = appRoot?.children
      .find((node: Body) =>
        node.props.some((prop: Body) => prop.value === 'row'),
      )
      ?.children.find((node: Body) => node.source?.line === plusLine);
    check(
      'the node tree has each node with its rect, its element and its props',
      tree.body?.nodes?.length === 1 &&
        root?.type === 'view' &&
        root.source === undefined &&
        appRoot?.props.some(
          (prop: Body) =>
            prop.name === 'backgroundColor' && prop.value === '#182030',
        ) &&
        plus?.type === 'pressable' &&
        plus.source?.path === source &&
        plus.rect.x <= PLUS.x &&
        PLUS.x < plus.rect.x + plus.rect.width &&
        plus.children[0]?.props.some(
          (prop: Body) => prop.name === 'text' && prop.value === '"+"',
        ),
      tree.body ?? tree,
    );

    const sizeless = await editor.request('embedded-react.outline', {
      rect: {x: 0, y: 0, width: 0, height: 0},
    });
    check(
      'a node with no size is not outlined',
      sizeless.body?.shown === false && page.status?.outline === null,
      [sizeless.body, page.status],
    );

    const outlined = await editor.request('embedded-react.outline', {
      rect: plus?.rect,
    });
    check(
      'a node is outlined on the stopped panel',
      outlined.body?.shown === true &&
        (await page.shows(
          status =>
            JSON.stringify(status.outline) ===
            JSON.stringify(plus && Object.values(plus.rect)),
        )),
      [outlined.body, page.status],
    );

    const framesBefore = page.frames;
    await new Promise(resolveWait => setTimeout(resolveWait, FROZEN_MS));
    check(
      'the panel is frozen at the stop',
      page.frames === framesBefore,
      page.frames - framesBefore,
    );

    await editor.request('next', {threadId: 1});
    check(
      'the outline goes when the app runs on',
      await page.shows(status => status.outline === null),
      page.status,
    );
    const stepped = await editor.event('stopped');
    frames = await editor.stack();
    check(
      'step over stops on the next line',
      stepped.body?.reason === 'step' && frames[0]?.line === setCountLine,
      {stop: stepped.body, frames},
    );
    await editor.request('stepOut', {threadId: 1});
    const returned = await editor.event('stopped');
    check(
      'step out stops where the handler returns',
      returned.body?.description === 'onPress returns to the runtime',
      returned.body,
    );

    const tooBig = await editor.request('setVariable', {
      variablesReference: STATE_REFERENCE,
      name: 'count',
      value: '4294967295',
    });
    check(
      'a number too big for a state is refused',
      tooBig.success === false && /whole number/.test(tooBig.message ?? ''),
      tooBig,
    );
    const badEscape = await editor.request('setBreakpoints', {
      source: {path: source},
      breakpoints: [
        {line: pressesLine},
        {line: setCountLine, condition: 'label === "\\u{110000}"'},
      ],
    });
    const refused = badEscape.body?.breakpoints?.[1];
    check(
      'a condition with a bad escape is refused, and the simulator runs on',
      refused?.verified === false &&
        /past the last character/.test(refused.message),
      badEscape.body,
    );
    const edited = await editor.request('setVariable', {
      variablesReference: STATE_REFERENCE,
      name: 'count',
      value: '7',
    });
    check('a state is set', edited.body?.value === '7', edited);
    await editor.request('continue', {threadId: 1});
    for (
      let waited = 0;
      waited < 50 && page.frames === framesBefore;
      waited++
    ) {
      await new Promise(resolveWait => setTimeout(resolveWait, 100));
    }
    check(
      'the panel draws again once the app runs on',
      page.frames > framesBefore,
    );

    // A condition and a logpoint: the count is 7 now, so the second tap is the one that stops.
    await editor.request('setBreakpoints', {
      source: {path: source},
      breakpoints: [
        {line: pressesLine, condition: 'count === 8'},
        {line: setCountLine, logMessage: 'count is {count}'},
      ],
    });
    page.tap(PLUS);
    page.tap(PLUS);
    const conditional = await editor.event('stopped');
    shown = await editor.variables((await editor.stack())[0].id);
    check(
      'a condition picks the pass that stops, and a logpoint prints',
      conditional.body?.reason === 'breakpoint' &&
        value(shown['App state'], 'count') === '8' &&
        editor.printed.includes('count is 7\n') &&
        !editor.printed.includes('count is 8'),
      {stop: conditional.body, printed: editor.printed},
    );
    await editor.request('setBreakpoints', {
      source: {path: source},
      breakpoints: [{line: pressesLine}],
    });
    await editor.request('continue', {threadId: 1});

    // A save: the app reloads, and the breakpoint is in the new build.
    const comment = lines.findIndex(candidate =>
      candidate.includes('const bump'),
    );
    writeFileSync(
      source,
      [
        ...lines.slice(0, comment),
        '  // A line added.',
        ...lines.slice(comment),
      ].join('\n'),
    );
    const moved = await editor.event('breakpoint');
    check(
      'a save reloads the app and keeps the breakpoint',
      moved.body?.breakpoint?.id === breakpoint?.id &&
        moved.body.breakpoint.verified === true &&
        /build [0-9a-f]{8} loaded/.test(editor.printed),
      {event: moved.body, printed: editor.printed},
    );
    page.tap(PLUS);
    const again = await editor.event('stopped');
    frames = await editor.stack();
    check(
      'the breakpoint stops in the new build',
      again.body?.reason === 'breakpoint' && frames[0]?.line === pressesLine,
      {stop: again.body, frames},
    );
    shown = await editor.variables(frames[0].id);
    check(
      'the app started over',
      value(shown['App state'], 'count') === '0',
      shown['App state'],
    );
    await editor.request('setBreakpoints', {
      source: {path: source},
      breakpoints: [],
    });
    await editor.request('continue', {threadId: 1});

    await editor.request('pause', {threadId: 1});
    const paused = await editor.event('stopped');
    frames = await editor.stack();
    shown = await editor.variables(frames[0].id);
    check(
      'a pause stops the app between its events, with its state',
      paused.body?.reason === 'pause' &&
        value(shown['App state'], 'count') === '1',
      {stop: paused.body, shown},
    );

    const info = await editor.request('dataBreakpointInfo', {
      variablesReference: STATE_REFERENCE,
      name: 'count',
    });
    const watched = await editor.request('setDataBreakpoints', {
      breakpoints: [{dataId: info.body?.dataId}],
    });
    check(
      'a state takes a data breakpoint',
      watched.body?.breakpoints?.[0]?.verified === true,
      watched.body,
    );
    await editor.request('continue', {threadId: 1});
    page.tap(PLUS);
    const changed = await editor.event('stopped');
    check(
      "a state's change stops the app",
      changed.body?.reason === 'data breakpoint' &&
        changed.body.description === 'count: 1 → 2',
      changed.body,
    );

    const left = await editor.request('disconnect');
    check('the session disconnects', left.success, left);
    let simulatorRuns = true;
    for (let waited = 0; waited < 50 && simulatorRuns; waited++) {
      await new Promise(resolveWait => setTimeout(resolveWait, 100));
      simulatorRuns = await serves(port);
    }
    check('the simulator the launch started ends with it', !simulatorRuns);
  } catch (error: any) {
    failed++;
    console.log(`✗ the session broke off: ${error.message}`);
    console.log(editor.transcript.slice(-12).join('\n'));
  } finally {
    page?.close();
    editor.kill();
  }
}

/** Whether the simulator's page answers. */
const serves = (port: number): Promise<boolean> =>
  fetch(`http://localhost:${port}/`).then(
    () => true,
    () => false,
  );

async function faultSession(project: string, port: number): Promise<void> {
  const source = join(project, 'App.tsx');
  const simulator = spawn(
    process.execPath,
    [join(PACKAGE_DIR, 'cli.mts'), 'dev', '--aot', '--port', String(port)],
    {cwd: project, stdio: 'ignore'},
  );
  const editor = new Editor();
  let page: Page | null = null;
  try {
    for (let waited = 0; waited < 100 && !(await serves(port)); waited++) {
      await new Promise(resolveWait => setTimeout(resolveWait, 100));
    }
    // A launch owns its simulator, so it doesn't take this one.
    const second = new Editor();
    await second.request('initialize', {adapterID: 'embedded-react'});
    const taken = await second.request('launch', {
      simulator: true,
      port,
      cwd: project,
    });
    second.kill();
    check(
      'a launch refuses a port a simulator already serves',
      taken.success === false && /already in use/.test(taken.message ?? ''),
      taken,
    );
    await editor.request('initialize', {adapterID: 'embedded-react'});
    const attached = editor.request('attach', {
      simulator: `localhost:${port}`,
    });
    await editor.event('initialized');
    check('an attach takes a simulator that runs', (await attached).success);
    await editor.request('setExceptionBreakpoints', {filters: ['erb-faults']});
    await editor.request('configurationDone');

    page = new Page(port);
    await page.opened();
    page.tap(READ);
    const stop = await editor.event('stopped');
    const frames = await editor.stack();
    check(
      "a fault stops as an exception, on the handler's line",
      stop.body?.reason === 'exception' &&
        stop.body.description === 'App fault' &&
        frames[0]?.name === 'onPress' &&
        frames[0].source?.path === source,
      {stop: stop.body, frames},
    );
    const info = await editor.request('exceptionInfo', {threadId: 1});
    check(
      'the fault says why and where',
      /list read past its end/i.test(info.body?.exceptionId ?? '') &&
        /App\.tsx:\d+ {2}onPress/.test(info.body?.description ?? ''),
      info.body,
    );
    await editor.request('continue', {threadId: 1});
    await editor.request('disconnect');
    await new Promise(resolveWait => setTimeout(resolveWait, 500));
    check('the simulator runs on after the session', await serves(port));
  } catch (error: any) {
    failed++;
    console.log(`✗ the fault session broke off: ${error.message}`);
    console.log(editor.transcript.slice(-12).join('\n'));
  } finally {
    page?.close();
    editor.kill();
    simulator.kill();
  }
}

if (!existsSync(join(PACKAGE_DIR, 'sim/embedded-react-aot.wasm'))) {
  console.error(
    'The Flow B simulator is not built: run `node tools/web-sim/build.mts` first.',
  );
  process.exit(1);
}

const project = realpathSync(mkdtempSync(join(tmpdir(), 'erb-sim-')));
copyFileSync(FIXTURE, join(project, 'App.tsx'));
try {
  await session(project, await freePort());
  copyFileSync(FAULT_FIXTURE, join(project, 'App.tsx'));
  await faultSession(project, await freePort());
} finally {
  rmSync(project, {recursive: true, force: true});
}
console.log(failed ? `\n${failed} check(s) failed` : '\nall checks passed');
process.exit(failed ? 1 : 0);
