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

// qjsc-wasm.mjs — compile a JS bundle to QuickJS bytecode using the prebuilt simulator `.wasm` (the SAME
// QuickJS that runs the app in the browser sim), so `embedded-react build` produces a device-ready
// bytecode container with NO native toolchain. The module is built with `-sENVIRONMENT=web,node`, so the
// same artifact that powers `dev` loads under Node here.

import { createRequire } from 'node:module';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { existsSync } from 'node:fs';

const HERE = dirname(fileURLToPath(import.meta.url));

/**
 * Compile JS source to a QuickJS bytecode blob via the prebuilt sim module.
 *
 * @param {string} jsSource  The bundled app (esbuild IIFE output).
 * @param {string} [simDir]  Dir holding embedded-react.{js,wasm} (defaults to the package's sim/).
 * @returns {Promise<Buffer>} The bytecode bytes.
 */
export async function compileToBytecode(jsSource, simDir = resolve(HERE, 'sim')) {
  // The .cjs (not .js): this package is "type": "module", so Node would load the emscripten .js as ESM and
  // its CommonJS factory export would never run. The .cjs is the same module forced to CommonJS for Node.
  const loader = resolve(simDir, 'embedded-react.cjs');
  if (!existsSync(loader)) {
    throw new Error(`prebuilt simulator module not found at ${loader} (a published package ships it; from source run tools/web-sim/build.mjs)`);
  }
  const require = createRequire(import.meta.url);
  const factory = require(loader); // forced CommonJS → MODULARIZE factory
  const Module = await factory();

  const bytes = new TextEncoder().encode(jsSource);
  // QuickJS's JS_Eval requires the source buffer to be NUL-terminated (buf[len] === 0). Allocate one
  // extra byte and zero it: without the terminator the lexer reads an uninitialized sentinel and can
  // stop early ("unexpected token ''") — a heap-state-dependent failure that only bites on larger inputs.
  const srcPtr = Module._malloc(bytes.length + 1);
  Module.HEAPU8.set(bytes, srcPtr);
  Module.HEAPU8[srcPtr + bytes.length] = 0;
  const outLenPtr = Module._malloc(4);

  const compile = Module.cwrap('er_web_compile_bytecode', 'number', ['number', 'number', 'number']);
  const bcPtr = compile(srcPtr, bytes.length, outLenPtr);

  // ALLOW_MEMORY_GROWTH=1 can detach views across the malloc/compile calls — read through a FRESH buffer.
  const heap = () => new Uint8Array(Module.HEAPU8.buffer);
  const bcLen = new DataView(heap().buffer).getUint32(outLenPtr, true);

  Module._free(srcPtr);
  Module._free(outLenPtr);
  if (!bcPtr || !bcLen) {
    if (bcPtr) Module._free(bcPtr);
    throw new Error('bytecode compile failed — check the bundle for a syntax error (see stderr above)');
  }
  const out = Buffer.from(heap().subarray(bcPtr, bcPtr + bcLen)); // copy out before freeing
  Module._free(bcPtr);
  return out;
}
