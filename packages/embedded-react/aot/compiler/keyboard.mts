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

import {parseColor} from '../../src/embedded-react/svg-ops.js';
import {aotError, withLoc} from './diagnostics.mts';
import {evalStatic} from './static-eval.mts';
import type * as t from '@babel/types';
import type {IrKeyboard, IrKeyboardKey} from './ir/keyboard.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * A keyboard-config color as ARGB; null when the config leaves it out (the engine's default).
 *
 * @param color  The color, if the config gives one.
 *
 * @returns The color, or null.
 */
const kbdColor = (color: unknown): number | null =>
  color == null ? null : parseColor(String(color)) >>> 0;

/**
 * A keyboard-config size, rounded; null when the config leaves it out (the engine's default).
 *
 * @param size  The size, if the config gives one.
 *
 * @returns The size, or null.
 */
const kbdSize = (size: unknown): number | null =>
  size == null ? null : Math.round(Number(size));

/**
 * Lowers one JS keyboard key object. A key marked `highlight` (e.g., shift) is drawn highlighted while its own
 * layer is showing. Shapes: { char } (types it), { char:' ', span } (space), { label, layer } (switch),
 * { label, backspace }, { label, done }; optional span / highlight.
 *
 * @param key  The key's config object.
 * @param layerIndex  The index of the layer the key is on.
 *
 * @returns The key.
 */
function lowerKey(
  key: Record<string, unknown> | null | undefined,
  layerIndex: number,
): IrKeyboardKey {
  if (key == null || typeof key !== 'object') {
    throw aotError(
      'AOT: each setKeyboardConfig key must be an object',
      'e.g. { char: "q" } or { label: "shift", layer: 1, highlight: true }',
    );
  }

  // Work out the key's kind from the one action field it carries, with that kind's default label.
  let kind: IrKeyboardKey['kind'];
  let label;
  let text: string | null = null;
  let layer = 0;
  if (key.backspace) {
    kind = 'backspace';
    label = key.label ?? '<';
  } else if (key.done) {
    kind = 'done';
    label = key.label ?? 'OK';
  } else if (key.layer != null) {
    kind = 'layer';
    label = key.label ?? '';
    layer = Math.round(Number(key.layer));
  } else if (key.char != null) {
    kind = 'char';
    text = String(key.char);
    label = key.label ?? (String(key.char) === ' ' ? '' : String(key.char)); // a space bar shows no label
  } else {
    throw aotError(
      'AOT: a setKeyboardConfig key needs one of char / layer / backspace / done',
    );
  }

  // Span defaults to one grid column.
  return {
    kind,
    label: label === '' ? null : String(label),
    text,
    layer,
    span: key.span != null ? Math.round(Number(key.span)) : 1,
    highlightLayer: key.highlight ? layerIndex : null,
  };
}

/**
 * Lowers the argument of a `setKeyboardConfig(...)` call, its AOT errors located at it.
 *
 * @param configArg  The call's config argument.
 *
 * @returns The config.
 */
const lowerConfig = withLoc((configArg: t.Node): IrKeyboard => {
  // Fold the config object at compile time.
  let config;
  try {
    config = evalStatic(configArg, {}) as
      | Record<string, unknown>
      | null
      | undefined;
  } catch {
    throw aotError(
      'AOT: setKeyboardConfig(...) needs a statically-foldable config object',
      'pass an object literal of colors/sizes (+ an optional `layers` array) — no state or runtime values.',
    );
  }

  if (config == null || typeof config !== 'object') {
    throw aotError('AOT: setKeyboardConfig(...) needs a config object');
  }

  // Lower the layout when the app gives one, layer by layer and row by row; C has no empty table.
  let layers: IrKeyboardKey[][][] | null = null;
  if (Array.isArray(config.layers)) {
    if (!config.layers.length) {
      throw aotError(
        'AOT: setKeyboardConfig `layers` needs at least one layer',
        'omit `layers` to keep the built-in QWERTY.',
      );
    }

    layers = config.layers.map((layer, layerIndex) => {
      if (!Array.isArray(layer)) {
        throw aotError(
          'AOT: setKeyboardConfig `layers[i]` must be an array of rows',
        );
      }

      if (!layer.length) {
        throw aotError(
          `AOT: setKeyboardConfig \`layers[${layerIndex}]\` needs at least one row`,
        );
      }

      return layer.map(row => {
        if (!Array.isArray(row) || !row.length) {
          throw aotError(
            'AOT: each keyboard row must be a non-empty array of keys',
          );
        }

        return row.map(key => lowerKey(key, layerIndex));
      });
    });
  }

  return {
    layers,
    gridCols: kbdSize(config.gridCols),
    rowHeight: kbdSize(config.rowHeight),
    keyGap: kbdSize(config.keyGap),
    keyRadius: kbdSize(config.keyRadius),
    fontSize: kbdSize(config.fontSize),
    panelColor: kbdColor(config.panelColor),
    keyColor: kbdColor(config.keyColor),
    keyActiveColor: kbdColor(config.keyActiveColor),
    labelColor: kbdColor(config.labelColor),
  };
});

/**
 * Lowers a module-level `setKeyboardConfig({...})`, which customizes the on-screen keyboard (colors/sizes, and
 * optionally a full layout) from the app with no engine edit. The config must be statically foldable; omit
 * `layers` to keep the built-in QWERTY.
 *
 * @param program  The parsed module.
 *
 * @returns The config, or null when the module has none.
 */
export function lowerKeyboardConfig(program: t.Program): IrKeyboard | null {
  // Find the first top-level setKeyboardConfig(...) call; without one, the built-in keyboard stays as is.
  let configArg: t.Node | null = null;
  for (const statement of program.body) {
    if (
      statement.type === 'ExpressionStatement' &&
      statement.expression.type === 'CallExpression' &&
      statement.expression.callee.type === 'Identifier' &&
      statement.expression.callee.name === 'setKeyboardConfig'
    ) {
      configArg = statement.expression.arguments[0];
      break;
    }
  }

  return configArg ? lowerConfig(configArg) : null;
}
