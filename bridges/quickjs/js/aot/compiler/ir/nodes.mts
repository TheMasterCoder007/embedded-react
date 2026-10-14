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
import type {ListState, StaticAssign} from '../types.mts';
import type {IrExpr} from './expressions.mts';
import type {IrPanResponder} from './pan-responder.mts';
import type {IrText} from './text.mts';
import type {IrStateDrivenVector, IrVectorTables} from './vectors.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** When a node built under a state-driven conditional shows; it exists either way. */
export type IrShow =
  /**
   * `cond && <X/>`, or a branch of `cond ? <A/> : <B/>` (the else branch is negated). A condition nested in
   * another's branch holds only `within` that branch.
   */
  | {kind: 'condition'; test: IrExpr; negate: boolean; within: IrShow | null}
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
  /** `value` while a node built under a state-driven conditional shows, else 0. */
  | {kind: 'whileShown'; show: IrShow; value: IrPropValue}
  /**
   * A byte that is `whenTrue` while the test holds, else 0: a 0/1 flag, or a gradient's stop count. `truthiness`
   * is how C reads the test: as a JS condition (a string is truthy when non-empty), or as the value itself.
   */
  | {
      kind: 'flag';
      test: IrExpr;
      truthiness: 'condition' | 'value';
      negate: boolean;
      whenTrue: number;
    }
  /** A value narrowed to a byte (an enum field stored as uint8_t). */
  | {kind: 'byte'; value: IrPropValue}
  /** A number stored in a float field. */
  | {kind: 'float'; value: IrExpr};

/**
 * An image to show: a name known at build time, or a string read from state or a list item. A node state drives
 * sets its props again on every update; `resetOnUpdate` is false where that loses a constant name (a host
 * <Image>, a known bug).
 */
export type IrImageName =
  | {kind: 'static'; name: string; resetOnUpdate: boolean}
  | {kind: 'value'; value: IrExpr};

/** Something wired onto a node once it exists, in the order the element wires it. */
export type IrWire =
  /** An animated value the native driver moves; `index` numbers its interpolation table. */
  | {kind: 'bind'; bind: AnimBind; index: number}
  /** A node ref that holds the node's handle. */
  | {kind: 'ref'; slot: string}
  /** An engine event (an ER_EVENT_* name) and the handler, from the app's handlers, that it runs. */
  | {kind: 'event'; event: string; handler: string}
  /** A PanResponder spread onto the node. */
  | {kind: 'responder'; pan: IrPanResponder}
  /** An <Svg>'s drawing, baked into const tables. */
  | {kind: 'vectorTables'; tables: IrVectorTables}
  /** An <Svg>'s drawing that state drives: built once at boot and kept as a handle for app_update. */
  | {kind: 'stateDrivenVector'; vector: IrStateDrivenVector};

/**
 * An element as an engine node: a host element (View, Text, Pressable, TouchableOpacity, Image, ScrollView), a
 * typed one (Switch, Dial, TextInput, ActivityIndicator, Modal), or an <Svg>'s box. A node with any state-driven
 * content keeps a file-scope handle, and app_update sets all of its props again.
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
  /** A <TextInput>'s placeholder. */
  placeholder: string | null;
  /** A <Text>'s body, or a <TextInput>'s value. */
  text: IrText | null;
  image: IrImageName | null;
  /** A <Text> with nested <Text> children draws these inline spans instead of a body. */
  spans: TextSpan[] | null;
  wiring: IrWire[];
  /** The node's children, appended after it is built; none for a <Text>, whose children are its text. */
  children: IrChild[];
}

/** What a JSX element lowered to: an engine node, or a function component inlined in place, wrapping its root. */
export type IrElement =
  | {kind: 'node'; id: string; node: IrNode}
  | {
      kind: 'component';
      /** The variable of the node its body lowered to. */
      id: string;
      name: string;
      /** Namespaces the instance's own state, refs and animated values (`c<n>_`). */
      prefix: string;
      root: IrElement;
    };

/**
 * One entry of a container's children. Everything is built at boot, so the node count never changes: a condition
 * or a `.map` that folds at build time is plain elements, and a state-driven one is built in full and toggled.
 */
export type IrChild =
  | {kind: 'element'; element: IrElement}
  | IrConditional
  /** `items.map(…)` over list state: a fixed pool of rows, row i shown while the list holds more than i items. */
  | {kind: 'pool'; list: ListState; rows: IrElement[]};

/**
 * `cond && <X/>` or `cond ? <A/> : <B/>` on state: each branch is built and shown only while it is chosen. A
 * branch is an element, another conditional (`a ? <A/> : b ? <B/> : <C/>`), or nothing.
 */
export interface IrConditional {
  kind: 'conditional';
  test: IrExpr;
  whenTrue: IrBranch;
  whenFalse: IrBranch;
}

/** One branch of a conditional. */
export type IrBranch = IrElement | IrConditional | null;

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Whether state drives any of a node's props, text or image, so it keeps a handle and app_update() sets its
 * props again.
 *
 * @param node  The node.
 *
 * @returns True when state drives it.
 */
export const isStateDriven = (node: IrNode): boolean =>
  (node.text?.parts.some(part => part.kind !== 'literal') ?? false) ||
  node.stateProps.length > 0 ||
  node.image?.kind === 'value';

/**
 * The drawing state drives on a node, if it has one.
 *
 * @param node  The node.
 *
 * @returns The drawing, or null.
 */
export function stateDrivenVectorOf(node: IrNode): IrStateDrivenVector | null {
  for (const wire of node.wiring) {
    if (wire.kind === 'stateDrivenVector') {
      return wire.vector;
    }
  }

  return null;
}

/**
 * Every node under an element, each before its children, in the order er_app_build() creates them.
 *
 * @param element  The element.
 *
 * @returns The nodes.
 */
export function nodesOf(element: IrElement): IrNode[] {
  if (element.kind === 'component') {
    return nodesOf(element.root);
  }

  const nodes = [element.node];
  for (const child of element.node.children) {
    if (child.kind === 'element') {
      nodes.push(...nodesOf(child.element));
    } else if (child.kind === 'conditional') {
      nodes.push(...branchElements(child).flatMap(nodesOf));
    } else {
      nodes.push(...child.rows.flatMap(nodesOf));
    }
  }

  return nodes;
}

/**
 * The elements a conditional builds, in source order: every branch, nested conditionals included.
 *
 * @param conditional  The conditional.
 *
 * @returns The elements.
 */
export function branchElements(conditional: IrConditional): IrElement[] {
  return [conditional.whenTrue, conditional.whenFalse].flatMap(branch =>
    branch == null
      ? []
      : branch.kind === 'conditional'
        ? branchElements(branch)
        : [branch],
  );
}
