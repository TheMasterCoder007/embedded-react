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
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

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
 * @param opts  The options the caller passed.
 *
 * @returns The same options with every value filled in.
 */
export function resolveOptions(opts: CompileOptions): ResolvedOptions {
  // A blank or unparsable variable falls back to the default for that value alone.
  const fromEnv = (name: string, fallback: number): number =>
    Number(process.env[name]) || fallback;
  const screenFromEnv =
    Number(process.env.ER_AOT_SCREEN_W) > 0 &&
    Number(process.env.ER_AOT_SCREEN_H) > 0;
  return {
    ...opts,
    screen: opts.screen ?? {
      width: fromEnv('ER_AOT_SCREEN_W', 800),
      height: fromEnv('ER_AOT_SCREEN_H', 480),
    },
    // Where the size came from, for the error footer: 'option', 'env' (both dimensions) or 'default'.
    screenFrom: opts.screen ? 'option' : screenFromEnv ? 'env' : 'default',
    caps: {
      listCap: opts.listCap ?? fromEnv('ER_AOT_LIST_CAP', 16),
      listStrCap: opts.listStrCap ?? fromEnv('ER_AOT_LIST_STR_CAP', 48),
      maxTextSpans: opts.maxTextSpans ?? fromEnv('ER_AOT_MAX_TEXT_SPANS', 4),
    },
  };
}
