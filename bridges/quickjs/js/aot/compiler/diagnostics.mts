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

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {codeFrameColumns} from '@babel/code-frame';
import type {Node} from '@babel/types';
import type {ResolvedOptions} from './options.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** A source position as Babel records it: `line` is 1-based, `column` 0-based. */
export interface SourcePosition {
  line: number;
  column: number;
}

/**
 * An error the app's author can act on. Its message starts "AOT:"; `aotLoc` pins it to the construct that
 * caused it, once one is known, and `aotHint` suggests a rewrite.
 */
export interface AotError extends Error {
  aotLoc?: SourcePosition;
  aotHint?: string;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** The location of each node a located emitter is lowering, innermost last; undefined for a node without one. */
const loweringAt: (SourcePosition | undefined)[] = [];

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Where an AOT error thrown now would be reported: the innermost node being lowered that has a location.
 *
 * @returns The location, or undefined outside every located emitter.
 */
export function currentLoc(): SourcePosition | undefined {
  for (let index = loweringAt.length - 1; index >= 0; index--) {
    if (loweringAt[index]) {
      return loweringAt[index];
    }
  }

  return undefined;
}

/**
 * Builds an AOT error carrying an optional `hint` (a "rewrite it like this" suggestion shown to the user).
 *
 * @param message  What is not supported; "AOT: " is prefixed when missing.
 * @param hint  How to rewrite it when there is a known way.
 *
 * @returns The error, for the caller to throw.
 */
export function aotError(message: string, hint?: string): AotError {
  const error: AotError = new Error(
    message.startsWith('AOT:') ? message : `AOT: ${message}`,
  );
  if (hint) {
    error.aotHint = hint;
  }

  return error;
}

/**
 * Pins an AOT error that has no location yet to `loc`, as a located emitter does.
 *
 * @param error  The thrown value.
 * @param loc  Where the construct being lowered is, if it is anywhere.
 */
export function locateError(
  error: unknown,
  loc: SourcePosition | undefined,
): void {
  if (
    error &&
    typeof (error as AotError).message === 'string' &&
    (error as AotError).message.startsWith('AOT:') &&
    !(error as AotError).aotLoc &&
    loc
  ) {
    (error as AotError).aotLoc = loc;
  }
}

/**
 * Wraps an emit function so a thrown AOT error (without a location yet) is tagged with the current node's
 * loc. Wrapped emitters nest, so the deepest node that failed is the one reported. The expression, node, and
 * handler emitters are all wrapped, and compileSource formats the result with formatAotError. While it runs,
 * currentLoc() reports the same location, for a check recorded now and run later.
 *
 * @param fn  The emit function; its first parameter is the node it lowers.
 *
 * @returns The same function, with its AOT errors located.
 */
export function withLoc<
  NodeArg extends Node | null | undefined,
  RestArgs extends unknown[],
  Result,
>(
  fn: (node: NodeArg, ...rest: RestArgs) => Result,
): (node: NodeArg, ...rest: RestArgs) => Result {
  return function (node, ...rest) {
    loweringAt.push(node?.loc?.start);
    try {
      return fn(node, ...rest);
    } catch (error) {
      locateError(error, node?.loc?.start); // babel loc: { line (1-based), column (0-based) }
      throw error;
    } finally {
      loweringAt.pop();
    }
  };
}

/**
 * Rebuilds an AOT error with `file:line:col`, a code-frame, any hint, and the screen size this build folded
 * its layout at (from the resolved `opts`), all folded into the message, for compileSource to throw.
 *
 * @param error  The error compileSource caught.
 * @param src  The app's source, for the code-frame.
 * @param filename  The source's path, as the message should show it.
 * @param opts  The compile's resolved options, for the screen footer.
 *
 * @returns The located error; `error` itself when it has no location.
 */
export function formatAotError(
  error: AotError,
  src: string,
  filename: string,
  opts: ResolvedOptions,
): AotError {
  if (!error || !error.aotLoc) return error; // nothing to locate — leave the bare message

  // Render the code-frame around the error's location, and the hint when there is one.
  const {line, column} = error.aotLoc;
  const frameLoc = {start: {line, column: column + 1}}; // code-frame columns are 1-based
  let codeFrame = '';
  try {
    codeFrame = codeFrameColumns(src, frameLoc, {highlightCode: false});
  } catch {
    /* code-frame is best-effort */
  }

  // Name the screen size the layout was folded at: it picks which branch of a responsive app the error came from.
  const hint = error.aotHint ? `\n\nhint: ${error.aotHint}` : '';
  const size = `${opts.screen.width}×${opts.screen.height}`;
  const screen =
    opts.screenFrom === 'option'
      ? `\n\nscreen: ${size}.`
      : opts.screenFrom === 'env'
        ? `\n\nscreen: ${size} (from ER_AOT_SCREEN_W/H).`
        : `\n\nscreen: ${size} — ER_AOT_SCREEN_W/H did not supply both dimensions, so the ` +
          `default filled in. A responsive app picks its layout from \`screen\`, so this may be compiling a ` +
          `branch meant for another board.`;

  // Build the located error, keeping the location and hint for any caller that inspects them.
  const located: AotError = new Error(
    `${error.message}\n  at ${filename}:${line}:${column + 1}\n\n${codeFrame}${hint}${screen}`,
  );
  located.aotLoc = error.aotLoc;
  if (error.aotHint) {
    located.aotHint = error.aotHint;
  }

  return located;
}
