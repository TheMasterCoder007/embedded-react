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

// The debug adapter's connection to the Flow B simulator (`embedded-react dev --aot`), which stands where GDB does
// for a board. The simulator answers the requests about the app that gdb/erb.py answers there, and this answers the
// few requests the adapter would hand GDB: the simulator runs the app alone, so there is no C to show.

const CONNECT_POLL_MS = 200;
const THREAD = 1;
/** The frame an app that isn't running its code stops in, so the editor has one to show the state under. */
const IDLE_FRAME = 1;

/** A response as the adapter reads GDB's. */
export interface LinkResponse {
  success: boolean;
  message?: string;
  body?: Record<string, any>;
}

interface Waiter {
  resolve(answer: Record<string, any>): void;
  reject(error: Error): void;
}

export class SimulatorLink {
  private readonly address: string;
  private readonly onEvent: (event: string, body: Record<string, any>) => void;
  private readonly onEnd: (reason: string) => void;
  private socket: WebSocket | null = null;
  private nextId = 1;
  private readonly waiting = new Map<number, Waiter>();
  private refusal = '';

  /**
   * @param address  Where the simulator listens: `localhost:3333`.
   * @param onEvent  Called with each of the simulator's events.
   * @param onEnd  Called once when the connection ends, with why.
   */
  constructor(
    address: string,
    onEvent: (event: string, body: Record<string, any>) => void,
    onEnd: (reason: string) => void,
  ) {
    this.address = address;
    this.onEvent = onEvent;
    this.onEnd = onEnd;
  }

  /**
   * Connects, trying until the simulator is up.
   *
   * @param timeoutMs  How long to try.
   */
  async connect(timeoutMs: number): Promise<void> {
    const deadline = Date.now() + timeoutMs;
    for (;;) {
      try {
        await this.open();
        return;
      } catch {
        if (Date.now() > deadline) {
          throw new Error(
            `No simulator at ${this.address}. Start one with: embedded-react dev --aot`,
          );
        }
        await new Promise(resolve => setTimeout(resolve, CONNECT_POLL_MS));
      }
    }
  }

  private open(): Promise<void> {
    return new Promise((resolve, reject) => {
      const socket = new WebSocket(`ws://${this.address}/debug`);
      let opened = false;
      socket.onopen = () => {
        opened = true;
        this.socket = socket;
        resolve();
      };
      socket.onerror = () => {
        if (!opened) reject(new Error('not connected'));
      };
      socket.onclose = () => {
        if (!opened) {
          reject(new Error('not connected'));
          return;
        }
        this.socket = null;
        const reason = this.refusal || 'The simulator ended.';
        for (const waiter of this.waiting.values()) {
          waiter.reject(new Error(reason));
        }
        this.waiting.clear();
        this.onEnd(reason);
      };
      socket.onmessage = event => {
        const message = JSON.parse(String(event.data));
        if (typeof message.id === 'number') {
          const waiter = this.waiting.get(message.id);
          this.waiting.delete(message.id);
          waiter?.resolve(message.answer ?? {});
        } else if (message.event === 'refused') {
          this.refusal = message.body?.message ?? 'The simulator refused.';
        } else if (typeof message.event === 'string') {
          this.onEvent(message.event, message.body ?? {});
        }
      };
    });
  }

  /**
   * Asks the simulator about the app.
   *
   * @param request  The request, named by its `do`, as gdb/erb.py's `erb dap` reads one.
   *
   * @returns Its answer.
   */
  async ask(request: Record<string, any>): Promise<Record<string, any>> {
    const socket = this.socket;
    if (!socket) {
      throw new Error(this.refusal || 'The simulator is not running.');
    }
    const answer = await new Promise<Record<string, any>>((resolve, reject) => {
      const id = this.nextId++;
      this.waiting.set(id, {resolve, reject});
      socket.send(JSON.stringify({id, request}));
    });
    if (answer.error) {
      throw new Error(answer.error);
    }
    return answer;
  }

  /**
   * Answers a request the adapter would hand GDB.
   *
   * @param command  The request.
   * @param args  Its arguments.
   *
   * @returns The response.
   */
  async call(
    command: string,
    args: Record<string, any> = {},
  ): Promise<LinkResponse> {
    const done = (body: Record<string, any> = {}): LinkResponse => ({
      success: true,
      body,
    });
    try {
      switch (command) {
        case 'threads':
          return done({threads: [{id: THREAD, name: 'App'}]});
        case 'continue':
          await this.ask({do: 'resume'});
          return done({allThreadsContinued: true});
        case 'next':
        case 'stepIn':
        case 'stepOut':
          // From a pause: on to the next statement the app runs.
          await this.ask({do: 'step', kind: 'into'});
          await this.ask({do: 'resume'});
          return done();
        case 'pause':
          await this.ask({do: 'pause'});
          return done();
        case 'stackTrace':
          return done({
            stackFrames: [
              {
                id: IDLE_FRAME,
                name: 'Waiting for an event',
                line: 0,
                column: 0,
                presentationHint: 'subtle',
              },
            ],
            totalFrames: 1,
          });
        case 'scopes':
          return done({scopes: []});
        case 'variables':
          return done({variables: []});
        case 'setBreakpoints':
          return done({
            breakpoints: (args.breakpoints ?? []).map(() => ({
              verified: false,
              message: "This isn't one of the app's files.",
            })),
          });
        case 'setFunctionBreakpoints':
        case 'setInstructionBreakpoints':
          return done({
            breakpoints: (args.breakpoints ?? []).map(() => ({
              verified: false,
              message: 'The simulator debugs the app, not C.',
            })),
          });
        case 'setExceptionBreakpoints':
          return done({breakpoints: []});
        case 'configurationDone':
        case 'disconnect':
        case 'terminate':
          return done();
        default:
          return {
            success: false,
            message: `The simulator debugs the app alone, so it has no ${command}.`,
          };
      }
    } catch (error: any) {
      return {success: false, message: error.message};
    }
  }

  close(): void {
    this.socket?.close();
  }
}
