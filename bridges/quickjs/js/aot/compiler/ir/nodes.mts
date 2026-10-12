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

import type {AnimBind, TextSpan} from '../style-text.mts';
import type {ListState, PanResponderRecord, StaticAssign} from '../types.mts';
import type {IrExpr} from './expressions.mts';
import type {IrText} from './text.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** When a node built under a state-driven conditional shows; it exists either way. */
export type IrShow =
  /** `cond && <X/>`, or a branch of `cond ? <A/> : <B/>` (the else branch is negated). */
  | {kind: 'condition'; test: IrExpr; negate: boolean}
  /** A row of a list's fixed pool, shown while the list holds that many items. */
  | {kind: 'rowInUse'; row: number; list: ListState};

/** A state-driven value written to an ERProps field on every app_update. */
export type IrPropValue =
  /** A constant already in its C form, as the style lowering and element defaults write it. */
  | {kind: 'constant'; code: string}
  /** One of two values by a condition: a color or enum ternary. */
  | {
      kind: 'select';
      test: IrExpr;
      whenTrue: IrPropValue;
      whenFalse: IrPropValue;
    }
  /** An opacity in 0..1, scaled to the engine's 0..255. */
  | {kind: 'opacity'; value: IrExpr}
  /** A size or offset, snapped to a whole pixel. */
  | {kind: 'dimension'; value: IrExpr}
  /** `visible={expr}`: shown while the value is truthy. */
  | {kind: 'visible'; value: IrExpr}
  /** The display toggle of a node built under a state-driven conditional. */
  | {kind: 'shown'; show: IrShow}
  /** C from an element that does not lower to the IR yet. */
  | {kind: 'foreign'; code: string};

/** An image to show: a name known at build time, or C from the image element that names one from state. */
export type IrImageName =
  | {kind: 'static'; name: string}
  | {kind: 'foreign'; code: string};

/** An engine event wired to a C handler that lowering already added to the output. */
export interface IrNodeEvent {
  /** The engine event, an ER_EVENT_* name. */
  event: string;
  handler: string;
}

/**
 * A host element (View, Text, Pressable, TouchableOpacity, Image, ScrollView) as an engine node. A node with any
 * state-driven content keeps a file-scope handle, and app_update sets all of its props again.
 */
export interface IrNode {
  /** The node's variable (`n<id>`); its handle is `s_n<id>`. */
  id: string;
  /** The engine node type, an ER_NODE_* name. */
  type: string;
  /** Props folded at build time. */
  constantProps: StaticAssign[];
  /** Props state drives, set again on every app_update after the constant ones. */
  stateProps: {field: string; value: IrPropValue}[];
  /** A <Text>'s body. */
  text: IrText | null;
  image: IrImageName | null;
  /** A <Text> with nested <Text> children draws these inline spans instead of a body. */
  spans: TextSpan[] | null;
  /** Animated values the native driver moves, the press feedback's last. */
  binds: AnimBind[];
  /** Node refs that hold the node's handle. */
  refs: string[];
  events: IrNodeEvent[];
  /** PanResponders spread onto the node; their functions are already in the output. */
  responders: PanResponderRecord[];
}

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * A prop value an element that does not lower to the IR yet has already written as C.
 *
 * @param code  The C.
 *
 * @returns The value.
 */
export const foreignProp = (code: string): IrPropValue => ({
  kind: 'foreign',
  code,
});
