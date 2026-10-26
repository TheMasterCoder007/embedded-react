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

import type {IrExpr} from './expressions.mts';
import type {IrType} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** A panel dimension, as `screen` names it. */
export type ScreenDimension = 'width' | 'height';

/** What a boot slot is set from: a panel dimension, or an expression over earlier slots and constants. */
export type IrBootInit =
  | {kind: 'screen'; dimension: ScreenDimension}
  | {kind: 'expr'; expr: IrExpr};

/** A value worked out once in er_app_build(), from the panel size: one field of the s_boot struct. */
export interface IrBootSlot {
  /** Its field of the s_boot struct. */
  field: string;
  /** A bool, an int, a float or a string. */
  type: IrType;
  init: IrBootInit;
}

/**
 * Something the build folded at its own panel, which the panel at boot has to give too: a slot's value, or,
 * for a value boot cannot work out, the panel size itself.
 */
export type IrBootAssumption =
  | {kind: 'value'; slot: IrBootSlot; value: number | string | boolean}
  | {kind: 'panel'};

/** The values an app works out at boot, in the order they are set, and what its build assumed of them. */
export interface IrBoot {
  slots: IrBootSlot[];
  assumptions: IrBootAssumption[];
}
