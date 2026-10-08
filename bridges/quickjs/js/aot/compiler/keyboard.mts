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

import {aotError} from './diagnostics.mts';
import {evalStatic} from './static-eval.mts';
import {cstr} from './c-syntax.mts';
import {argbLiteral} from './style-text.mts';
import type * as t from '@babel/types';
import type {Out} from './out.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * A keyboard-config color → C ARGB literal; null/undefined → "0" (the engine's "use default" sentinel).
 *
 * @param color  The color, if the config gives one.
 *
 * @returns Its C literal.
 */
function kbdColor(color: unknown): string {
  return color == null ? '0' : argbLiteral(color);
}

/**
 * One JS keyboard key object → a C ERKeyboardKey initializer. A key marked `highlight` (e.g., shift) is drawn
 * highlighted while its own layer is showing. Shapes: { char } (types it), { char:' ', span } (space),
 * { label, layer } (switch), { label, backspace }, { label, done }; optional span / highlight.
 *
 * @param key  The key's config object.
 * @param layerIndex  The index of the layer the key is on.
 *
 * @returns The C initializer.
 */
function kbdKeyToC(
  key: Record<string, unknown> | null | undefined,
  layerIndex: number,
): string {
  if (key == null || typeof key !== 'object') {
    throw aotError(
      'AOT: each setKeyboardConfig key must be an object',
      'e.g. { char: "q" } or { label: "shift", layer: 1, highlight: true }',
    );
  }

  // Work out the key's kind from the one action field it carries, with that kind's default label.
  let type;
  let label;
  let text = 'NULL';
  let layer = 0;
  if (key.backspace) {
    type = 'ER_KBD_KEY_BACKSPACE';
    label = key.label ?? '<';
  } else if (key.done) {
    type = 'ER_KBD_KEY_DONE';
    label = key.label ?? 'OK';
  } else if (key.layer != null) {
    type = 'ER_KBD_KEY_LAYER';
    label = key.label ?? '';
    layer = Math.round(Number(key.layer));
  } else if (key.char != null) {
    type = 'ER_KBD_KEY_CHAR';
    text = cstr(String(key.char));
    label = key.label ?? (String(key.char) === ' ' ? '' : String(key.char)); // a space bar shows no label
  } else {
    throw aotError(
      'AOT: a setKeyboardConfig key needs one of char / layer / backspace / done',
    );
  }

  // Span defaults to one grid column; 255 means "never highlighted".
  const span = key.span != null ? Math.round(Number(key.span)) : 1;
  const highlightLayer = key.highlight ? layerIndex : 255;
  return `{ ${label === '' ? 'NULL' : cstr(String(label))}, ${text}, ${type}, ${layer}, ${span}, ${highlightLayer} }`;
}

/**
 * Lowers a module-level `setKeyboardConfig({...})` to static C tables (ERKeyboardKey / Row / Layer and an
 * ERKeyboardConfig) plus an er_keyboard_set_config() call in er_app_build, customizing the on-screen
 * keyboard (colors/sizes, and optionally a full layout) from the app with no engine edit. The config must be
 * statically foldable; omit `layers` to keep the built-in QWERTY.
 *
 * @param program  The parsed module.
 * @param out  Everything emitted so far; the tables and the setup call are added to it.
 */
export function compileKeyboardConfig(program: t.Program, out: Out): void {
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

  if (!configArg) return;

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

  // Emit a key table per row, a row table per layer, and the layer table, when the app gives a layout.
  const tableDecls: string[] = [];
  let layersExpr = 'NULL';
  let layerCount = 0;
  if (Array.isArray(config.layers)) {
    const layerVars: string[] = [];
    config.layers.forEach((layer, layerIndex) => {
      if (!Array.isArray(layer)) {
        throw aotError(
          'AOT: setKeyboardConfig `layers[i]` must be an array of rows',
        );
      }
      const rowVars: string[] = [];
      layer.forEach((row, rowIndex) => {
        if (!Array.isArray(row) || !row.length) {
          throw aotError(
            'AOT: each keyboard row must be a non-empty array of keys',
          );
        }

        tableDecls.push(
          `static const ERKeyboardKey kbd_l${layerIndex}r${rowIndex}[] = { ` +
            `${row.map(key => kbdKeyToC(key, layerIndex)).join(', ')} };`,
        );
        rowVars.push(`{ kbd_l${layerIndex}r${rowIndex}, ${row.length} }`);
      });

      tableDecls.push(
        `static const ERKeyboardRow kbd_l${layerIndex}rows[] = { ${rowVars.join(', ')} };`,
      );
      layerVars.push(`{ kbd_l${layerIndex}rows, ${layer.length} }`);
    });

    tableDecls.push(
      `static const ERKeyboardLayer kbd_layers[] = { ${layerVars.join(', ')} };`,
    );
    layersExpr = 'kbd_layers';
    layerCount = config.layers.length;
  }

  // Emit the config struct (absent sizes and colors are 0, the engine's "use default") and the setup call.
  const intOrZero = (value: unknown) =>
    value == null ? 0 : Math.round(Number(value));
  tableDecls.push(
    `static const ERKeyboardConfig kbd_cfg = { ${layersExpr}, ${layerCount}, ` +
      `${intOrZero(config.gridCols)}, ${intOrZero(config.rowHeight)}, ` +
      `${intOrZero(config.keyGap)}, ${intOrZero(config.keyRadius)}, ` +
      `${intOrZero(config.fontSize)}, ${kbdColor(config.panelColor)}, ` +
      `${kbdColor(config.keyColor)}, ` +
      `${kbdColor(config.keyActiveColor)}, ${kbdColor(config.labelColor)} };`,
  );
  out.kbdData = tableDecls.join('\n');
  out.kbdSetup = '    er_keyboard_set_config(&kbd_cfg);';
}
