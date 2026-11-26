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

// debug-guard — leaves a board running when the debug adapter is killed.
//
// An editor may end a session by killing the adapter and every process it started, with no time to resume the
// board. A board that was halted then stays halted. The adapter starts this guard outside its own process tree, and
// the guard runs the GDB server for it. When the adapter goes without saying it left the board, the guard connects a
// GDB of its own, runs the configuration's `disconnectCommands` and disconnects. Either way it ends the server.
//
// It reads its plan as one line of JSON on stdin, and then `left` once the adapter has left the board itself. The
// server's output goes to stdout, and what the guard has to say to stderr, each a line that starts with `@guard `.

import {spawn, type ChildProcess} from 'node:child_process';
import {createInterface} from 'node:readline';
import {fileURLToPath} from 'node:url';
import type {Readable, Writable} from 'node:stream';

/** What the adapter hands the guard as it starts. */
export interface GuardPlan {
  /** The GDB server's program and arguments, when the session starts one. */
  server?: string[];
  cwd?: string;
  environment: NodeJS.ProcessEnv;
  gdb: string;
  /** Where the GDB server listens, as `target remote` takes it. */
  target?: string;
  disconnectCommands: string[];
}

/** The guard as the adapter holds it. */
export interface Guard {
  /** The server's output. */
  stdout: Readable;
  /** Calls the listener with why the server can't be used, if it ends or never starts. */
  onFailure(listener: (failure: string) => void): void;
  /** Ends the guard: `left` when the adapter left the board itself, so nothing is left to rescue. */
  end(left: boolean): void;
}

/** Starts a line of the guard's own on stderr. */
export const GUARD_MARK = '@guard ';
/** The adapter's word that it left the board. */
const LEFT = 'left';
const RESCUE_TRIES = 10;
const RESCUE_WAIT_MS = 500;
const RESCUE_TIMEOUT_MS = 5000;
const SERVER_EXIT_MS = 2000;

/**
 * Starts the guard outside the adapter's process tree.
 *
 * @param plan  The session's server, GDB and commands.
 *
 * @returns The guard.
 */
export function startGuard(plan: GuardPlan): Guard {
  const script = fileURLToPath(
    new URL(
      import.meta.url.endsWith('.mts')
        ? './debug-guard.mts'
        : './debug-guard.mjs',
      import.meta.url,
    ),
  );
  // The shell starts the guard in the background and ends, so the guard is no longer a child of the adapter. A
  // background command's stdin is /dev/null, so the plan goes in on another descriptor.
  const shell = spawn(
    '/bin/sh',
    ['-c', '"$@" <&3 3<&- &', 'sh', process.execPath, script],
    {stdio: ['ignore', 'pipe', 'pipe', 'pipe'], detached: true},
  );
  const control = shell.stdio[3] as Writable;
  control.on('error', () => {});
  control.write(`${JSON.stringify(plan)}\n`);
  let listener: ((failure: string) => void) | null = null;
  createInterface({input: shell.stderr!}).on('line', line => {
    if (line.startsWith(GUARD_MARK)) {
      listener?.(line.slice(GUARD_MARK.length));
    }
  });
  shell.on('error', error => listener?.(error.message));
  return {
    stdout: shell.stdout!,
    onFailure: onFailure => {
      listener = onFailure;
    },
    end: left => {
      listener = null;
      control.end(left ? `${LEFT}\n` : undefined);
    },
  };
}

/**
 * Runs one GDB that resumes the board.
 *
 * @param plan  The session's GDB and commands.
 *
 * @returns Whether GDB connected and ran them.
 */
function rescueOnce(plan: GuardPlan): Promise<boolean> {
  const commands = [
    `target remote ${plan.target}`,
    ...plan.disconnectCommands,
    'disconnect',
  ];
  return new Promise(resolve => {
    const gdb = spawn(
      plan.gdb,
      ['-q', '-nx', '-batch', ...commands.flatMap(command => ['-ex', command])],
      {cwd: plan.cwd, env: plan.environment, stdio: 'ignore'},
    );
    const timer = setTimeout(() => gdb.kill('SIGKILL'), RESCUE_TIMEOUT_MS);
    gdb.on('error', () => {
      clearTimeout(timer);
      resolve(false);
    });
    gdb.on('exit', code => {
      clearTimeout(timer);
      resolve(code === 0);
    });
  });
}

/**
 * Resumes a board the adapter left halted. The adapter's own GDB may still hold the server's one connection for a
 * moment, so this tries more than once.
 *
 * @param plan  The session's GDB and commands.
 */
async function rescue(plan: GuardPlan): Promise<void> {
  if (!plan.target) {
    return;
  }
  for (let attempt = 0; attempt < RESCUE_TRIES; attempt++) {
    if (await rescueOnce(plan)) {
      return;
    }
    await new Promise(resolve => setTimeout(resolve, RESCUE_WAIT_MS));
  }
}

/**
 * Ends the server, by force when it doesn't end when asked.
 *
 * @param server  The server.
 */
function stopServer(server: ChildProcess | null): Promise<void> {
  if (!server || server.exitCode !== null || server.signalCode !== null) {
    return Promise.resolve();
  }
  return new Promise(resolve => {
    const timer = setTimeout(() => server.kill('SIGKILL'), SERVER_EXIT_MS);
    server.on('exit', () => {
      clearTimeout(timer);
      resolve();
    });
    server.kill();
  });
}

/** The guard's own process: runs the server, and waits for the adapter to end. */
async function main(): Promise<void> {
  // The adapter is gone when these are closed, and the guard still has work to do.
  process.stdout.on('error', () => {});
  process.stderr.on('error', () => {});
  for (const signal of ['SIGINT', 'SIGHUP', 'SIGPIPE'] as const) {
    process.on(signal, () => {});
  }
  const say = (failure: string) =>
    process.stderr.write(`${GUARD_MARK}${failure}\n`);

  let plan: GuardPlan | null = null;
  let left = false;
  let server: ChildProcess | null = null;
  const lines = createInterface({input: process.stdin});
  lines.on('line', line => {
    if (plan !== null) {
      left ||= line === LEFT;
      return;
    }
    const read: GuardPlan = JSON.parse(line);
    plan = read;
    if (!read.server) {
      return;
    }
    const [command, ...args] = read.server;
    const started = spawn(command, args, {
      cwd: read.cwd,
      env: read.environment,
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    server = started;
    // Through the guard, so the server never writes to a pipe the adapter has closed.
    started.stdout?.on('data', chunk => process.stdout.write(chunk));
    started.stderr?.on('data', chunk => process.stdout.write(chunk));
    started.on('error', error =>
      say(`${command} could not start: ${error.message}`),
    );
    started.on('exit', code => say(`${command} ended with code ${code}`));
  });
  await new Promise(resolve => lines.on('close', resolve));
  if (plan !== null && !left) {
    await rescue(plan);
  }
  await stopServer(server);
  process.exit(0);
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  await main();
}
