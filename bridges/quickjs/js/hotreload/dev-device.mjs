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
import {packAppContainer} from './pack-app.mjs';
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
  const uploader = new SerialUploader({
    device,
    onLog: line => console.log(`  ▸ ${line}`),
  });

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

  let building = false;
  let pending = false;
  const buildAndSend = async () => {
    if (building) {
      pending = true; // a save landed mid-build; rebuild once this one finishes
      return;
    }
    building = true;
    const started = Date.now();
    try {
      const {container, bytecodeLen, assetsLen} = await packAppContainer({
        entry,
        projectRoot,
        libSrc,
        nodePaths,
        simDir,
        persist: true,
      });
      await uploader.send(container);
      console.log(
        `↻ pushed ${kb(container.length)} (bytecode ${kb(bytecodeLen)}` +
          (assetsLen ? `, assets ${kb(assetsLen)}` : '') +
          `) in ${Date.now() - started} ms`,
      );
    } catch (e) {
      console.error(`✗ build failed: ${e.message}`);
    } finally {
      building = false;
      if (pending) {
        pending = false;
        buildAndSend();
      }
    }
  };

  // Initial push so the device shows the current app immediately.
  await buildAndSend();

  const onChange = debounce(buildAndSend, 120);
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
