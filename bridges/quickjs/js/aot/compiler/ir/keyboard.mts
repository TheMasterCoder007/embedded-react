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
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** What a key does when tapped. */
export type IrKeyKind = 'char' | 'layer' | 'backspace' | 'done';

/** One on-screen keyboard key. */
export interface IrKeyboardKey {
  kind: IrKeyKind;
  /** The cap's label, or null for none (a space bar). */
  label: string | null;
  /** The text a `char` key types, or null. */
  text: string | null;
  /** The layer a `layer` key switches to (0 otherwise). */
  layer: number;
  /** Grid columns the key spans. */
  span: number;
  /** The layer the key is drawn highlighted on (its own, for a shift key), or null for never. */
  highlightLayer: number | null;
}

/**
 * A module-level `setKeyboardConfig({...})`, folded: an optional layout (layers of rows of keys) and the sizes
 * and colors. A size or color the app leaves out is null, which the engine reads as its default.
 */
export interface IrKeyboard {
  /** The layout, or null to keep the built-in QWERTY. */
  layers: IrKeyboardKey[][][] | null;
  gridCols: number | null;
  rowHeight: number | null;
  keyGap: number | null;
  keyRadius: number | null;
  fontSize: number | null;
  /** ARGB colors. */
  panelColor: number | null;
  keyColor: number | null;
  keyActiveColor: number | null;
  labelColor: number | null;
}
