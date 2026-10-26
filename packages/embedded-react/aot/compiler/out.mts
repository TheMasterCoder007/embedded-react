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

import type {Program} from '@babel/types';
import type {
  IrAnimationCallback,
  IrDepEffect,
  IrEffectFunction,
  IrHandler,
  IrMountEffect,
  IrStaticDecl,
  IrTimerFunction,
} from './ir/functions.mts';
import type {IrPanResponder} from './ir/pan-responder.mts';
import type {IrVectorData} from './ir/vectors.mts';
import type {
  AnimRecord,
  FunctionNode,
  RefRecord,
  StateRecord,
} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Classes
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Everything the passes lower for one app besides its element tree: the functions, drawings, responders and
 * effects, and the counters that name them. Each pass appends to it as the JSX tree is walked; writeApp then
 * writes it out as app.gen.c.
 *
 * @class Out
 */
export class Out {
  /** Engine nodes created so far */
  private nodes = 0;
  /** Event handlers, one function each. */
  readonly handlers: IrHandler[] = [];
  /** A useCallback's C handler, by component prefix and name, so each is emitted once. */
  readonly cbEmitted = new Map<string, string>();
  /** File-scope vector op-tapes and paint tables, and the builders of state-driven drawings. */
  readonly vectors: IrVectorData[] = [];
  /** Numbers the next <Svg> (its tapes are `s_svg<id>_…`). */
  svgN = 0;
  /** Whether the generated C calls into <math.h>. */
  needsMath = false;
  readonly timerFns: IrTimerFunction[] = [];
  usesTimers = false;
  /** What er_app_build() runs last: mount-once effects, and the first run of each dep-driven one. */
  readonly mountEffects: IrMountEffect[] = [];
  /** Animations' on_complete callbacks: completions, completion waits, and chains. */
  readonly animCbs: IrAnimationCallback[] = [];
  /** The owner number of each composition that completion waits belong to, by composition; `isStarted` once a start waits. */
  readonly animWaits = new Map<string, {owner: number; isStarted: boolean}>();
  /** Numbers the next animation sequence. */
  seqN = 0;
  /** Image assets the app reaches: asset name → source-relative import path. */
  readonly images = new Map<string, string>();
  /** A dynamic image source was reached, so every imported image has to be baked. */
  bakeAllImages = false;
  /** Numbers the next inlined component instance (its storage prefix is `c<n>_`). */
  instN = 0;
  /** State, refs, and animated values that belong to inlined component instances. */
  readonly childStateRecords: StateRecord[] = [];
  readonly childRefs: RefRecord[] = [];
  readonly childAnimations: AnimRecord[] = [];
  /** Numbers the next effect. */
  effN = 0;
  readonly effectFns: IrEffectFunction[] = [];
  /** File-scope variables the effects and animation loops need. */
  readonly effectDecls: IrStaticDecl[] = [];
  /** Dep-driven effects, which app_update() re-runs when one of their deps changed. */
  readonly depEffects: IrDepEffect[] = [];
  /** Numbers the next PanResponder. */
  panN = 0;
  /** Every PanResponder a node spreads, lowered once each. */
  readonly panResponders: IrPanResponder[] = [];
  /** The module's function components, by name, for inlining. */
  readonly components: Map<string, FunctionNode>;
  /** The parsed module, for the passes that scan it again. */
  readonly program: Program;

  /**
   * @param components  The module's function components, by name, for inlining.
   * @param program  The parsed module, for the passes that scan it again.
   */
  constructor(components: Map<string, FunctionNode>, program: Program) {
    this.components = components;
    this.program = program;
  }

  /** How many engine nodes have been created. */
  get nodeCount(): number {
    return this.nodes;
  }

  /**
   * Reserves the next engine node.
   *
   * @returns Its number: the node is the C variable `n<number>`.
   */
  allocateNodeId(): number {
    return this.nodes++;
  }
}
