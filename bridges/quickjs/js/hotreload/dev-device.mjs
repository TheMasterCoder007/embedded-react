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

// dev-device — the on-device hot-reload dev loop (`embedded-react dev --device <port>`).
//
// Watches the project, re-packs the app to an .erpkg on every save (the same Flow A build the device
// boots from), streams it over the serial/USB port as an ERHR frame, and prints the device's logs that
// come back. The firmware swaps the new config in live — no reflash, no reboot. State written with
// usePersistentState (or plain useState in this dev mode) survives the reload, like the simulator.

import {watch} from 'node:fs';
import {relative} from 'node:path';
import {packApp, emitAppFrame} from './pack-split.mjs';
import {SerialUploader, listSerialPorts} from './uploader.mjs';

const kb = n => `${(n / 1024).toFixed(1)} KB`;

/** Debounce: coalesce a burst of file events (editors often write several) into one rebuild. */
function debounce(fn, ms) {
  let t = null;
  return (...args) => {
    if (t) clearTimeout(t);
    t = setTimeout(() => {
      t = null;
      fn(...args);
    }, ms);
  };
}

/**
 * Run the device hot-reload dev loop until interrupted.
 *
 * @param {object}   o
 * @param {string}   o.entry, o.projectRoot, o.libSrc, o.simDir, o.label, o.device
 * @param {string[]} o.nodePaths
 */
export async function runDeviceDevServer({
  entry,
  projectRoot,
  libSrc,
  nodePaths,
  simDir,
  device,
  label,
}) {
  // Each push ends with exactly one terminal line in the device's log stream:
  //   "hot reload: ok (...)"     → loaded and running
  //   "hot reload: <reason>"     → loaded but the container was rejected (an app/runtime error)
  //   "dropped frame: <reason>"  → the upload was corrupt / overran the device's RX buffer (transport)
  // We watch for it to gate the next send (so a second push can't land while the device is still
  // applying — which would overrun its RX buffer and corrupt that frame) and to resend after a drop.
  let settleOutcome = null;
  const onDeviceLog = line => {
    console.log(`  ▸ ${line}`);
    if (!settleOutcome) return;
    if (/dropped frame/i.test(line)) settleOutcome('dropped');
    else if (/hot reload:\s*ok\b/i.test(line)) settleOutcome('ok');
    else if (/hot reload:/i.test(line)) settleOutcome('rejected');
  };

  const uploader = new SerialUploader({device, onLog: onDeviceLog});

  try {
    await uploader.open();
  } catch (e) {
    console.error(`✗ ${e.message}`);
    const ports = await listSerialPorts();
    if (ports.length) {
      console.error('\nAvailable ports:');
      for (const p of ports)
        console.error(
          `  ${p.path}${p.manufacturer ? `  (${p.manufacturer})` : ''}`,
        );
    }
    process.exit(1);
  }

  console.log(`embedded-react on-device hot reload → ${device}`);
  console.log(
    `watching ${relative(projectRoot, entry) || label} — edit & save to push. Ctrl-C to quit.`,
  );
  console.log(
    '  sending app-only frames; the device must be running a split build (embedded-react build) so the vendor is resident.',
  );

  const APPLY_TIMEOUT_MS = 15000; // a normal reload settles in ~3-4 s; this only catches a stuck device
  const MAX_RESENDS = 2;

  /** Resolves with the device's terminal status ('ok' | 'rejected' | 'dropped' | 'timeout') for the
   * push currently in flight. */
  const nextOutcome = () =>
    new Promise(resolve => {
      let done = false;
      const finish = r => {
        if (done) return;
        done = true;
        settleOutcome = null;
        clearTimeout(timer);
        resolve(r);
      };
      const timer = setTimeout(() => finish('timeout'), APPLY_TIMEOUT_MS);
      settleOutcome = finish; // armed before the send so the terminal log can't be missed
    });

  /** Send a container and wait for the device to apply it, resending on a transport drop/timeout. */
  const sendAndConfirm = async container => {
    for (let attempt = 0; ; attempt++) {
      const outcome = nextOutcome();
      await uploader.send(container);
      const result = await outcome;
      if (result === 'ok' || result === 'rejected') return result;
      if (attempt >= MAX_RESENDS) {
        console.error(
          `✗ reload ${result} after ${attempt + 1} attempts — save again to retry`,
        );
        return result;
      }
      console.log(`↻ ${result}; resending (${attempt + 1}/${MAX_RESENDS})…`);
    }
  };

  // Single-flight: one build→send→apply cycle at a time. A save that lands mid-cycle just re-arms it, so
  // we rebuild and resend the latest source once the current reload settles — pushes never overlap.
  let inFlight = false;
  let queued = false;
  const reloadCycle = async () => {
    if (inFlight) {
      queued = true;
      return;
    }
    inFlight = true;
    try {
      do {
        queued = false;
        const started = Date.now();
        let container, bytecodeLen, assetsLen;
        try {
          const app = await packApp({
            entry,
            projectRoot,
            libSrc,
            nodePaths,
            simDir,
            persist: true,
          });
          container = await emitAppFrame({
            appBytecode: app.bytecode,
            assetPack: app.assetPack,
          });
          bytecodeLen = app.bytecodeLen;
          assetsLen = app.assetsLen;
        } catch (e) {
          console.error(`✗ build failed: ${e.message}`);
          continue;
        }
        const result = await sendAndConfirm(container);
        if (result === 'ok') {
          console.log(
            `↻ reloaded app ${kb(container.length)} (bytecode ${kb(bytecodeLen)}` +
              (assetsLen ? `, assets ${kb(assetsLen)}` : '') +
              `) in ${Date.now() - started} ms`,
          );
        }
      } while (queued);
    } finally {
      inFlight = false;
    }
  };

  // Initial push so the device shows the current app immediately.
  await reloadCycle();

  const onChange = debounce(reloadCycle, 120);
  const watcher = watch(projectRoot, {recursive: true}, (_event, file) => {
    if (!file) return;
    // Ignore churn that can't affect the bundle (build output, deps, VCS, editor temp files).
    if (/(^|\/)(node_modules|dist|\.git)(\/|$)/.test(file)) return;
    if (/\.(erpkg|map)$|~$|\.swp$/.test(file)) return;
    onChange();
  });

  const shutdown = async () => {
    watcher.close();
    await uploader.close();
    process.exit(0);
  };
  process.on('SIGINT', shutdown);
  process.on('SIGTERM', shutdown);
}
