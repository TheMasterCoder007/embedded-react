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

import {afterEach, beforeEach, describe, expect, it} from 'vitest';
import {
  chmodSync,
  existsSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {startGuard, type GuardPlan} from '../debug-guard.mts';

const WAIT_MS = 10000;
const POLL_MS = 50;

/**
 * Waits until a condition holds.
 *
 * @param holds  The condition.
 */
async function until(holds: () => boolean): Promise<void> {
  const deadline = Date.now() + WAIT_MS;
  while (!holds()) {
    if (Date.now() > deadline) {
      throw new Error('waited too long');
    }
    await new Promise(resolve => setTimeout(resolve, POLL_MS));
  }
}

/**
 * Whether a process is still there.
 *
 * @param pid  The process.
 *
 * @returns True while it runs.
 */
function alive(pid: number): boolean {
  try {
    process.kill(pid, 0);
    return true;
  } catch {
    return false;
  }
}

// The guard leaves the adapter's process tree through a shell.
describe.skipIf(process.platform === 'win32')('debug guard', () => {
  let folder = '';
  let gdbArguments = '';
  let serverPid = '';
  let plan: GuardPlan;

  beforeEach(() => {
    folder = mkdtempSync(join(tmpdir(), 'erb-guard-'));
    gdbArguments = join(folder, 'gdb-arguments');
    serverPid = join(folder, 'server-pid');
    // A stand-in GDB that records what it was asked to run.
    const gdb = join(folder, 'gdb');
    writeFileSync(gdb, `#!/bin/sh\nprintf '%s\\n' "$@" > "${gdbArguments}"\n`);
    chmodSync(gdb, 0o755);
    plan = {
      // A stand-in server that says it is up and stays.
      server: [
        process.execPath,
        '-e',
        `require('node:fs').writeFileSync(${JSON.stringify(serverPid)}, String(process.pid)); console.log('listening'); setInterval(() => {}, 1000);`,
      ],
      environment: process.env,
      gdb,
      target: 'localhost:3333',
      disconnectCommands: ['monitor resume'],
    };
  });

  afterEach(() => {
    rmSync(folder, {recursive: true, force: true});
  });

  /**
   * Starts the guard and waits for its server.
   *
   * @returns The guard, and the server's process.
   */
  async function started() {
    const guard = startGuard(plan);
    let printed = '';
    guard.stdout.on('data', chunk => {
      printed += String(chunk);
    });
    await until(() => printed.includes('listening') && existsSync(serverPid));
    return {guard, pid: Number(readFileSync(serverPid, 'utf8'))};
  }

  it('resumes the board and ends the server when the adapter goes without leaving the board', async () => {
    const {guard, pid} = await started();
    guard.end(false);
    await until(() => existsSync(gdbArguments) && !alive(pid));
    expect(readFileSync(gdbArguments, 'utf8').trim().split('\n')).toEqual([
      '-q',
      '-nx',
      '-batch',
      '-ex',
      'target remote localhost:3333',
      '-ex',
      'monitor resume',
      '-ex',
      'disconnect',
    ]);
  });

  it('only ends the server when the adapter left the board itself', async () => {
    const {guard, pid} = await started();
    guard.end(true);
    await until(() => !alive(pid));
    expect(existsSync(gdbArguments)).toBe(false);
  });

  it('says when the server ends by itself', async () => {
    plan.server = [process.execPath, '-e', 'process.exit(3)'];
    const guard = startGuard(plan);
    const failure = await new Promise<string>(resolve =>
      guard.onFailure(resolve),
    );
    expect(failure).toBe(`${process.execPath} ended with code 3`);
    guard.end(true);
  });
});
