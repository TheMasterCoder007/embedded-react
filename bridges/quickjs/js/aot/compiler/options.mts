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

import type {Caps, SvgArtifact} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** A panel size, in pixels. */
export interface Screen {
  width: number;
  height: number;
}

/** compileSource's options, as a caller passes them. */
export interface CompileOptions {
  /** The source's path, as errors show it. Defaults to `demos/<demo>/App.jsx`. */
  filename?: string;
  /** The app's <Svg source> files, baked ahead of the compilation by bakeSvgArtifacts. */
  svgArtifacts?: Record<string, SvgArtifact>;
  /** Parse the source as TypeScript. By default, the filename's extension decides. */
  ts?: boolean;
  /** The target panel's size. The fallbacks for this and the caps below are listed on resolveOptions. */
  screen?: Screen | null;
  /** Rows a list state holds. */
  listCap?: number;
  /** Characters per string buffer. */
  listStrCap?: number;
  /** Inline segments in a nested <Text>. */
  maxTextSpans?: number;
}

/** compileSource's options with the screen and the caps filled in. */
export interface ResolvedOptions extends CompileOptions {
  screen: Screen;
  /** Where the screen size came from, for the error footer. */
  screenFrom: 'option' | 'env' | 'default';
  caps: Caps;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * The largest value each size or cap takes; the smallest is 1. Screen dimensions end up in int16_t props, a
 * list's row count and a string buffer's size in a C `int`, and a nested <Text>'s segment count in the
 * engine's uint8_t `span_count`.
 */
const MAX_SCREEN = 32767;
const MAX_LIST_CAP = 32767;
const MAX_LIST_STR_CAP = 32767;
const MAX_TEXT_SPANS = 255;

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Checks that a size or cap is a whole number from 1 to `max`, since it is written into the generated C as a
 * `#define` or an array size.
 *
 * @param value  The value, as the option or the parsed variable gave it.
 * @param name  The option or variable, as the error names it.
 * @param max  The largest value allowed.
 * @param shown  The value as the error shows it. Defaults to `value`.
 *
 * @returns The value, when it is in range.
 */
function wholeInRange(
  value: unknown,
  name: string,
  max: number,
  shown: string = String(value),
): number {
  if (
    typeof value !== 'number' ||
    !Number.isInteger(value) ||
    value < 1 ||
    value > max
  ) {
    throw new Error(
      `AOT: ${name} must be a whole number from 1 to ${max}, got ${shown}`,
    );
  }

  return value;
}

/**
 * compileSource's options with the target screen and the fixed-buffer caps filled in. Each one the caller
 * leaves out falls back to its environment variable, then to a default. The environment is read on every
 * call, not at import, so one process can compile for several boards.
 *
 *   screen        ER_AOT_SCREEN_W / _H    800×480  Baked so a responsive app's `screen.width` branching
 *                                                  folds to the layout for THIS build.
 *   listCap       ER_AOT_LIST_CAP         16       Rows a list state holds. Each pooled row costs engine
 *                                                  nodes, so lower it on a tight-RAM MCU.
 *   listStrCap    ER_AOT_LIST_STR_CAP     48       Characters per string buffer (list field, string state).
 *   maxTextSpans  ER_AOT_MAX_TEXT_SPANS   4        Inline segments in a nested <Text>. Must match the
 *                                                  engine's ER_TEXT_MAX_SPANS.
 *
 * Each value must be a whole number from 1 up: to 32767 for the screen and the list caps, to 255 for
 * maxTextSpans. One that isn't, from the options or a variable, is an `AOT:` error that names it.
 *
 * @param opts  The options the caller passed.
 *
 * @returns The same options with every value filled in.
 */
export function resolveOptions(opts: CompileOptions): ResolvedOptions {
  // An unset or blank variable falls back to the default; any other value must parse to one in range.
  const isSet = (name: string): boolean =>
    (process.env[name] ?? '').trim() !== '';
  const fromEnv = (name: string, fallback: number, max: number): number => {
    if (!isSet(name)) return fallback;
    const raw = process.env[name] as string;
    return wholeInRange(Number(raw), name, max, JSON.stringify(raw));
  };
  const fromOption = (
    value: number | undefined,
    name: string,
    envName: string,
    fallback: number,
    max: number,
  ): number =>
    value != null
      ? wholeInRange(value, name, max)
      : fromEnv(envName, fallback, max);

  return {
    ...opts,
    screen: opts.screen
      ? {
          width: wholeInRange(opts.screen.width, 'screen.width', MAX_SCREEN),
          height: wholeInRange(opts.screen.height, 'screen.height', MAX_SCREEN),
        }
      : {
          width: fromEnv('ER_AOT_SCREEN_W', 800, MAX_SCREEN),
          height: fromEnv('ER_AOT_SCREEN_H', 480, MAX_SCREEN),
        },
    // Where the size came from, for the error footer: 'option', 'env' (both dimensions) or 'default'.
    screenFrom: opts.screen
      ? 'option'
      : isSet('ER_AOT_SCREEN_W') && isSet('ER_AOT_SCREEN_H')
        ? 'env'
        : 'default',
    caps: {
      listCap: fromOption(
        opts.listCap,
        'listCap',
        'ER_AOT_LIST_CAP',
        16,
        MAX_LIST_CAP,
      ),
      listStrCap: fromOption(
        opts.listStrCap,
        'listStrCap',
        'ER_AOT_LIST_STR_CAP',
        48,
        MAX_LIST_STR_CAP,
      ),
      maxTextSpans: fromOption(
        opts.maxTextSpans,
        'maxTextSpans',
        'ER_AOT_MAX_TEXT_SPANS',
        4,
        MAX_TEXT_SPANS,
      ),
    },
  };
}
