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

import type * as t from '@babel/types';
import type {Out} from './out.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** A function the compiler can inline or compile: a declaration, a function expression, or an arrow. */
export type FunctionNode =
  | t.FunctionDeclaration
  | t.FunctionExpression
  | t.ArrowFunctionExpression;

/** Compile-time bindings a constant fold can read: module consts, folded component consts, static props. */
export type Scope = Record<string, unknown>;

/** The C kind of a value: a 32-bit int (booleans included), a float, a 64-bit int, or a char buffer. */
export type CType = 'int' | 'float' | 'i64' | 'string';

/** A JS expression lowered to C. */
export interface CExpr {
  code: string;
  cType: CType;
  /** The value is a JS boolean, which renders as nothing on its own and as "true"/"false" when concatenated. */
  isBool?: boolean;
  /** A 64-bit constant, which converts like any number, as opposed to a runtime timestamp. */
  lit?: boolean;
  /** Whole-number math worked out in 64 bits beside a timestamp, as opposed to the timestamp itself. */
  wide?: boolean;
}

/** One column of a list-state item: a string or a number. */
export interface ItemField {
  key: string;
  kind: 'string' | 'int' | 'float';
}

/** The C struct used to store each item in a list state, inferred from the list's first initial item. */
export interface ItemStruct {
  fields: ItemField[];
}

/** A name bound at runtime: a handler local, a dynamic prop, or a list row's item (which has a `struct`). */
export interface Local extends CExpr {
  struct?: ItemStruct;
}

/** What a useState / useHostValue slot and a list state have in common. */
interface StateSlot {
  /** The JS name, which reads resolve. */
  name: string;
  /** The C storage name: the JS name, prefixed for an inlined component instance. */
  cField: string;
  /** The setter's JS name; none for a host value. */
  setter: string | undefined;
}

/** A scalar state: one field of the ErAppState struct. */
export interface ScalarState extends StateSlot {
  kind: 'scalar';
  cType: CType;
  isBool: boolean;
  /** How C reads it: `s_state.<cField>`. */
  cMember: string;
  initCode: string;
  /** Fed by the host through a generated er_app_set_<name>() (useHostValue). */
  host: boolean;
}

/** A list state: a fixed-capacity struct array and a count. */
export interface ListState extends StateSlot {
  kind: 'list';
  struct: ItemStruct;
  items: Record<string, unknown>[];
  cap: number;
  cTypeName: string;
  arrayName: string;
  countMember: string;
}

export type StateRecord = ScalarState | ListState;

/** A component's state, by JS name (reads) and by setter name (writes). */
export interface StateTable {
  byName: Map<string, StateRecord>;
  bySetter: Map<string, StateRecord>;
}

/** A `useRef(number)`: a mutable C slot that does not re-render. */
export interface ValueRef {
  kind: 'value';
  cVar: string;
  cType: 'int' | 'float' | 'i64';
  initCode: string;
  /** Whether the generated C reads or writes it; an unused ref declares nothing. */
  used: boolean;
}

/** A `useRef()` / `useRef(null)`: holds an `ERNode*`, captured by `ref={r}` on an element. */
export interface NodeRef {
  kind: 'node';
  cVar: string;
  cType: 'ERNode*';
  initCode: 'NULL';
  used: boolean;
}

export type RefRecord = ValueRef | NodeRef;

/** A useAnimatedValue: an engine-side ERAnimValueHandle. */
export interface AnimRecord {
  cVar: string;
  initCode: string;
}

/** The fixed-buffer caps, resolved per compile (see resolveOptions). */
export interface Caps {
  listCap: number;
  listStrCap: number;
  maxTextSpans: number;
}

/**
 * How the element being emitted is placed. `displayCode` is a C condition: the element exists either way,
 * and app_update() shows it only while the condition holds (a state-driven `cond && <El/>`).
 */
export interface EmitOptions {
  displayCode?: string;
}

/** An ERProps field set from a compile-time constant. */
export interface StaticAssign {
  field: string;
  expr: string;
}

/** An ERProps field set from a runtime expression, re-applied in app_update(). */
export interface DynAssign {
  field: string;
  code: string;
}

/** An expression lowered to one printf format and its arguments; `format` has `%` escaped as `%%`. */
export interface FormatResult {
  format: string;
  args: string[];
}

/** A gradient as svg-ops flattens it, its geometry in the op-tape's coordinates. */
export interface VectorGradient {
  type: number;
  stops: {offset: number; color: number}[];
  ax?: number;
  ay?: number;
  bx?: number;
  by?: number;
  r?: number;
}

/** An <Svg source> file baked to a vector op-tape (svgToVector), sized at its viewBox. */
export interface VectorArtifact {
  kind?: undefined;
  ops: number[];
  paints: number[];
  gradients?: VectorGradient[];
  width: number;
  height: number;
  /** SVG features the baker could not represent; when any are listed, the file is baked as a PNG instead. */
  dropped?: string[];
}

/** An <Svg source> file that used features the vector baker cannot represent, baked as a PNG. */
export interface RasterArtifact {
  kind: 'raster';
  name: string;
  width: number;
  height: number;
  /** The PNG's path, for the image baker. */
  png: string;
}

/** An <Svg source> file baked ahead of the compile (bakeSvgArtifacts). */
export type SvgArtifact = VectorArtifact | RasterArtifact;

/** An asset import: `import wxSun from './assets/wx_sun.png'` → name `wx_sun`, importPath as written. */
export interface AssetImport {
  name: string;
  importPath: string;
}

/** A `useEffect(fn, deps)` call. The deps are checked when the effect is compiled. */
export interface EffectRecord {
  fn: FunctionNode;
  deps: t.CallExpression['arguments'][number] | undefined;
  node: t.CallExpression;
}

/** A PanResponder callback: an arrow or function expression, or a method of the config object. */
export type PanCallback = FunctionNode | t.ObjectMethod;

/** A `PanResponder.create({...})`, lowered onto the engine's responder queries and events. */
export interface PanResponderRecord {
  /** The JS name it is bound to. */
  name: string;
  /** Prefix of its file-scope gesture state (`<cPrefix>_granted`, …). */
  cPrefix: string;
  /** Prefix of its generated C functions. */
  fnPrefix: string;
  /** The gesture's id, assigned when the responder is first wired to a node. */
  stateID: number;
  /** Should-set predicates, by engine query. */
  queries: Map<string, PanCallback>;
  /** Callbacks, by engine event. */
  events: Map<string, PanCallback>;
  /** The C functions already generated for it, so a second node reuses them: [query or event, function]. */
  emitted: {events: [string, string][]; queries: [string, string][]} | null;
  /** It was created in a useRef, so the JSX spreads `<name>.current.panHandlers`. */
  viaRefCurrent?: boolean;
}

/** How a component's body refers to the children it was given: a destructured local, or `props.children`. */
export interface ChildrenRef {
  kind: 'local' | 'props';
  name: string;
}

/** The children passed to an inlined component, emitted in the caller's scope wherever the body uses them. */
export interface ChildrenSlot {
  nodes: (t.JSXElement | t.JSXExpressionContainer)[];
  scope: Scope;
  env: Env;
  ref: ChildrenRef | null;
}

/** A callback prop: the caller's function, compiled in the caller's env and state. */
export interface FunctionProp {
  node: FunctionNode;
  env: Env;
  state: StateTable;
}

/**
 * The expression environment: everything a JS expression can refer to while it is lowered to C. The App's
 * env is built once per compile; an inlined component, a list row, a handler, and an effect each extend it
 * with their own bindings.
 */
export interface Env {
  /** State in reach, by JS name. */
  state: Map<string, StateRecord>;
  /** Runtime bindings: handler locals, dynamic props, list rows. */
  locals: Map<string, Local>;
  /** Compile-time constants. */
  consts: Scope;
  /** The module's own constants, which an inlined component closes over instead of its caller's. */
  moduleConsts: Scope;
  animations: Map<string, AnimRecord>;
  refs: Map<string, RefRecord>;
  pans: Map<string, PanResponderRecord>;
  /** useCallback handlers, by name. */
  callbacks: Map<string, FunctionNode>;
  /** Plain helper functions a handler can inline, by name. */
  helpers: Map<string, FunctionNode>;
  /** Imported images: asset name → source-relative path. */
  imageNames: Map<string, string>;
  /** <Svg source> imports, by local name. */
  svgImports: Map<string, AssetImport>;
  svgArtifacts: Record<string, SvgArtifact>;
  caps: Caps;
  /** State and ref slots declared 64-bit this pass (see compileWidened). */
  wide: Set<string>;
  /** Collects the int slots a 64-bit timestamp was stored in, for the next pass to widen. */
  found: Set<string>;
  /** `Date` / `performance` calls that name the app's own binding rather than the engine clock. */
  shadowedClock: WeakSet<t.Node>;
  /** Children passed to the component being inlined. */
  children?: ChildrenSlot | null;
  /** Callback props of the component being inlined. */
  fnProps?: Map<string, FunctionProp>;
  /** Namespaces the C handlers an inlined component instance emits. */
  cbPrefix?: string;
  /** The handler's event parameter, whose touch fields read EREventData. */
  event?: string | null;
  /** A PanResponder callback's gestureState parameter, and the responder it belongs to. */
  gesture?: string | null;
  pan?: PanResponderRecord | null;
  /** Whole-number math is being worked out in 64 bits, beside a timestamp. */
  math64?: boolean;
  /** Handler locals bound to an Animated composition, which `.start()` / `.stop()` compile. */
  animLocals?: Map<string, t.Expression>;
}

/**
 * What the statement compiler carries through one body — a handler, an effect, a timer, or animation callback —
 * and reports back to the caller that wraps it in a C function.
 */
export interface StatementContext {
  /** A statement set state, so the body ends with app_update(). */
  stateChanged: boolean;
  /** Numbers the ERAnimConfig locals an Animated.start() declares, unique within the body. */
  animIdx: number;
  out: Out;
  /** A `return` may exit this body: it is an effect body. */
  allowReturn?: boolean;
  /** The body's own top-level statements, which a tail `return` and hoisted locals are measured against. */
  bodyList?: t.Statement[];
  /** An early `return` was lowered, so the body needs a C function of its own. */
  usedReturn?: boolean;
  /** The body is a dep-driven effect. */
  depDriven?: boolean;
  /** The body's locals become file-scope slots named `<prefix><name>`, declared into `decls`: its cleanup
   *  closure outlives the call. */
  hoist?: {prefix: string; decls: string[]};
  /** Compiles a dep-driven effect's `return () => …` into its companion cleanup function; `armed` is the flag
   *  that says the last run reached it. */
  cleanup?: {armed: string; emit: (fn: FunctionNode, env: Env) => void};
  /** Helpers being inlined, so a recursive one is refused. */
  inlining?: Set<string>;
}
