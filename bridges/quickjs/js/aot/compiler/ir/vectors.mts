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

import type {VectorGradient} from '../types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** One shape's paint, as the engine stores it. */
export interface IrVectorPaint {
  /** ARGB fill and stroke colors. */
  fill: number;
  stroke: number;
  strokeWidth: number;
  miter: number;
  /** The cap, join and fill rule, as the engine numbers them. */
  cap: number;
  join: number;
  fillRule: number;
  /** 1-based indexes into the drawing's gradient table; 0 paints a solid color. */
  fillGradient: number;
  strokeGradient: number;
}

/**
 * A drawing baked at build time into const tables: an op-tape (opcodes and coordinates, as floats), one paint
 * per shape, and the gradients the paints refer to. An empty tape draws nothing and has no tables.
 */
export interface IrVectorTables {
  /** Numbers the drawing's tables (`s_svg<id>_…`). */
  id: number;
  ops: number[];
  paints: IrVectorPaint[];
  gradients: VectorGradient[];
}
