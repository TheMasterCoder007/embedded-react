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

/** compilePackage's options, as a caller passes them. */
export interface CompileOptions {
  /** The source's path, as errors show it. Defaults to `demos/<demo>/App.jsx`. */
  filename?: string;
  /** The app's <Svg source> files, baked ahead of the compilation by bakeSvgArtifacts. */
  svgArtifacts?: Record<string, SvgArtifact>;
  /** Parse the source as TypeScript. By default, the filename's extension decides. */
  ts?: boolean;
  /** A panel to keep a layout for. The fallbacks for this and the caps below are listed on resolveOptions. */
  screen?: Screen | null;
  /** The sizes a package keeps layouts for. Overrides screen. With neither, a package has every layout. */
  screens?: Screen[];
  /** Rows a list state holds. */
  listCap?: number;
  /** Characters per string buffer. */
  listStrCap?: number;
  /** Inline segments in a nested <Text>. */
  maxTextSpans?: number;
  /** Timers that run at once. */
  maxTimers?: number;
  /** Build the package for release: the debug flag clear, and the debug table left out of it. */
  release?: boolean;
}

/** compilePackage's options with the screen and the caps filled in. */
export interface ResolvedOptions extends CompileOptions {
  screen: Screen;
  /** Where the screen size came from, for the error footer. `search` is a size that picks a layout being found. */
  screenFrom: 'option' | 'search';
  caps: Caps;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * The largest value each size or cap takes; the smallest is 1. Screen dimensions end up in int16_t props, and a
 * nested <Text>'s segment count in the engine's uint8_t `span_count`.
 */
const MAX_SCREEN = 32767;
const MAX_LIST_CAPACITY = 32767;
const MAX_LIST_STRING_CAPACITY = 32767;
const MAX_TEXT_SPANS = 255;
const MAX_TIMERS = 4096;

// What each size or cap is when neither the options nor a variable set it.
const DEFAULT_SCREEN_WIDTH = 800;
const DEFAULT_SCREEN_HEIGHT = 480;
const DEFAULT_LIST_CAPACITY = 16;
const DEFAULT_LIST_STRING_CAPACITY = 48;
const DEFAULT_TEXT_SPANS = 4;
const DEFAULT_TIMERS = 8;

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Checks that a size or cap is a whole number from 1 to `max`, since it sizes what the package holds.
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
 * compilePackage's options with the screen and the fixed-buffer caps filled in. Each one the caller
 * leaves out falls back to its environment variable, then to a default. The environment is read on every
 * call, not at import, so one process can compile for several boards.
 *
 *   screen        ER_AOT_SCREEN_W / _H    800×480  A panel to keep a layout for. A package finds its layouts
 *                                                  unless told a size, with both variables or the option.
 *   listCap       ER_AOT_LIST_CAP         16       Rows a list state holds. Each pooled row costs engine
 *                                                  nodes, so lower it on a tight-RAM MCU.
 *   listStrCap    ER_AOT_LIST_STR_CAP     48       Bytes for a string built from itself (`log + '.'`), with
 *                                                  its terminator. Every other string is sized from its values.
 *   maxTextSpans  ER_AOT_MAX_TEXT_SPANS   4        Inline segments in a nested <Text>. Must match the
 *                                                  engine's ER_TEXT_MAX_SPANS.
 *   maxTimers     ER_AOT_MAX_TIMERS       8        Timers that run at once. Sizes the package's timer
 *                                                  table.
 *
 * Each value must be a whole number from 1 up: to 32767 for the screen and the list caps, to 255 for
 * maxTextSpans, to 4096 for maxTimers. One that isn't, from the options or a variable, is an `AOT:` error
 * that names it.
 *
 * @param options  The options the caller passed.
 *
 * @returns The same options with every value filled in.
 */
export function resolveOptions(options: CompileOptions): ResolvedOptions {
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
    environmentName: string,
    fallback: number,
    max: number,
  ): number =>
    value != null
      ? wholeInRange(value, name, max)
      : fromEnv(environmentName, fallback, max);

  return {
    ...options,
    screen: options.screen
      ? {
          width: wholeInRange(options.screen.width, 'screen.width', MAX_SCREEN),
          height: wholeInRange(
            options.screen.height,
            'screen.height',
            MAX_SCREEN,
          ),
        }
      : {
          width: fromEnv('ER_AOT_SCREEN_W', DEFAULT_SCREEN_WIDTH, MAX_SCREEN),
          height: fromEnv('ER_AOT_SCREEN_H', DEFAULT_SCREEN_HEIGHT, MAX_SCREEN),
        },
    screenFrom: 'option',
    caps: {
      listCap: fromOption(
        options.listCap,
        'listCap',
        'ER_AOT_LIST_CAP',
        DEFAULT_LIST_CAPACITY,
        MAX_LIST_CAPACITY,
      ),
      listStrCap: fromOption(
        options.listStrCap,
        'listStrCap',
        'ER_AOT_LIST_STR_CAP',
        DEFAULT_LIST_STRING_CAPACITY,
        MAX_LIST_STRING_CAPACITY,
      ),
      maxTextSpans: fromOption(
        options.maxTextSpans,
        'maxTextSpans',
        'ER_AOT_MAX_TEXT_SPANS',
        DEFAULT_TEXT_SPANS,
        MAX_TEXT_SPANS,
      ),
      maxTimers: fromOption(
        options.maxTimers,
        'maxTimers',
        'ER_AOT_MAX_TIMERS',
        DEFAULT_TIMERS,
        MAX_TIMERS,
      ),
    },
  };
}
