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

import type {BootValue} from './boot.mts';
import type {SourcePosition} from './diagnostics.mts';
import type {IrBootSlot} from './ir/boot.mts';
import type * as t from '@babel/types';
import type {Out} from './out.mts';
import type {IrPropValue, IrShow} from './ir/nodes.mts';
import type {IrExpr} from './ir/expressions.mts';
import type {IrPanResponder} from './ir/pan-responder.mts';
import type {IrStaticDecl} from './ir/functions.mts';
import type {IrSlotStore, IrSource} from './ir/source.mts';
import type {IrStmt} from './ir/statements.mts';
import type {SlotWidths, WideSlot} from './ir/types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** A function the compiler can inline or compile: a declaration, a function expression, or an arrow. */
export type FunctionNode =
  t.FunctionDeclaration | t.FunctionExpression | t.ArrowFunctionExpression;

/** Compile-time bindings a constant fold can read: module consts, folded component consts, static props. */
export type Scope = Record<string, unknown>;

/** The kind of slot that holds a value: a 32-bit int (booleans included), a float, a 64-bit int, or a string. */
export type SlotKind = 'int' | 'float' | 'i64' | 'string';

/** What a slot's kind records about the value in it. */
export interface SlotValue {
  slotKind: SlotKind;
  /** The value is a JS boolean, which renders as nothing on its own and as "true"/"false" when concatenated. */
  isBoolean?: boolean;
  /** A 64-bit constant, which converts like any number, as opposed to a runtime timestamp. */
  isLiteral?: boolean;
  /** Whole-number math worked out in 64 bits beside a timestamp, as opposed to the timestamp itself. */
  wide?: boolean;
}

/** One column of a list item: a string, a number or a boolean, at a path into the item. */
export interface ItemField {
  /** The path joined with dots: `pos.x`. A list of plain values has one field, keyed ''. */
  key: string;
  path: string[];
  kind: 'string' | 'int' | 'float' | 'bool';
  /** Some item leaves it out, so its row holds a flag for whether it is there. */
  optional?: boolean;
}

/** The fields of each item in a list, the union of its initial items' fields. */
export interface ItemStruct {
  fields: ItemField[];
  /** The items are plain values, not objects: one field, keyed ''. */
  plain?: boolean;
}

/** A name bound at runtime: a handler local, a dynamic prop, or a list row's item (which has a `struct`). */
export interface Local extends SlotValue {
  /** What a local with no `expression` is known by in the function that holds it. */
  name?: string;
  struct?: ItemStruct;
  /** A list row's item: the list it is a row of. */
  list?: ListState;
  /** A list row's item: the row it is. */
  row?: number;
  /** A row of a filtered or sorted list: its row and index count in this list, which holds the row of `list`. */
  through?: ListState;
  /** The row a filter or sort visits, which reads no state. */
  visit?: boolean;
  /** `list[i]`: the row, worked out at run time. */
  index?: IrExpr;
  /** `list[i]` reads as undefined past the list's end instead of faulting, as `list[i]?.field` does. */
  guarded?: boolean;
  /** What it was set from, which the typing pass reads when it infers slot widths. */
  source?: IrSource;
  /** The typed expression it stands for, when every read of it is that expression (a memo, a prop). */
  expression?: IrExpr;
  /** A `let`, which assignments may set: the key its width is inferred under (see compileWidened). */
  assignable?: string;
  /** A `for` loop's counter, which only the loop sets. */
  counter?: boolean;
  /** The lowest and highest value a `for` loop's counter takes, which bounds a loop inside it. */
  range?: [number, number];
}

/** What a useState / useHostValue slot and a list state have in common. */
interface StateSlot {
  /** The JS name, which reads resolve. */
  name: string;
  /** The storage name: the JS name, prefixed for an inlined component instance. */
  slotName: string;
  /** The setter's JS name; none for a host value. */
  setter: string | undefined;
  /** A node or drawing reads it, so a write flags it for the update to set them again. */
  isWatched?: boolean;
  /** Where its initial value is, for errors about it. */
  initialAt?: SourcePosition;
}

/** A scalar state: one field of the ErAppState struct. */
export interface ScalarState extends StateSlot {
  kind: 'scalar';
  slotKind: SlotKind;
  isBoolean: boolean;
  /** Its initial value: a string for a string slot, a number otherwise. */
  initial: number | string;
  /** Fed by the host through an input (useHostValue). */
  host: boolean;
  /** What it holds once widened to 64 bits (see compileWidened). */
  widened?: WideSlot;
  /** An initial value derived from `screen`, until settleBootInitials settles it. */
  bootValue?: BootValue;
  /** The boot value it starts at; `initial` is then what that is on the build's panel. */
  bootInitial?: IrBootSlot;
}

/** A list state: a fixed-capacity struct array and a count. */
export interface ListState extends StateSlot {
  kind: 'list';
  struct: ItemStruct;
  items: unknown[];
  capacity: number;
  /** A constant array read by a runtime index: its rows are set at boot and never change. */
  constant?: boolean;
  /**
   * A list worked out in render, which the update rebuilds: one filtered or sorted holds row numbers of its source,
   * and one split from a string holds the pieces.
   */
  derived?: DerivedList;
  /** A list a handler, effect or timer splits a string into, filled where its `const` is. */
  handlerSplit?: boolean;
}

/** One `filter` or `sort` of a list, and the environment its function is read in. */
export interface OrderStep {
  kind: 'filter' | 'sort';
  /** The function it is given, if any. */
  callback: t.Node | undefined;
  /** The call, for errors: `items.filter`. */
  what: string;
  environment?: Env;
}

/** How a list worked out in render is rebuilt. */
export interface DerivedList {
  /** For a filtered or sorted list, the list whose rows it holds: a list state, a constant array, or a split. */
  source?: ListState;
  /** The filters and sorts, in the order they run, which a list worked out from this one goes on from. */
  steps: OrderStep[];
  /** Rebuilds it: the rows the filters keep, in the order the sorts leave them, or a string's pieces. */
  rebuild: IrStmt[];
  /** What the rebuild works out (tests and compare functions, or the split's string), for what it reads. */
  tests: IrExpr[];
}

export type StateRecord = ScalarState | ListState;

/** An object state: a fixed-layout record with one scalar state per field, keyed by its path joined with dots. */
export interface RecordState {
  kind: 'record';
  name: string;
  setter: string | undefined;
  fields: Map<string, ScalarState>;
  /** What reads of it fold to: its fields' reads, nested as the object is. */
  value: Record<string, unknown>;
  /** An object that takes one of several shapes: the field naming the shape, and the shapes. */
  tag?: RecordTag;
}

/** A tagged record's shapes, told apart by a hidden slot holding the shape's index. */
export interface RecordTag {
  /** The field whose constant names the shape: `kind`. */
  key: string;
  /** The hidden slot. */
  state: ScalarState;
  /** Each shape's value of the tag field. */
  values: (string | number | boolean)[];
  /** Each shape's fields by their paths joined with dots, the tag left out. */
  shapes: Set<string>[];
}

/** A component's state, by JS name (reads) and by setter name (writes). A record's fields are keyed `name.path`. */
export interface StateTable {
  byName: Map<string, StateRecord>;
  bySetter: Map<string, StateRecord | RecordState>;
}

/** A `useRef(number)`: a mutable slot that does not re-render. */
export interface ValueRef {
  kind: 'value';
  slotName: string;
  slotKind: 'int' | 'float' | 'i64';
  initial: number;
  /** What it holds once widened to 64 bits (see compileWidened). */
  widened?: WideSlot;
  /** Whether the app reads or writes it; an unused ref declares nothing. */
  used: boolean;
}

/** A `useRef()` / `useRef(null)`: holds an node handle, captured by `ref={r}` on an element. */
export interface NodeRef {
  kind: 'node';
  slotName: string;
  slotKind: 'node';
  used: boolean;
}

export type RefRecord = ValueRef | NodeRef;

/** A useAnimatedValue: an engine-side ERAnimationValueHandle. */
export interface AnimRecord {
  slotName: string;
  initial: number;
}

/** The fixed-buffer caps, resolved per compile (see resolveOptions). */
export interface Caps {
  listCap: number;
  listStrCap: number;
  maxTextSpans: number;
  maxTimers: number;
}

/**
 * How the element being emitted is placed. `show` is set on a list row, and under a conditional in one: the
 * element exists either way, and the update shows it only while the condition holds.
 */
export interface EmitOptions {
  show?: IrShow;
}

/** The name of a constant in er_scene.h. */
export type EngineName = `ER_${string}`;

/** A constant prop value: a number, or a constant er_scene.h names. */
export type PropConstant = number | EngineName;

/** An ERProps field set from a compile-time constant. */
export interface StaticAssign {
  field: string;
  value: PropConstant;
}

/** An ERProps field set from a runtime value, re-applied in the update. */
export interface DynAssign {
  field: string;
  value: IrPropValue;
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
  functionNode: FunctionNode;
  dependencies: t.CallExpression['arguments'][number];
  node: t.CallExpression;
}

/** A PanResponder callback: an arrow or function expression, or a method of the config object. */
export type PanCallback = FunctionNode | t.ObjectMethod;

/** A `PanResponder.create({...})`, lowered onto the engine's responder queries and events. */
export interface PanResponderRecord {
  /** The JS name it is bound to. */
  name: string;
  /** Prefix of its gesture state (`<statePrefix>_granted`, …). */
  statePrefix: string;
  /** Prefix of its functions. */
  functionPrefix: string;
  /** The gesture's id, assigned when the responder is first wired to a node. */
  stateID: number;
  /** Should-set predicates, by engine query. */
  queries: Map<string, PanCallback>;
  /** Callbacks, by engine event. */
  events: Map<string, PanCallback>;
  /** The responder once lowered, so a second node that spreads it reuses it. */
  lowered: IrPanResponder | null;
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
  nodes: t.JSXElement['children'];
  scope: Scope;
  environment: Env;
  ref: ChildrenRef | null;
}

/** A callback prop: the caller's function, compiled in the caller's env and state. */
export interface FunctionProp {
  node: FunctionNode;
  environment: Env;
  state: StateTable;
}

/**
 * The expression environment: everything a JS expression can refer to while it is lowered. The App's
 * env is built once per compile; an inlined component, a list row, a handler, and an effect each extend it
 * with their own bindings.
 */
export interface Env {
  /** State in reach, by JS name. */
  state: Map<string, StateRecord>;
  /** Runtime bindings: handler locals, dynamic props, list rows. */
  locals: Map<string, Local>;
  /** Compile-time constants. */
  constants: Scope;
  /** The module's own constants, which an inlined component closes over instead of its caller's. */
  moduleConstants: Scope;
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
  /** State and ref slots declared 64-bit this pass, and what each holds (see compileWidened). */
  wide: SlotWidths;
  /** Collects every value stored into an int slot, from which the typing pass infers the slots to widen. */
  stores: IrSlotStore[];
  /** `Date` / `performance` calls that name the app's own binding rather than the engine clock. */
  shadowedClock: WeakSet<t.Node>;
  /** Children passed to the component being inlined. */
  children?: ChildrenSlot | null;
  /** Inside a list row, whose conditionals build every branch and toggle it. */
  inListRow?: boolean;
  /** Lowering a handler, an effect or a timer, which runs apart from the render. */
  outsideRender?: boolean;
  /** Callback props of the component being inlined. */
  functionProps?: Map<string, FunctionProp>;
  /** Namespaces the handlers an inlined component instance emits. */
  callbackPrefix?: string;
  /** The handler's event parameter, whose touch fields read EREventData. */
  event?: string | null;
  /** The handler is an onLayout handler, so its event carries the layout rect. */
  layout?: boolean;
  /** A PanResponder callback's gestureState parameter, and the responder it belongs to. */
  gesture?: string | null;
  pan?: PanResponderRecord | null;
  /** Whole-number math is being worked out in 64 bits, beside a timestamp. */
  math64?: boolean;
  /** Handler locals bound to an Animated composition, which `.start()` / `.stop()` compile. */
  animationLocals?: Map<string, t.Expression>;
  /** Constant arrays read by a runtime index, each kept as a list whose rows never change. */
  constantLists?: Map<unknown[], ListState>;
  /** Lists filtered or sorted in render, by the call that makes them, then the component instance and the list it starts from. */
  derivedLists?: Map<t.Node, Map<string, ListState>>;
  /** Helpers being inlined, so a recursive one is refused. */
  inlining?: ReadonlySet<string>;
  /** Lists a handler, effect, or timer splits a string into, by the `const` that names them, then the component instance. */
  splitLists?: Map<t.Node, Map<string, ListState>>;
}

/**
 * What the statement compiler carries through one body — a handler, an effect, a timer, or animation callback —
 * and reports back to the caller that wraps it in a function.
 */
export interface StatementContext {
  /** A statement set state, so the body ends with the update. */
  stateChanged: boolean;
  output: Out;
  /** The body's own top-level statements, which a tail `return` and hoisted locals are measured against. */
  bodyList?: t.Statement[];
  /** The statements of the helper being inlined, whose tail `return` needs no jump. */
  helperList?: t.Statement[];
  /** An early `return` was lowered, so the body or helper is a block it leaves, or an effect's function of its own. */
  usedReturn?: boolean;
  /** The body is a dep-driven effect. */
  dependencyDriven?: boolean;
  /** The loops around the statements being lowered, innermost last: the lists each visits or is bounded by. */
  loops?: ListState[][];
  /** The body's locals become slots named `<prefix><name>`, declared into `declarations`: its cleanup
   *  closure outlives the call. */
  hoist?: {prefix: string; declarations: IrStaticDecl[]};
  /** Compiles a dep-driven effect's `return () => …` into its companion cleanup function; `armed` is the flag
   *  that says the last run reached it. */
  cleanup?: {
    armed: string;
    emit: (functionNode: FunctionNode, environment: Env) => void;
  };
}
