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

import type {IrCheck} from '../check.mts';
import type {Out} from '../out.mts';
import type {
  AnimRecord,
  AssetImport,
  RefRecord,
  StateRecord,
} from '../types.mts';
import type {IrKeyboard} from './keyboard.mts';
import type {IrElement} from './nodes.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** A Flow B app, lowered: what the C backend writes app.gen.c and app.gen.h from. */
export interface IrApp {
  /** The app's state, then each inlined component instance's. */
  state: StateRecord[];
  /** The app's refs, then each instance's. */
  refs: RefRecord[];
  /** The app's animated values, then each instance's. */
  animations: AnimRecord[];
  /** The on-screen keyboard's config, or null to keep the engine's own. */
  keyboard: IrKeyboard | null;
  /** The root element, which er_app_build() appends to the screen-sized root. */
  root: IrElement;
  /** Something has to be re-applied after a state change, so the app has an app_update(). */
  hasUpdate: boolean;
  /** The image imports the app reaches, which the CLI bakes. */
  images: AssetImport[];
  /** The values lowering recorded for the check pass, in order. */
  checks: IrCheck[];
  /** Everything else the tree and its effects emitted. */
  out: Out;
}
