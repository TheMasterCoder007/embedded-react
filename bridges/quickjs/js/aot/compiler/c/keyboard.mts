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

import {cstr} from '../c-syntax.mts';
import type {IrKeyboard, IrKeyboardKey, IrKeyKind} from '../ir/keyboard.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** Key kind → its ERKeyboardKey type. */
const KEY_TYPES: Record<IrKeyKind, string> = {
  char: 'ER_KBD_KEY_CHAR',
  layer: 'ER_KBD_KEY_LAYER',
  backspace: 'ER_KBD_KEY_BACKSPACE',
  done: 'ER_KBD_KEY_DONE',
};

/** The highlight layer that means "never highlighted". */
const NEVER_HIGHLIGHTED = 255;

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * An ARGB color as a C literal, or the engine's "use default" 0.
 *
 * @param color  The color, or null.
 *
 * @returns The C.
 */
const colorOrDefault = (color: number | null): string =>
  color == null
    ? '0'
    : '0x' + color.toString(16).padStart(8, '0').toUpperCase() + 'u';

/**
 * A size, or the engine's "use default" 0.
 *
 * @param size  The size, or null.
 *
 * @returns The C.
 */
const sizeOrDefault = (size: number | null): string =>
  size == null ? '0' : String(size);

/**
 * One key as an ERKeyboardKey initializer.
 *
 * @param key  The key.
 *
 * @returns The C.
 */
const printKey = (key: IrKeyboardKey): string =>
  `{ ${key.label == null ? 'NULL' : cstr(key.label)}, ${key.text == null ? 'NULL' : cstr(key.text)}, ` +
  `${KEY_TYPES[key.kind]}, ${key.layer}, ${key.span}, ${key.highlightLayer ?? NEVER_HIGHLIGHTED} }`;

/**
 * A keyboard config as static C tables (a key table per row, a row table per layer, the layer table, and the
 * ERKeyboardConfig) and the er_app_build() line that installs it.
 *
 * @param keyboard  The config.
 *
 * @returns The tables and the setup line.
 */
export function printKeyboard(keyboard: IrKeyboard): {
  data: string;
  setup: string;
} {
  const tables: string[] = [];
  let layersExpr = 'NULL';
  if (keyboard.layers) {
    const layerInits = keyboard.layers.map((layer, layerIndex) => {
      const rowInits = layer.map((row, rowIndex) => {
        const name = `kbd_l${layerIndex}r${rowIndex}`;
        tables.push(
          `static const ERKeyboardKey ${name}[] = { ${row.map(printKey).join(', ')} };`,
        );
        return `{ ${name}, ${row.length} }`;
      });
      tables.push(
        `static const ERKeyboardRow kbd_l${layerIndex}rows[] = { ${rowInits.join(', ')} };`,
      );
      return `{ kbd_l${layerIndex}rows, ${layer.length} }`;
    });
    tables.push(
      `static const ERKeyboardLayer kbd_layers[] = { ${layerInits.join(', ')} };`,
    );
    layersExpr = 'kbd_layers';
  }

  tables.push(
    `static const ERKeyboardConfig kbd_cfg = { ${layersExpr}, ${keyboard.layers?.length ?? 0}, ` +
      `${sizeOrDefault(keyboard.gridCols)}, ${sizeOrDefault(keyboard.rowHeight)}, ` +
      `${sizeOrDefault(keyboard.keyGap)}, ${sizeOrDefault(keyboard.keyRadius)}, ` +
      `${sizeOrDefault(keyboard.fontSize)}, ${colorOrDefault(keyboard.panelColor)}, ` +
      `${colorOrDefault(keyboard.keyColor)}, ` +
      `${colorOrDefault(keyboard.keyActiveColor)}, ${colorOrDefault(keyboard.labelColor)} };`,
  );

  return {
    data: tables.join('\n'),
    setup: '    er_keyboard_set_config(&kbd_cfg);',
  };
}
