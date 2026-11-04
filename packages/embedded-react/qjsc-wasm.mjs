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

import {createRequire} from 'node:module';
import {dirname, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {existsSync} from 'node:fs';

const HERE = dirname(fileURLToPath(import.meta.url));
const UINT32_BYTES = 4;

/**
 * Compile JS source to a QuickJS bytecode blob via the prebuilt sim module.
 *
 * @param {string} jsSource  The bundled app (esbuild IIFE output).
 * @param {string} [simDir]  Dir holding embedded-react.{js,wasm} (defaults to the package's sim/).
 * @param {{strip?: boolean}} [options]  strip: true drops the embedded source text + debug tables
 *   (release/device — ~8x smaller blob); default false keeps them so dev-loop stack traces have
 *   line numbers. (A sim .wasm built before the strip parameter existed ignores it and always
 *   keeps debug info.)
 * @returns {Promise<Buffer>} The bytecode bytes.
 */
export async function compileToBytecode(
  jsSource,
  simDir = resolve(HERE, 'sim'),
  {strip = false} = {},
) {
  // The .cjs (not .js): this package is "type": "module", so Node would load the emscripten .js as ESM and
  // its CommonJS factory export would never run. The .cjs is the same module forced to CommonJS for Node.
  const loader = resolve(simDir, 'embedded-react.cjs');
  if (!existsSync(loader)) {
    throw new Error(
      `prebuilt simulator module not found at ${loader} (a published package ships it; from source run tools/web-sim/build.mjs)`,
    );
  }
  const require = createRequire(import.meta.url);
  const factory = require(loader); // forced CommonJS → MODULARIZE factory
  const Module = await factory();

  const bytes = new TextEncoder().encode(jsSource);
  // QuickJS's JS_Eval requires the source buffer to be NUL-terminated (buf[len] === 0). Allocate one
  // extra byte and zero it: without the terminator the lexer reads an uninitialized sentinel and can
  // stop early ("unexpected token ''") — a heap-state-dependent failure that only bites on larger inputs.
  const sourcePointer = Module._malloc(bytes.length + 1);
  Module.HEAPU8.set(bytes, sourcePointer);
  Module.HEAPU8[sourcePointer + bytes.length] = 0;
  const outputLengthPointer = Module._malloc(UINT32_BYTES);

  const compile = Module.cwrap('er_web_compile_bytecode', 'number', [
    'number',
    'number',
    'number',
    'number',
  ]);
  const bytecodePointer = compile(
    sourcePointer,
    bytes.length,
    outputLengthPointer,
    strip ? 1 : 0,
  );

  // ALLOW_MEMORY_GROWTH=1 can detach views across the malloc/compile calls — read through a FRESH buffer.
  const heap = () => new Uint8Array(Module.HEAPU8.buffer);
  const bytecodeLength = new DataView(heap().buffer).getUint32(
    outputLengthPointer,
    true,
  );

  Module._free(sourcePointer);
  Module._free(outputLengthPointer);
  if (!bytecodePointer || !bytecodeLength) {
    if (bytecodePointer) Module._free(bytecodePointer);
    throw new Error(
      'bytecode compile failed — check the bundle for a syntax error (see stderr above)',
    );
  }
  const bytecode = Buffer.from(
    heap().subarray(bytecodePointer, bytecodePointer + bytecodeLength),
  ); // copy out before freeing
  Module._free(bytecodePointer);
  return bytecode;
}
