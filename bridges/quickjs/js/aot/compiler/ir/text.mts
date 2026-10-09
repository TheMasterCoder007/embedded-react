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

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** Text known at compile time, exactly as JS would print it. */
export interface IrTextLiteral {
  kind: 'literal';
  text: string;
}

/** A runtime value, printed the way JS converts it to a string. */
export interface IrTextValue {
  kind: 'value';
  value: IrExpr;
}

/** A boolean operand of a string `+`, printed as "true" or "false". */
export interface IrTextBoolean {
  kind: 'booleanWord';
  value: IrExpr;
}

/** One piece of a text. */
export type IrTextPart = IrTextLiteral | IrTextValue | IrTextBoolean;

/** Text built from its parts in order: what a <Text> body, a string state or a <TextInput value> holds. */
export interface IrText {
  parts: IrTextPart[];
}
