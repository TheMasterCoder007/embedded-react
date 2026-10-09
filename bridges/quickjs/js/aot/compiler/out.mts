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
  AnimRecord,
  DynAssign,
  FormatResult,
  FunctionNode,
  RefRecord,
  StateRecord,
  StaticAssign,
} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** A generated C function: `static void <name>(void)` with `body` as its lines. */
export interface CFunction {
  name: string;
  body: string[];
}

/** A timer callback. Its slot is reserved before the body compiles (the body may add timers of its own). */
export interface TimerFunction {
  name: string;
  body: string[] | null;
}

/** A node whose props depend on state: app_update() rebuilds its ERProps and re-applies them. */
export interface NodeUpdate {
  /** The node's variable (`n<id>`); its handle is the file-scope `s_n<id>`. */
  nodeId: string;
  styleAssigns: StaticAssign[];
  dynAssigns: DynAssign[];
  /** A dynamic text body, as one printf format and its arguments. */
  text: FormatResult | null;
  /** A C expression naming the image to show when the source is dynamic. */
  imageName?: string | null;
  /** A static TextInput placeholder. */
  placeholder?: string | null;
}

/** A state-driven <Svg>: app_update() rebuilds its op-tape with build_svg<id>() and uploads it again. */
export interface SvgUpdate {
  id: number;
  len: number;
  nPaints: number;
  nGrads: number;
  /** The node's file-scope handle. */
  nodeVar: string;
}

/** A PanResponder should-set predicate, compiled to a C function that returns its value. */
export interface ResponderQuery {
  name: string;
  expr: string;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Classes
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Everything the passes emit for one app. Each pass appends to it as the JSX tree is walked; compileSourceImpl
 * then lays the pieces out as app.gen.c: file-scope declarations, the builders, er_app_build, app_update, and
 * the event handlers.
 *
 * @class Out
 */
export class Out {
  /** Engine nodes created so far */
  private nodes = 0;
  /** Statements of er_app_build(), in order. */
  build: string[] = [];
  /** Event handlers, one C function each. */
  readonly handlers: CFunction[] = [];
  /** Nodes whose props app_update() re-applies. */
  readonly updates: NodeUpdate[] = [];
  /** Nodes that need a file-scope handle (`static ERNode* s_<v>`), for app_update or an imperative call. */
  readonly handles: string[] = [];
  /** A useCallback's C handler, by component prefix and name, so each is emitted once. */
  readonly cbEmitted = new Map<string, string>();
  /** File-scope vector op-tapes and paint tables. */
  readonly vectorData: string[] = [];
  /** build_svg<id>() functions that recompute a state-driven <Svg>. */
  readonly vectorBuilders: string[] = [];
  readonly svgUpdates: SvgUpdate[] = [];
  /** Numbers the next <Svg> (its tapes are `s_svg<id>_…`). */
  svgN = 0;
  /** Whether the generated C calls into <math.h>. */
  needsMath = false;
  readonly timerFns: TimerFunction[] = [];
  usesTimers = false;
  /** Mount-once effect bodies, inlined at the end of er_app_build(). */
  mountEffects: string[] = [];
  /** on_complete callbacks that chain an animation sequence or loop. */
  readonly animCbs: CFunction[] = [];
  /** Numbers the next animation sequence. */
  seqN = 0;
  /** The on-screen keyboard's static tables and the er_app_build() line that installs them. */
  kbdData = '';
  kbdSetup = '';
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
  readonly effectFns: CFunction[] = [];
  /** File-scope declarations the effects and animation loops need. */
  readonly effectDecls: string[] = [];
  /** app_update() blocks that re-run a dep-driven effect when one of its deps changed. */
  readonly depEffects: string[] = [];
  /** Numbers the next PanResponder. */
  panN = 0;
  /** File-scope state for each PanResponder's gesture. */
  readonly panDecls: string[] = [];
  readonly queries: ResponderQuery[] = [];
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
