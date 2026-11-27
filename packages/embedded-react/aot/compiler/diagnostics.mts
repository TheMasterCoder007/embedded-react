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
import type {IrText, IrTextLiteral} from './ir/text.mts';
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
  /** The file `aotLoc` is in, when it is one the entry imports: its path from the entry's folder. */
  aotFile?: {path: string; source: string};
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

/** Where each function component being expanded is used, outermost first. */
const componentUses: SourcePosition[] = [];

/**
 * Where the function components a node is made inside are used: `<Card />`'s line for a node of Card's body.
 *
 * @returns The places, outermost first; empty for a node of the root component's own.
 */
export function currentUses(): SourcePosition[] {
  return [...componentUses];
}

/**
 * Runs the expansion of a function component, so the nodes it makes know where it is used.
 *
 * @param use  Where the component's element is.
 * @param expand  The expansion.
 *
 * @returns What the expansion returns.
 */
export function withComponentUse<Result>(
  use: SourcePosition | undefined,
  expand: () => Result,
): Result {
  if (!use) {
    return expand();
  }
  componentUses.push(use);
  try {
    return expand();
  } finally {
    componentUses.pop();
  }
}

/**
 * Lowers elements written where a component is used, such as the children it is given, so they are the caller's
 * and not the component's.
 *
 * @param uses  The uses the caller's own nodes have, from currentUses() at the call.
 * @param lower  The lowering.
 *
 * @returns What the lowering returns.
 */
export function withUses<Result>(
  uses: SourcePosition[],
  lower: () => Result,
): Result {
  const inside = componentUses.splice(0, componentUses.length, ...uses);
  try {
    return lower();
  } finally {
    componentUses.splice(0, componentUses.length, ...inside);
  }
}

/** The most bytes each of the engine's fixed string buffers holds, as er_scene.h defines them. */
export const ENGINE_STRING_MAX = {
  ER_TEXT_MAX: 255,
  ER_SPAN_TEXT_MAX: 63,
  ER_PLACEHOLDER_MAX: 63,
  ER_IMAGE_NAME_MAX: 63,
  ER_FONT_FAMILY_MAX: 31,
} as const;

/**
 * A text's string when every part of it is known at compile time.
 *
 * @param text  The text.
 *
 * @returns The string, or null when a part is a run-time value.
 */
export const constantText = (text: IrText): string | null =>
  text.parts.every(part => part.kind === 'literal')
    ? text.parts.map(part => (part as IrTextLiteral).text).join('')
    : null;

/**
 * Refuses a constant string too long for the engine buffer it is copied into, which would cut it short.
 *
 * @param text  The string, or null for one built at run time (checked when it is written).
 * @param limit  The er_scene.h define that sizes the buffer.
 * @param what  What the string is, for the error.
 */
export function checkEngineString(
  text: string | null,
  limit: keyof typeof ENGINE_STRING_MAX,
  what: string,
): void {
  if (text === null) {
    return;
  }

  const bytes = Buffer.byteLength(text, 'utf8');
  if (bytes > ENGINE_STRING_MAX[limit]) {
    throw aotError(
      `AOT: ${what} is ${bytes} bytes, but the engine holds ${ENGINE_STRING_MAX[limit]} (${limit})`,
      `shorten it: ${limit} in er_scene.h sizes the engine's buffer.`,
    );
  }
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
 * Pins an AOT error that has no location yet to `location`, as a located emitter does.
 *
 * @param error  The thrown value.
 * @param location  Where the construct being lowered is, if it is anywhere.
 */
export function locateError(
  error: unknown,
  location: SourcePosition | undefined,
): void {
  if (
    error &&
    typeof (error as AotError).message === 'string' &&
    (error as AotError).message.startsWith('AOT:') &&
    !(error as AotError).aotLoc &&
    location
  ) {
    (error as AotError).aotLoc = location;
  }
}

/**
 * Wraps an emit function so a thrown AOT error (without a location yet) is tagged with the current node's
 * loc. Wrapped emitters nest, so the deepest node that failed is the one reported. The expression, node, and
 * handler emitters are all wrapped, and compilePackage formats the result with formatAotError. While it runs,
 * currentLoc() reports the same location, for a check recorded now and run later.
 *
 * @param lower  The emit function; its first parameter is the node it lowers.
 *
 * @returns The same function, with its AOT errors located.
 */
export function withLoc<
  NodeArg extends Node | null | undefined,
  RestArgs extends unknown[],
  Result,
>(
  lower: (node: NodeArg, ...rest: RestArgs) => Result,
): (node: NodeArg, ...rest: RestArgs) => Result {
  return function (node, ...rest) {
    loweringAt.push(node?.loc?.start);
    try {
      return lower(node, ...rest);
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
 * its layout at (from the resolved `options`), all folded into the message, for compilePackage to throw.
 *
 * @param error  The error compilePackage caught.
 * @param source  The entry's source, for the code-frame.
 * @param filename  The entry's path, as the message should show it. An error in a file it imports names that file.
 * @param options  The compile's resolved options, for the screen footer.
 *
 * @returns The located error; `error` itself when it has no location.
 */
export function formatAotError(
  error: AotError,
  source: string,
  filename: string,
  options: ResolvedOptions,
): AotError {
  if (!error || !error.aotLoc) return error; // nothing to locate — leave the bare message

  // Render the code-frame around the error's location, and the hint when there is one.
  const {line, column} = error.aotLoc;
  if (error.aotFile) {
    source = error.aotFile.source;
    filename = filename.replace(/[^\\/]*$/, '') + error.aotFile.path;
  }
  const frameLocation = {start: {line, column: column + 1}}; // code-frame columns are 1-based
  let codeFrame = '';
  try {
    codeFrame = codeFrameColumns(source, frameLocation, {highlightCode: false});
  } catch {
    /* code-frame is best-effort */
  }

  // Name the screen size the layout was folded at: it picks which branch of a responsive app the error came from.
  const hint = error.aotHint ? `\n\nhint: ${error.aotHint}` : '';
  const size = `${options.screen.width}×${options.screen.height}`;
  const screen =
    options.screenFrom === 'search'
      ? `\n\nscreen: this is the layout a ${size} panel picks. Every layout has to compile; name the panels with ` +
        `--screen to drop the others.`
      : `\n\nscreen: ${size}.`;

  // Build the located error, keeping the location and hint for any caller that inspects them.
  const located: AotError = new Error(
    `${error.message}\n  at ${filename}:${line}:${column + 1}\n\n${codeFrame}${hint}${screen}`,
  );
  located.aotLoc = error.aotLoc;
  if (error.aotFile) {
    located.aotFile = error.aotFile;
  }
  if (error.aotHint) {
    located.aotHint = error.aotHint;
  }

  return located;
}
