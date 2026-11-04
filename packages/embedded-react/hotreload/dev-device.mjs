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
//
// With a compilePackage function it is the Flow B loop (`--device --aot`): each save compiles a package and sends it
// as a PackageTransfer, and the app starts over, since a package's state doesn't carry across yet.

import {relative} from 'node:path';
import {PackageTransfer} from './package-transfer.mjs';
import {packApp, emitAppFrame} from './pack-split.mjs';
import {SerialUploader, listSerialPorts} from './uploader.mjs';
import {watchTree} from './watch-tree.mjs';

const BYTES_PER_KB = 1024;
const WATCH_DEBOUNCE_MS = 120;
const LIVENESS_CHECKS = 30;
const LIVENESS_INTERVAL_MS = 100;

const kb = bytes => `${(bytes / BYTES_PER_KB).toFixed(1)} KB`;

/** Debounce: coalesce a burst of file events (editors often write several) into one rebuild. */
function debounce(callback, delayMs) {
  let timer = null;
  return (...args) => {
    if (timer) clearTimeout(timer);
    timer = setTimeout(() => {
      timer = null;
      callback(...args);
    }, delayMs);
  };
}

/**
 * Run the device hot-reload dev loop until interrupted.
 *
 * @param {object}   options
 * @param {string}   options.entry, options.projectRoot, options.libSrc, options.simDir, options.label, options.device
 * @param {string[]} options.nodePaths
 * @param {() => Promise<Buffer>} [options.compilePackage]  Flow B: builds the package to send. Throws to report a build error.
 */
export async function runDeviceDevServer({
  entry,
  projectRoot,
  libSrc,
  nodePaths,
  simDir,
  device,
  label,
  compilePackage,
}) {
  // Each push ends with exactly one terminal line in the device's log stream:
  //   "hot reload: ok (...)"     → loaded and running
  //   "hot reload: <reason>"     → loaded but the container was rejected (an app/runtime error)
  //   "dropped frame: <reason>"  → the upload was corrupt / overran the device's RX buffer (transport)
  // We watch for it to gate the next send (so a second push can't land while the device is still
  // applying — which would overrun its RX buffer and corrupt that frame) and to resend after a drop.
  let settleOutcome = null;
  // Track whether the board is alive at all (any log line) and whether a hot-reload RECEIVER is actually
  // running (it prints a "hot reload: …" or "dropped frame: …" line per frame). Firmware built WITHOUT
  // ER_HOTRELOAD still prints normal logs but never those — that's how we flag a disabled build below.
  let sawAnyLog = false;
  let sawReceiverAck = false;
  let closing = false; // set on Ctrl-C so we don't surface errors from a write canceled by the shutdown
  let transfer = null;
  const onDeviceLog = line => {
    if (closing) return;
    sawAnyLog = true;
    if (/dropped frame/i.test(line) || /(hot|erb) reload:/i.test(line))
      sawReceiverAck = true;
    if (transfer) {
      if (!transfer.onLine(line)) console.log(`  ▸ ${line}`);
      return;
    }
    console.log(`  ▸ ${line}`);
    if (!settleOutcome) return;
    if (/dropped frame/i.test(line)) settleOutcome('dropped');
    else if (/hot reload:\s*ok\b/i.test(line)) settleOutcome('ok');
    else if (/hot reload:/i.test(line)) settleOutcome('rejected');
  };

  const uploader = new SerialUploader({device, onLog: onDeviceLog});
  if (compilePackage) {
    transfer = new PackageTransfer({
      write: frame => uploader.writeFrame(frame),
    });
  }

  try {
    await uploader.open();
  } catch (error) {
    console.error(`✗ ${error.message}`);
    const ports = await listSerialPorts();
    if (ports.length) {
      console.error('\nAvailable ports:');
      for (const port of ports)
        console.error(
          `  ${port.path}${port.manufacturer ? `  (${port.manufacturer})` : ''}`,
        );
    }
    process.exit(1);
  }

  console.log(`embedded-react on-device hot reload → ${device}`);
  console.log(
    `watching ${relative(projectRoot, entry) || label} — edit & save to push. Ctrl-C to quit.`,
  );
  console.log(
    compilePackage
      ? '  sending Flow B packages; the device must be running a Flow B firmware built with ER_HOTRELOAD=1.'
      : '  sending app-only frames; the device must be running a split build (embedded-react build) so the vendor is resident.',
  );

  const APPLY_TIMEOUT_MS = 15000; // a normal reload settles in ~3-4 s; this only catches a stuck device
  const PROBE_TIMEOUT_MS = 8000; // initial push: shorter, so a hot-reload-disabled board is flagged fast
  const MAX_RESENDS = 2;

  /** Resolves with the device's terminal status ('ok' | 'rejected' | 'dropped' | 'timeout') for the
   * push currently in flight. */
  const nextOutcome = (timeoutMs = APPLY_TIMEOUT_MS) =>
    new Promise(resolve => {
      let done = false;
      const finish = outcome => {
        if (done) return;
        done = true;
        settleOutcome = null;
        clearTimeout(timer);
        resolve(outcome);
      };
      const timer = setTimeout(() => finish('timeout'), timeoutMs);
      settleOutcome = finish; // armed before the send so the terminal log can't be missed
    });

  /** Send a container and wait for the device to apply it, resending on a transport drop/timeout. */
  const sendAndConfirm = async (
    container,
    {
      timeoutMs = APPLY_TIMEOUT_MS,
      maxResends = MAX_RESENDS,
      probe = false,
    } = {},
  ) => {
    for (let attempt = 0; ; attempt++) {
      const outcome = nextOutcome(timeoutMs);
      uploader.send(container).catch(error => {
        if (closing) return;
        if (settleOutcome) settleOutcome('senderr');
        else console.error(`✗ send failed: ${error.message}`);
      });
      const result = await outcome;
      if (result === 'ok' || result === 'rejected') return result;
      // On the initial probe, a timeout / write stall means nothing is consuming frames — don't spin.
      if (probe && (result === 'timeout' || result === 'senderr'))
        return result;
      if (attempt >= maxResends) {
        console.error(
          `✗ reload ${result} after ${attempt + 1} attempts — save again to retry`,
        );
        return result;
      }
      console.log(`↻ ${result}; resending (${attempt + 1}/${maxResends})…`);
    }
  };

  let triedToSend = false; // a first save that doesn't build says nothing about the board

  /** One Flow B cycle: compile a package, send it and report how it went. */
  const reloadPackage = async ({probe}) => {
    const started = Date.now();
    let packageData;
    try {
      packageData = await compilePackage();
    } catch (error) {
      console.error(`✗ build failed: ${error.message}`);
      return;
    }
    triedToSend = true;
    const result = await transfer
      .send(packageData, probe ? {beginTimeoutMs: PROBE_TIMEOUT_MS} : {})
      .catch(error => {
        if (!closing) console.error(`✗ send failed: ${error.message}`);
        return 'senderr';
      });
    if (result === 'ok') {
      console.log(
        `↻ reloaded package ${kb(packageData.length)} in ${Date.now() - started} ms`,
      );
    } else if (result !== 'rejected' && result !== 'senderr') {
      console.error(`✗ reload ${result} — save again to retry`);
    }
  };

  // Single-flight: one build→send→apply cycle at a time. A save that lands mid-cycle just re-arms it, so
  // we rebuild and resend the latest source once the current reload settles — pushes never overlap.
  let inFlight = false;
  let queued = false;
  const reloadCycle = async ({probe = false} = {}) => {
    if (inFlight) {
      queued = true;
      return;
    }
    inFlight = true;
    try {
      let firstSend = probe; // the probe timeout applies to the initial push only
      do {
        queued = false;
        if (compilePackage) {
          await reloadPackage({probe: firstSend});
          firstSend = false;
          continue;
        }
        const started = Date.now();
        let container, bytecodeLength, assetsLength;
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
          bytecodeLength = app.bytecodeLen;
          assetsLength = app.assetsLen;
        } catch (error) {
          console.error(`✗ build failed: ${error.message}`);
          continue;
        }
        triedToSend = true;
        const result = await sendAndConfirm(
          container,
          firstSend ? {timeoutMs: PROBE_TIMEOUT_MS, probe: true} : {},
        );
        firstSend = false;
        if (result === 'ok') {
          console.log(
            `↻ reloaded app ${kb(container.length)} (bytecode ${kb(bytecodeLength)}` +
              (assetsLength ? `, assets ${kb(assetsLength)}` : '') +
              `) in ${Date.now() - started} ms`,
          );
        }
      } while (queued);
    } finally {
      inFlight = false;
    }
  };

  // Before flooding the port with a frame, give the board a moment to prove it's alive (one log line is
  // enough). Flooding a firmware that never drains its RX can congest the link, so we sample liveness
  // first — this is also what tells "hot reload disabled" apart from "dead/wrong port" if no ack comes.
  for (let check = 0; check < LIVENESS_CHECKS && !sawAnyLog; check++)
    await new Promise(resolve => setTimeout(resolve, LIVENESS_INTERVAL_MS));

  await reloadCycle({probe: true});
  if (triedToSend && !sawReceiverAck) {
    console.warn('');
    console.warn('⚠ the board never acknowledged the hot-reload frame.');
    if (sawAnyLog) {
      console.warn(
        '  It IS sending logs, so the port is right — but nothing is consuming reload frames.',
      );
      console.warn(
        "  Its firmware likely doesn't have the on-device hot-reload receiver enabled.",
      );
    } else {
      console.warn(
        '  No logs are coming back either — check the USB cable/port, and that the app is running.',
      );
    }
    console.warn('  (still watching — fix it and save again to retry)');
    console.warn('');
  }

  const onChange = debounce(reloadCycle, WATCH_DEBOUNCE_MS);
  const onWatchEvent = (_event, file) => {
    if (!file) return;
    // Ignore churn that can't affect the bundle (build output, deps, VCS, editor temp files).
    if (/(^|\/)(node_modules|dist|\.git)(\/|$)/.test(file)) return;
    if (/\.(erpkg|map)$|~$|\.swp$/.test(file)) return;
    onChange();
  };

  // Recursively watch the project so edits under src/** push too — on every platform, including ones
  // where fs.watch's native {recursive:true} isn't available (older Linux/Node).
  const watcher = watchTree(projectRoot, onWatchEvent);

  const shutdown = async () => {
    closing = true;
    watcher.close();
    await uploader.close();
    process.exit(0);
  };
  process.on('SIGINT', shutdown);
  process.on('SIGTERM', shutdown);
}
