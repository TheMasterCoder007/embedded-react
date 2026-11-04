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

/*
 * The package backend: lowers the IR to a Flow B program for one panel.
 *
 * The tree becomes the root template and its constant prop sets. State, refs and values worked out at boot are
 * slots in the arena. Handlers, timer callbacks, effects, the boot function and the update function are
 * bytecode (codegen.mts). The update re-sets a node when a state it reads changed, sets
 * the props of a node that reads only boot values once, in its first pass, and queues the effects.
 *
 * An <Svg> drawing is a tape (vectors.mts): a baked one is committed when its node is built, and the update
 * rebuilds one state drives, after the nodes. setKeyboardConfig is the keyboard table, which
 * the runtime hands the engine at load.
 *
 * Animations (animations.mts): each animation is a leaf descriptor, each on_complete callback an
 * animation-done function, each completion a helper.
 *
 * A list is a pool of rows in the arena. Its `.map` is a row template, built once per row in the update's first
 * pass; a row shows while the list holds it. Rows whose nodes and handlers
 * are the same share a template, and a row handler reads its item through the row it was built for.
 *
 * A PanResponder is a handler for each responder event and a query function for each should-set predicate,
 * bound on each node that spreads it. Its gesture state is slots, kept by its grant, release and terminate
 * handlers, and a callback reads the gesture through them and EVT.
 *
 * An effect runs inside an update pass, so the state it sets shows at the next pass that isn't its own, not at
 * once. The VM runs a pass whenever state changed, so a pass that only an
 * effect's write started keeps what changed in a pending flag, and the next pass a handler, a timer or the
 * load started sets it. A dep-driven effect checks its deps itself, when it runs,
 * each in turn. The mount effects run one after another, after the first pass; one that set state has the
 * pass after it queue the next.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {
  aotError,
  constantText,
  ENGINE_STRING_MAX,
  locateError,
} from '../diagnostics.mts';
import {
  hasRuntimeProps,
  isStateDriven,
  nodesOf,
  branchElements,
  stateDrivenVectorOf,
  updatedVectorOf,
} from '../ir/nodes.mts';
import {nodeReads, vectorReads} from '../ir/reads.mts';
import {armedFlag} from '../ir/functions.mts';
import {AnimationPlan, doneFunctions, startPressFade} from './animations.mts';
import {Arena} from './arena.mts';
import {FunctionBuilder, OP} from './bytecode.mts';
import {internedStates} from './enums.mts';
import {Codegen, notYet} from './codegen.mts';
import {
  BindingKind,
  ENGINE_VALUES,
  EventField,
  FLAG_DEBUG,
  FRAME_CELLS,
  FunctionKind,
  MAX_SPANS,
  NONE,
  Resource,
  SlotFlag,
  TemplateKind,
  ValueType,
} from './format.mts';
import {
  abiProp,
  constantPropEntry,
  floatBits,
  stringPropEntry,
} from './props.mts';
import {ProgramTables} from './tables.mts';
import {commitDrawing, VectorPlan} from './vectors.mts';
import type {Slot} from './arena.mts';
import type {Label, Register} from './bytecode.mts';
import type {ExprType, InputText, ProgramSlots} from './codegen.mts';
import type {SourcePosition} from '../diagnostics.mts';
import type {IrApp} from '../ir/app.mts';
import type {IrKeyboard, IrKeyKind} from '../ir/keyboard.mts';
import {lowerCheck} from './checks.mts';
import type {IrBootSlot} from '../ir/boot.mts';
import type {IrExpr} from '../ir/expressions.mts';
import type {IrCompletion} from '../ir/animations.mts';
import type {
  IrDepEffect,
  IrMountEffect,
  IrPressFade,
} from '../ir/functions.mts';
import type {IrPanResponder, IrPanRole} from '../ir/pan-responder.mts';
import type {IrCallback, IrStmt, IrUpdateVector} from '../ir/statements.mts';
import type {IrElement, IrNode} from '../ir/nodes.mts';
import type {IrStateDrivenVector} from '../ir/vectors.mts';
import type {StateReads} from '../ir/reads.mts';
import type {ResolvedOptions} from '../options.mts';
import type {TextSpan} from '../style-text.mts';
import type {ListState, ScalarState, StateRecord} from '../types.mts';
import type {
  Program,
  ProgramBinding,
  ProgramFunction,
  ProgramKeyboard,
  ProgramPropEntry,
  ProgramRow,
  ProgramTemplate,
  ProgramTemplateNode,
} from './writer.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** A handler as a function: the callback, and the event it is entered with. */
interface HandlerFunction {
  index: number;
  callback: IrCallback;
  /** A <TouchableOpacity>'s fade, started before the callback. */
  fade: IrPressFade | null;
  event: number;
  name: string;
  /** For a <TextInput>'s onChangeText, the input it reads. */
  input: InputText | undefined;
  /** Bound in a list row: entered with the row it was built for. */
  inRow: boolean;
  /** A PanResponder event's: the responder's gesture state, and what the handler keeps of it. */
  gesture?: {statePrefix: string; role: IrPanRole};
  /** A PanResponder query's predicate, which the function returns. */
  test?: IrExpr;
}

/** A node in a list row that code reaches: its handle array, and its row. */
interface RowHandle {
  array: Slot;
  row: number;
}

/** A `.map` over a list: where its rows are built, and each row's template. */
interface Pool {
  list: ListState;
  rows: IrElement[];
  parent: IrNode;
  /** The node the rows go before, or null for the end. */
  anchor: IrNode | null;
  templates: number[];
}

/** A dep-driven effect's copy of one dep, and the type it is kept in. */
interface DepCopy {
  slot: Slot;
  type: ExprType;
}

/** What the effects keep between runs, and the passes that run them. */
interface EffectSlots {
  /** Each dep-driven effect's copy of each dep. */
  copies: Map<IrDepEffect, DepCopy[]>;
  /** Each state an effect sets, by its slot: set while a change no update has set yet waits. */
  pending: Map<Slot, Slot>;
  /** The bits that start an update: a handler's or timer's request, the load, and the host. */
  realBits: Slot[];
  /** The next mount effect the update queues, plus one; 0 for none. */
  mountNext: Slot | null;
}

/** Everything lowering one program shares. */
interface Lowering {
  app: IrApp;
  slots: ProgramSlots;
  images: ReadonlySet<string>;
  /** Each node handle code reaches, by node, outside the list rows. */
  handles: Map<IrNode, Slot>;
  rowHandles: Map<IrNode, RowHandle>;
  /** Each list's row layout. */
  rows: ProgramRow[];
  pools: Pool[];
  /** The row templates, after the root. */
  templates: ProgramTemplate[];
  /** The buffer a <TextInput>'s onChangeText reads its text into. */
  inputText: Slot | null;
  /** Handlers by name and event, in function order. */
  handlers: Map<string, HandlerFunction>;
  bindings: ProgramBinding[];
  /** Toggled by a handler that set state, so the update runs. */
  requested: Slot | null;
  /** Set only at load: it guards the props the update sets once, and runs the update's first pass. */
  once: Slot | null;
  effects: EffectSlots;
  /** The function each dep-driven effect checks its deps in, and each mount effect's. */
  effectFunctions: {checks: number[]; mounts: number[]};
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** The most a u16 count or size in the format holds: a row's bytes, a panel's nodes, the row fields. */
const MAX_U16 = 0xffff;

/** The most fields a row has: ROWGET names one in a byte. */
const MAX_ROW_FIELDS = 255;

/** Key kind → its ERKeyboardKey type. */
const KEY_TYPES: Record<IrKeyKind, string> = {
  char: 'ER_KEYBOARD_KEY_CHAR',
  layer: 'ER_KEYBOARD_KEY_LAYER',
  backspace: 'ER_KEYBOARD_KEY_BACKSPACE',
  done: 'ER_KEYBOARD_KEY_DONE',
};

/** The highlight layer that means "never highlighted". */
const NEVER_HIGHLIGHTED = 255;

/** A TextSpan's constant fields, in their ABI ids' order. */
const SPAN_FIELDS = [
  'color',
  'font_size',
  'font_weight',
  'font_style',
  'text_decoration',
  'letter_spacing',
] as const;

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Refuses what a package can't lower yet, pointing at where it is.
 *
 * @param what  What the app uses.
 * @param location  Where in the source, when it is known.
 */
function notYetAt(what: string, location?: SourcePosition): never {
  try {
    notYet(what);
  } catch (error) {
    locateError(error, location);
    throw error;
  }
}

/** Whether the update sets a node's props once, at load: they read only boot values, or it has spans. */
const isSetOnce = (node: IrNode): boolean =>
  !isStateDriven(node) && (hasRuntimeProps(node) || node.spans !== null);

/*----------------------------------------------------------------------------------------------------------------------
 - Slots
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * The most bytes a boot value's string can be: its longest literal, since nothing else reaches it at boot.
 *
 * @param expression  The value.
 *
 * @returns The bytes.
 */
function longestLiteral(expression: IrExpr): number {
  let longest = 0;
  const walk = (value: unknown): void => {
    if (!value || typeof value !== 'object') {
      return;
    }
    const node = value as {kind?: string; value?: unknown};
    if (node.kind === 'string' && typeof node.value === 'string') {
      longest = Math.max(longest, Buffer.byteLength(node.value, 'utf8'));
      return;
    }
    Object.values(node).forEach(walk);
  };
  walk(expression);
  return Math.max(longest, 1);
}

/**
 * Every string local a statement list declares, with its cap.
 *
 * @param statements  The statements.
 * @param found  Collects each local's name and cap.
 */
function stringLocals(statements: IrStmt[], found: Map<string, number>): void {
  for (const statement of statements) {
    if (
      statement.kind === 'declareLocal' &&
      statement.initializer.type.kind === 'string'
    ) {
      found.set(
        statement.name,
        Math.max(found.get(statement.name) ?? 0, statement.stringCapacity - 1),
      );
    } else if (statement.kind === 'if') {
      stringLocals(statement.consequent, found);
      stringLocals(statement.alternate ?? [], found);
    }
  }
}

/**
 * Every updateVector call a statement list makes.
 *
 * @param statements  The statements.
 * @param found  Collects the calls.
 */
function updateVectors(statements: IrStmt[], found: IrUpdateVector[]): void {
  for (const statement of statements) {
    if (statement.kind === 'updateVector') {
      found.push(statement);
    } else if (statement.kind === 'if') {
      updateVectors(statement.consequent, found);
      updateVectors(statement.alternate ?? [], found);
    }
  }
}

/**
 * Every state and list a statement list sets.
 *
 * @param statements  The statements.
 * @param found  Collects the states.
 */
function statesSet(statements: IrStmt[], found: Set<StateRecord>): void {
  for (const statement of statements) {
    if (statement.kind === 'setState' || statement.kind === 'setStateText') {
      found.add(statement.state);
    } else if ('list' in statement) {
      found.add(statement.list);
    } else if (statement.kind === 'if') {
      statesSet(statement.consequent, found);
      statesSet(statement.alternate ?? [], found);
    }
  }
}

/**
 * The type a dep's copy is kept in.
 *
 * @param dependency  The dep.
 *
 * @returns Its type.
 */
function depType(dependency: IrExpr): ExprType {
  switch (dependency.type.kind) {
    case 'bool':
    case 'float':
    case 'int64':
    case 'string':
      return dependency.type.kind;
    default:
      return 'int';
  }
}

/**
 * Whether a mount effect sets state, so an update runs after it.
 *
 * @param mountEffect  The mount effect.
 *
 * @returns Whether it does.
 */
const mountUpdates = (mountEffect: IrMountEffect): boolean =>
  mountEffect.kind === 'inline'
    ? mountEffect.callback.updatesState
    : mountEffect.kind === 'call' && mountEffect.updatesState;

/**
 * Every statement list the program runs outside the update and boot functions.
 *
 * @param app  The app.
 *
 * @returns The lists.
 */
function bodies(app: IrApp): IrStmt[][] {
  const {output} = app;
  return [
    ...output.handlers.flatMap(handler =>
      handler.kind === 'callback'
        ? [handler.callback.body]
        : handler.kind === 'panEvent' && handler.event.callback
          ? [handler.event.callback.body]
          : [],
    ),
    ...output.timerFunctions.map(timer => timer.callback!.body),
    ...output.effectFunctions.map(effectFunction =>
      effectFunction.kind === 'mount'
        ? effectFunction.body
        : effectFunction.kind === 'cleanup'
          ? effectFunction.effect.cleanup!
          : effectFunction.effect.body,
    ),
    ...output.mountEffects.flatMap(mountEffect =>
      mountEffect.kind === 'inline' ? [mountEffect.callback.body] : [],
    ),
    ...completionsOf(app).map(completion => completion.body),
  ];
}

/**
 * Every animation completion, in the order the app declares them.
 *
 * @param app  The app.
 *
 * @returns The completions.
 */
const completionsOf = (app: IrApp): IrCompletion[] =>
  app.output.animationCallbacks.flatMap(callback =>
    callback.kind === 'completion' ? [callback.completion] : [],
  );

/**
 * A state's slot: a scalar's own, or a list's pool.
 *
 * @param state  The state.
 * @param slots  The program's slots.
 *
 * @returns The slot.
 */
const stateSlotOf = (state: StateRecord, slots: ProgramSlots): Slot =>
  state.kind === 'list'
    ? slots.lists.get(state)!.slot
    : slots.states.get(state)!;

/**
 * The first node an element builds.
 *
 * @param element  The element.
 *
 * @returns The node.
 */
const firstNode = (element: IrElement): IrNode =>
  element.kind === 'component' ? firstNode(element.root) : element.node;

/**
 * Every `.map` over a list under an element, in tree order, with the node its rows go under and the one they go
 * before. A list inside a list's row is refused.
 *
 * @param element  The element.
 * @param found  Collects the pools.
 */
function poolsOf(element: IrElement, found: Pool[]): void {
  if (element.kind === 'component') {
    poolsOf(element.root, found);
    return;
  }
  const {node} = element;
  node.children.forEach((child, index) => {
    if (child.kind === 'element') {
      poolsOf(child.element, found);
    } else if (child.kind === 'conditional') {
      branchElements(child).forEach(branch => poolsOf(branch, found));
    } else {
      for (const row of child.rows) {
        const nested = nodesOf(row).find(rowNode =>
          rowNode.children.some(rowChild => rowChild.kind === 'pool'),
        );
        if (nested) {
          notYetAt('a list inside a list row', nested.location);
        }
      }
      // The rows go before the next node the parent builds itself.
      const after = node.children
        .slice(index + 1)
        .flatMap(sibling =>
          sibling.kind === 'element'
            ? [sibling.element]
            : sibling.kind === 'conditional'
              ? branchElements(sibling)
              : [],
        );
      found.push({
        list: child.list,
        rows: child.rows,
        parent: node,
        anchor: after.length ? firstNode(after[0]) : null,
        templates: [],
      });
    }
  });
}

/**
 * Plans every slot the program uses, then lays the arena out.
 *
 * @param app  The app.
 * @param tables  The program's tables.
 * @param caps  The buffer sizes, and the timers that run at once.
 *
 * @returns The slots, the node handles, and the bookkeeping bits.
 */
function planSlots(
  app: IrApp,
  tables: ProgramTables,
  caps: ResolvedOptions['caps'],
): Pick<
  Lowering,
  | 'slots'
  | 'handles'
  | 'rowHandles'
  | 'rows'
  | 'pools'
  | 'inputText'
  | 'requested'
  | 'once'
  | 'effects'
> {
  const {listStrCap: listStringCapacity} = caps;
  const {output} = app;
  const arena = new Arena();
  const animations = new AnimationPlan(app, arena);
  const slots: ProgramSlots = {
    arena,
    tables,
    states: new Map(),
    lists: new Map(),
    refs: new Map(),
    boot: new Map(),
    stringLocals: new Map(),
    enums: new Map(),
    hoisted: new Map(),
    armed: new Map(),
    timerFunctions: new Map(),
    animations,
    gestures: new Map(),
    vectors: new VectorPlan(arena),
  };

  // String state with a closed set of values is an interned enum, starting at its initial value (the first).
  for (const [state, values] of internedStates(app)) {
    slots.enums.set(state, {
      set: slots.enums.size,
      values,
    });
  }

  // State, each with a dirty bit when a node reads it.
  for (const state of app.state) {
    if (state.kind === 'list') {
      continue;
    }
    const interned = slots.enums.get(state);
    // One the boot function sets starts at 0, so nothing of the build's panel is left in the package.
    const initial = state.bootInitial ? 0 : state.initial;
    const slot = interned
      ? Object.assign(arena.slot(ValueType.enum, 0), {aux: interned.set})
      : state.slotKind === 'string'
        ? arena.slot(
            ValueType.string,
            tables.string(String(initial)),
            listStringCapacity - 1,
          )
        : state.slotKind === 'i64'
          ? arena.slot(
              ValueType.int64,
              tables.constant64(BigInt(Math.trunc(Number(initial)))),
            )
          : state.slotKind === 'float'
            ? arena.slot(ValueType.float, floatBits(Number(initial)))
            : arena.slot(
                state.isBoolean ? ValueType.bool : ValueType.int,
                Math.trunc(Number(initial)) | 0,
              );
    // A host value an effect's deps may read runs an update when it is set.
    if (state.isWatched || (state.host && output.dependencyEffects.length)) {
      arena.track(slot);
    }
    slots.states.set(state, slot);
  }

  // A list is a pool of rows, empty at load; the boot function appends its first items. Each row field is
  // 4-aligned, and a string field holds its cap and a terminator.
  const rows: ProgramRow[] = [];
  for (const state of app.state) {
    if (state.kind !== 'list') {
      continue;
    }
    if (!state.struct.fields.length) {
      notYet(`list state "${state.name}", whose items have no fields`);
    }
    if (state.struct.fields.length > MAX_ROW_FIELDS) {
      throw aotError(
        `AOT: list state "${state.name}" has ${state.struct.fields.length} fields, but a package row holds ${MAX_ROW_FIELDS}`,
        'give the items fewer fields.',
      );
    }
    let size = 0;
    const fields = state.struct.fields.map(field => {
      const capacity = field.kind === 'string' ? listStringCapacity - 1 : 0;
      const entry = {type: ValueType[field.kind], cap: capacity, offset: size};
      size += field.kind === 'string' ? (capacity + 1 + 3) & ~3 : 4;
      return entry;
    });
    if (size > MAX_U16) {
      throw aotError(
        `AOT: a row of list state "${state.name}" takes ${size} bytes, but a package row holds ${MAX_U16 - 3}`,
        'lower ER_AOT_LIST_STR_CAP (the listStrCap option), or give the items fewer text fields.',
      );
    }
    const slot = arena.list(rows.length, size, state.capacity);
    rows.push({size, fields});
    if (state.isWatched) {
      arena.track(slot);
    }
    // An appended item's text is built in a buffer of its own, then copied into the row.
    const buffers = new Map(
      state.struct.fields
        .filter(field => field.kind === 'string')
        .map(field => [
          field.key,
          arena.slot(ValueType.string, undefined, listStringCapacity - 1),
        ]),
    );
    slots.lists.set(state, {slot, buffers});
  }
  const fieldCount = rows.reduce((count, row) => count + row.fields.length, 0);
  if (fieldCount > MAX_U16) {
    throw aotError(
      `AOT: the lists have ${fieldCount} fields in all, but a package holds ${MAX_U16}`,
      'use fewer lists, or give their items fewer fields.',
    );
  }

  for (const ref of app.refs) {
    if (ref.kind !== 'value') {
      continue;
    }
    slots.refs.set(
      ref,
      ref.slotKind === 'i64'
        ? arena.slot(
            ValueType.int64,
            tables.constant64(BigInt(Math.trunc(ref.initial))),
          )
        : ref.slotKind === 'float'
          ? arena.slot(ValueType.float, floatBits(ref.initial))
          : arena.slot(ValueType.int, Math.trunc(ref.initial) | 0),
    );
  }

  // Boot values: a fraction is kept as a double.
  for (const bootSlot of app.boot.slots) {
    const slot =
      bootSlot.type.kind === 'float'
        ? arena.slot(ValueType.double)
        : bootSlot.type.kind === 'string'
          ? arena.slot(
              ValueType.string,
              undefined,
              bootSlot.initializer.kind === 'expr'
                ? longestLiteral(bootSlot.initializer.expression)
                : 1,
            )
          : arena.slot(
              bootSlot.type.kind === 'bool' ? ValueType.bool : ValueType.int,
            );
    slot.flags |= SlotFlag.bootOnly;
    slots.boot.set(bootSlot, slot);
  }

  // The effect locals a cleanup reads, and each effect's armed flag and copy of each dep.
  for (const declaration of output.effectDeclarations) {
    if (declaration.kind === 'local') {
      slots.hoisted.set(
        declaration.name,
        declaration.slotKind === 'string'
          ? arena.slot(ValueType.string, undefined, listStringCapacity - 1)
          : arena.slot(
              declaration.slotKind === 'i64'
                ? ValueType.int64
                : declaration.slotKind === 'float'
                  ? ValueType.float
                  : ValueType.int,
            ),
      );
    } else if (declaration.kind === 'armed') {
      slots.armed.set(
        armedFlag(declaration.effect.id),
        arena.slot(ValueType.bool),
      );
    }
  }
  const copies = new Map<IrDepEffect, DepCopy[]>();
  for (const effect of output.dependencyEffects) {
    copies.set(
      effect,
      effect.dependencies.map(dependency => {
        const type = depType(dependency);
        return {
          type,
          slot:
            type === 'string'
              ? arena.slot(ValueType.string, undefined, listStringCapacity - 1)
              : arena.slot(ValueType[type]),
        };
      }),
    );
  }

  const locals = new Map<string, number>();
  for (const body of bodies(app)) {
    stringLocals(body, locals);
  }
  for (const [name, capacity] of locals) {
    if (!slots.hoisted.has(name)) {
      slots.stringLocals.set(
        name,
        arena.slot(ValueType.string, undefined, capacity),
      );
    }
  }

  const nodes = nodesOf(app.root);
  // A <TextInput>'s onChangeText reads the input's text, through its handle.
  const handles = new Map<IrNode, Slot>();
  const readsInput = (node: IrNode): boolean =>
    node.wiring.some(
      wire => wire.kind === 'event' && wire.event === 'ER_EVENT_CHANGE_TEXT',
    );
  // An updateVector call draws on the node its ref holds: the last one built with it.
  const calls: IrUpdateVector[] = [];
  for (const body of bodies(app)) {
    updateVectors(body, calls);
  }
  const drawnRefs = new Set(calls.map(call => call.node));
  const refNodes = new Map<string, IrNode>();
  for (const node of nodes) {
    for (const wire of node.wiring) {
      if (wire.kind === 'ref' && drawnRefs.has(wire.slot)) {
        refNodes.set(wire.slot, node);
      }
    }
  }
  const drawnOn = new Set(refNodes.values());
  const reached = (node: IrNode): boolean =>
    isStateDriven(node) ||
    isSetOnce(node) ||
    readsInput(node) ||
    stateDrivenVectorOf(node) !== null;

  // A row's node keeps its handle in an array by the row, one array for each place in the row that needs one.
  const pools: Pool[] = [];
  poolsOf(app.root, pools);
  const rowHandles = new Map<IrNode, RowHandle>();
  for (const pool of pools) {
    const arrays: Slot[] = [];
    pool.rows.forEach((rowElement, row) =>
      nodesOf(rowElement).forEach((node, place) => {
        if (readsInput(node)) {
          notYetAt('onChangeText in a list row', node.location);
        }
        if (drawnOn.has(node)) {
          notYetAt('updateVector on a node in a list row', node.location);
        }
        if (reached(node)) {
          arrays[place] ??= arena.slot(ValueType.node, 0, pool.list.capacity);
          rowHandles.set(node, {array: arrays[place], row});
        }
      }),
    );
  }

  // The rows are built under their parent, before the node after them.
  const builtUnder = new Set(
    pools.flatMap(pool =>
      pool.anchor ? [pool.parent, pool.anchor] : [pool.parent],
    ),
  );
  for (const node of nodes) {
    if (
      !rowHandles.has(node) &&
      (reached(node) || builtUnder.has(node) || drawnOn.has(node))
    ) {
      handles.set(node, arena.slot(ValueType.node));
    }
  }
  for (const [ref, node] of refNodes) {
    slots.vectors.refNodes.set(ref, handles.get(node)!);
  }
  const inputText = nodes.some(readsInput)
    ? arena.slot(ValueType.string, undefined, ENGINE_STRING_MAX.ER_TEXT_MAX)
    : null;

  // Each PanResponder's gesture state.
  for (const pan of output.panResponders) {
    slots.gestures.set(pan.statePrefix, {
      granted: arena.slot(ValueType.bool),
      baseDeltaX: arena.slot(ValueType.int),
      baseDeltaY: arena.slot(ValueType.int),
      x0: arena.slot(ValueType.int),
      y0: arena.slot(ValueType.int),
    });
  }

  const requestsUpdate =
    output.handlers.some(handler =>
      handler.kind === 'callback'
        ? handler.callback.updatesState
        : handler.kind === 'panEvent' && !!handler.event.callback?.updatesState,
    ) ||
    output.timerFunctions.some(timer => timer.callback!.updatesState) ||
    output.mountEffects.some(mountUpdates) ||
    completionsOf(app).some(completion => completion.updatesState);
  const requested = requestsUpdate
    ? arena.track(arena.slot(ValueType.bool))
    : null;
  const hasEffects =
    output.dependencyEffects.length > 0 || output.mountEffects.length > 0;
  const once =
    hasEffects ||
    pools.length ||
    nodes.some(
      node =>
        isStateDriven(node) || isSetOnce(node) || stateDrivenVectorOf(node),
    )
      ? arena.track(arena.slot(ValueType.bool))
      : null;

  // A state an effect sets that a node reads waits for the next update in a pending flag.
  const effectWrites = new Set<StateRecord>();
  for (const effect of output.dependencyEffects) {
    statesSet(effect.body, effectWrites);
    statesSet(effect.cleanup ?? [], effectWrites);
  }
  const pending = new Map<Slot, Slot>();
  for (const state of effectWrites) {
    const slot = stateSlotOf(state, slots);
    if (slot.flags & SlotFlag.tracked) {
      pending.set(slot, arena.slot(ValueType.bool));
    }
  }
  const realBits = [requested, once].filter(slot => slot !== null);
  for (const [state, slot] of slots.states) {
    if (state.host && slot.flags & SlotFlag.tracked) {
      realBits.push(slot);
    }
  }
  // The first mount effect is queued by the first pass.
  const mountNext = output.mountEffects.length
    ? arena.slot(ValueType.int, 1)
    : null;

  if (output.timerFunctions.length) {
    arena.timerTable(caps.maxTimers);
  }
  // A pass queues each dep-driven effect and the next mount effect, and a mount effect the one after it.
  if (hasEffects) {
    arena.eventQueue(output.dependencyEffects.length + 1);
  }

  arena.layOut();
  return {
    slots,
    handles,
    rowHandles,
    rows,
    pools,
    inputText,
    requested,
    once,
    effects: {copies, pending, realBits, mountNext},
  };
}

/*----------------------------------------------------------------------------------------------------------------------
 - Templates
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * A node's constant props as a prop set: its style, then a placeholder,
 * text, and image known at compile time.
 *
 * @param node  The node.
 * @param lowering  The program being lowered.
 *
 * @returns The set's entries.
 */
function constantProps(node: IrNode, lowering: Lowering): ProgramPropEntry[] {
  const {tables} = lowering.slots;
  const entries = node.constantProps.map(prop =>
    constantPropEntry(prop.field, prop.value),
  );
  if (node.placeholder != null) {
    entries.push(
      stringPropEntry('placeholder', tables.string(node.placeholder)),
    );
  }

  const text = node.text ? constantText(node.text) : null;
  if (text !== null) {
    entries.push(stringPropEntry('text', tables.string(text)));
  }

  // The runtime only loads a package whose images are all in its own asset pack.
  if (node.image?.kind === 'static') {
    if (!lowering.images.has(node.image.name)) {
      throw aotError(
        `AOT: the package has no image named "${node.image.name}"`,
        'import the image file, so the build bakes it into the package.',
      );
    }
    entries.push(stringPropEntry('image_name', tables.string(node.image.name)));
  }

  return entries;
}

/**
 * The handler function an event on a node runs, made the first time. A handler bound to two events is one
 * function per event, since a function's kind is the event it is entered with, and an onChangeText one per
 * input, since it reads its own. Each row has its own handler; rows whose handlers have the same
 * code share one function, so they can share a template.
 *
 * @param name  The handler.
 * @param eventName  The event, an ER_EVENT_* name.
 * @param node  The node it is bound to.
 * @param lowering  The program being lowered.
 * @param inRow  Whether the node is in a list row.
 *
 * @returns The function's index.
 */
function handlerFunction(
  name: string,
  eventName: string,
  node: IrNode,
  lowering: Lowering,
  inRow: boolean,
): number {
  // A PanResponder's handlers are wired as a responder, not by name.
  const handler = lowering.app.output.handlers.find(
    candidate => candidate.kind !== 'panEvent' && candidate.name === name,
  );
  if (!handler || handler.kind === 'panEvent') {
    throw new Error(`AOT internal: no handler "${name}"`);
  }
  // A press fade runs the app's own handler for the event after it, when it has one.
  const then =
    handler.kind === 'pressFade'
      ? handler.then
        ? lowering.app.output.handlers.find(
            candidate =>
              candidate.kind === 'callback' && candidate.name === handler.then,
          )
        : null
      : handler;
  if (then === undefined || (then && then.kind !== 'callback')) {
    throw new Error(`AOT internal: no handler for "${name}"`);
  }
  const readsInput = eventName === 'ER_EVENT_CHANGE_TEXT';
  const made: Omit<HandlerFunction, 'index'> = {
    callback: then?.callback ?? {body: [], updatesState: false},
    fade: handler.kind === 'pressFade' ? handler.fade : null,
    event: ENGINE_VALUES.get(eventName)!,
    name,
    input: readsInput
      ? {node: lowering.handles.get(node)!, text: lowering.inputText!}
      : undefined,
    inRow,
  };
  // Compared by code alone, so where each handler is in the source doesn't keep two apart.
  const key = inRow
    ? `row ${JSON.stringify({...handlerBody({...made, index: NONE}, lowering), debug: undefined})}`
    : `${name} ${eventName}${readsInput ? ` ${node.id}` : ''}`;
  const existing = lowering.handlers.get(key);
  if (existing) {
    return existing.index;
  }
  const index = lowering.handlers.size;
  lowering.handlers.set(key, {...made, index});
  return index;
}

/**
 * A PanResponder's bindings: a handler for each of its events, then a query function for each predicate.
 * Each function is made once, however many nodes spread the responder.
 *
 * @param pan  The responder.
 * @param lowering  The program being lowered.
 *
 * @returns The bindings.
 */
function responderBindings(
  pan: IrPanResponder,
  lowering: Lowering,
): ProgramBinding[] {
  const made = (key: string, entry: Omit<HandlerFunction, 'index'>): number => {
    const existing = lowering.handlers.get(key);
    if (existing) {
      return existing.index;
    }
    const index = lowering.handlers.size;
    lowering.handlers.set(key, {...entry, index});
    return index;
  };
  const common = {fade: null, input: undefined, inRow: false};
  return [
    ...pan.events.map(event => {
      const kind = ENGINE_VALUES.get(event.event)!;
      return {
        kind: BindingKind.event,
        event: kind,
        target: made(`pan ${pan.functionPrefix} ${event.role}`, {
          ...common,
          callback: event.callback ?? {body: [], updatesState: false},
          event: kind,
          name: `${pan.name} ${event.role}`,
          gesture: {statePrefix: pan.statePrefix, role: event.role},
        }),
      };
    }),
    ...pan.queries.map(query => {
      const kind = ENGINE_VALUES.get(query.query)!;
      return {
        kind: BindingKind.query,
        event: kind,
        target: made(`pan ${pan.functionPrefix} query ${kind}`, {
          ...common,
          callback: {body: [], updatesState: false},
          event: FunctionKind.query + kind,
          name: `${pan.name} query`,
          test: query.test,
        }),
      };
    }),
  ];
}

/**
 * An element and everything under it as template nodes, each before its children. A conditional's branches
 * are all built, and the update shows the chosen one. A list's rows are row templates of their own. What a node
 * needs that a package can't lower yet is refused at the node's element.
 *
 * @param element  The element.
 * @param lowering  The program being lowered.
 * @param nodes  Collects the template nodes.
 * @param inRow  Whether the element is a list row's.
 */
function templateNodes(
  element: IrElement,
  lowering: Lowering,
  nodes: ProgramTemplateNode[],
  inRow = false,
): void {
  if (element.kind === 'component') {
    templateNodes(element.root, lowering, nodes, inRow);
    return;
  }

  const {node} = element;
  let children: IrElement[];
  try {
    const bindings: ProgramBinding[] = [];
    for (const wire of node.wiring) {
      if (wire.kind === 'bind') {
        bindings.push(lowering.slots.animations.binding(wire.bind));
      } else if (wire.kind === 'event') {
        bindings.push({
          kind: BindingKind.event,
          event: ENGINE_VALUES.get(wire.event)!,
          target: handlerFunction(
            wire.handler,
            wire.event,
            node,
            lowering,
            inRow,
          ),
        });
      } else if (wire.kind === 'responder') {
        bindings.push(...responderBindings(wire.pan, lowering));
      } else if (wire.kind === 'vectorTables') {
        // A drawing state drives is committed by the update instead, and a ref needs nothing itself.
        const tape = lowering.slots.vectors.tableTape(wire.tables);
        if (tape !== null) {
          bindings.push({kind: BindingKind.tape, event: 0, target: tape});
        }
      }
    }

    children = node.children.flatMap(child =>
      child.kind === 'element'
        ? [child.element]
        : child.kind === 'conditional'
          ? branchElements(child)
          : [],
    );

    // A node the update sets starts with no props.
    const handle = inRow
      ? lowering.rowHandles.get(node)?.array
      : lowering.handles.get(node);
    nodes.push({
      type: ENGINE_VALUES.get(node.type)!,
      children: children.length,
      props:
        isStateDriven(node) || isSetOnce(node)
          ? NONE
          : lowering.slots.tables.propSet(constantProps(node, lowering)),
      handle: handle?.index ?? NONE,
      bindings: bindings.length ? lowering.bindings.length : 0,
      bindingCount: bindings.length,
    });
    lowering.bindings.push(...bindings);
  } catch (error) {
    locateError(error, node.location);
    throw error;
  }

  for (const child of children) {
    templateNodes(child, lowering, nodes, inRow);
  }
}

/**
 * Each list row's template. Rows whose nodes and bindings are the same share one.
 *
 * @param lowering  The program being lowered.
 */
function rowTemplates(lowering: Lowering): void {
  const made = new Map<string, number>();
  for (const pool of lowering.pools) {
    pool.templates = pool.rows.map(row => {
      const first = lowering.bindings.length;
      const nodes: ProgramTemplateNode[] = [];
      templateNodes(row, lowering, nodes, true);
      const key = JSON.stringify({
        nodes: nodes.map(node => ({
          ...node,
          bindings: node.bindingCount ? node.bindings - first : 0,
        })),
        bindings: lowering.bindings.slice(first),
      });
      const existing = made.get(key);
      if (existing !== undefined) {
        lowering.bindings.length = first;
        return existing;
      }
      // The root template is first, so a row template's index is its place after it.
      const index = lowering.templates.push({kind: TemplateKind.row, nodes});
      made.set(key, index);
      return index;
    });
  }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Sets a node's props again: its constant set, what state drives, its image and its text, then spans when it
 * has them and the update sets it once.
 *
 * @param codegen  The update function's codegen.
 * @param node  The node.
 * @param lowering  The program being lowered.
 */
function setNode(codegen: Codegen, node: IrNode, lowering: Lowering): void {
  const {builder} = codegen;
  const {tables} = lowering.slots;
  const spans = isSetOnce(node) ? node.spans : null;
  if (spans && spans.length > MAX_SPANS) {
    notYet(`more than ${MAX_SPANS} nested <Text> spans`);
  }

  // Span styles are props too.
  const entries = constantProps(node, lowering);
  spans?.forEach((span, index) =>
    SPAN_FIELDS.forEach(field =>
      entries.push(
        constantPropEntry(
          `spans[${index}].${field}`,
          (span as TextSpan)[field],
        ),
      ),
    ),
  );
  builder.abx('PROPS', 0, tables.propSet(entries));

  for (const {field, value} of node.stateProps) {
    codegen.setProp(field, value);
  }
  if (node.image?.kind === 'value') {
    const image = node.image.value;
    codegen.scoped(() =>
      builder.abx(
        'PSETS',
        codegen.valueAs(image, 'string').index,
        abiProp('image_name').id,
      ),
    );
  }
  if (node.text && constantText(node.text) === null) {
    const {parts} = node.text;
    codegen.scoped(() => {
      codegen.text(parts);
      builder.abc('FMTTEXT');
    });
  }
  spans?.forEach((span, index) => {
    builder.abx('FMTK', 0, tables.string(span.text));
    builder.abc('FMTSPAN', index);
  });
  if (spans) {
    builder.abc('SPANS', spans.length);
  }

  codegen.scoped(() =>
    builder.abc('COMMIT', nodeHandle(codegen, node, lowering).index),
  );
}

/**
 * A node's handle: from its slot, or a list row's from its handle array.
 *
 * @param codegen  The function's codegen.
 * @param node  The node.
 * @param lowering  The program being lowered.
 *
 * @returns A node register.
 */
function nodeHandle(
  codegen: Codegen,
  node: IrNode,
  lowering: Lowering,
): Register {
  const {builder} = codegen;
  const handle = builder.register('node');
  const inRow = lowering.rowHandles.get(node);
  if (inRow) {
    builder.abc(
      'GETNI',
      handle.index,
      codegen.load('int', inRow.row).index,
      0,
      inRow.array.index,
    );
  } else {
    builder.abx('GET64', handle.index, lowering.handles.get(node)!.index);
  }
  return handle;
}

/**
 * Builds every list row, in order, under its parent and before the node after it.
 *
 * @param codegen  The update function's codegen.
 * @param lowering  The program being lowered.
 */
function buildRows(codegen: Codegen, lowering: Lowering): void {
  const {builder} = codegen;
  for (const pool of lowering.pools) {
    codegen.scoped(() => {
      const parent = nodeHandle(codegen, pool.parent, lowering);
      // BUILD before the parent itself puts the row last.
      const before = pool.anchor
        ? nodeHandle(codegen, pool.anchor, lowering)
        : parent;
      pool.templates.forEach((template, row) =>
        codegen.scoped(() =>
          builder.abc(
            'BUILD',
            builder.register('node').index,
            parent.index,
            before.index,
            template | (codegen.load('int', row).index << 16),
          ),
        ),
      );
    });
  }
}

/**
 * The dirty bits of a set of slots, as a mask per dirty word.
 *
 * @param slots  The slots.
 *
 * @returns Each word's mask.
 */
function dirtyMasks(slots: Slot[]): Map<number, number> {
  const words = new Map<number, number>();
  for (const slot of slots) {
    const word = slot.dirtyBit >> 5;
    words.set(
      word,
      ((words.get(word) ?? 0) | (1 << (slot.dirtyBit & 31))) >>> 0,
    );
  }
  return words;
}

/**
 * Runs `body` only when one of a set of dirty bits is set in this update pass, or one of a set of pending flags.
 *
 * @param builder  The update function.
 * @param slots  The slots whose bits guard it.
 * @param body  Emits what it guards.
 * @param pending  The pending flags that guard it too.
 */
function whenDirty(
  builder: FunctionBuilder,
  slots: Slot[],
  body: () => void,
  pending: Slot[] = [],
): void {
  const run = builder.label();
  const skip = builder.label();
  for (const [word, mask] of dirtyMasks(slots)) {
    builder.branchIfDirty(word, mask, run);
  }
  for (const flag of pending) {
    const checkpoint = builder.checkpoint();
    const set = builder.register('bool');
    builder.abx('GET32', set.index, flag.index);
    builder.branch('JT', run, set);
    builder.release(checkpoint);
  }
  builder.branch('JMP', skip);
  builder.place(run);
  body();
  builder.place(skip);
}

/**
 * Writes a constant to a 4-byte slot.
 *
 * @param codegen  The function's codegen.
 * @param slot  The slot.
 * @param value  The value.
 */
function setConstant(codegen: Codegen, slot: Slot, value: number): void {
  codegen.scoped(() =>
    codegen.builder.abx(
      'SET32',
      codegen.load(slot.type === ValueType.bool ? 'bool' : 'int', value).index,
      slot.index,
    ),
  );
}

/**
 * The update function: what a state change sets again, each node when a state it reads changed, and the nodes set
 * once in the first pass; then the effects it runs, queued.
 *
 * @param lowering  The program being lowered.
 *
 * @returns The function, or null when nothing needs it.
 */
function updateFunction(lowering: Lowering): ProgramFunction | null {
  const {output} = lowering.app;
  const nodes = nodesOf(lowering.app.root);
  const updated = nodes.filter(isStateDriven);
  const setOnce = nodes.filter(isSetOnce);
  const drawings = nodes.flatMap(node => {
    const vector = stateDrivenVectorOf(node);
    return vector ? [{node, vector}] : [];
  });
  if (
    !updated.length &&
    !setOnce.length &&
    !drawings.length &&
    !output.dependencyEffects.length &&
    !output.mountEffects.length
  ) {
    return null;
  }

  const builder = new FunctionBuilder(FunctionKind.update, 'the update');
  const codegen = new Codegen(builder, lowering.slots);
  const {pending, realBits, mountNext} = lowering.effects;
  const located = (node: IrNode, lower: () => void): void => {
    builder.at(node.location);
    try {
      lower();
    } catch (error) {
      locateError(error, node.location);
      throw error;
    }
  };
  const draw = (node: IrNode, vector: IrStateDrivenVector): void =>
    located(node, () =>
      commitDrawing(codegen, vector, lowering.slots.vectors, () =>
        nodeHandle(codegen, node, lowering),
      ),
    );
  // Set again when a state it reads changed; what reads something no state write flags (a ref, the clock) is
  // set on every pass.
  const whenRead = (reads: StateReads, lower: () => void): void => {
    const bits = reads
      ? [...reads].map(state => stateSlotOf(state, lowering.slots))
      : [];
    if (bits.length && bits.every(slot => slot.flags & SlotFlag.tracked)) {
      whenDirty(
        builder,
        bits,
        lower,
        bits.flatMap(slot => pending.get(slot) ?? []),
      );
    } else {
      lower();
    }
  };

  // A pass only an effect's writes started keeps what they changed for the next one.
  const end = builder.label();
  if (pending.size) {
    const real = builder.label();
    for (const [word, mask] of dirtyMasks(realBits)) {
      builder.branchIfDirty(word, mask, real);
    }
    for (const [state, flag] of pending) {
      whenDirty(builder, [state], () => setConstant(codegen, flag, 1));
    }
    builder.branch('JMP', end);
    builder.place(real);
  }

  // The rows, the props that read only boot values, and the drawings that do, built once.
  const builtOnce = drawings.filter(({node}) => !updatedVectorOf(node));
  if (setOnce.length || lowering.pools.length || builtOnce.length) {
    whenDirty(builder, [lowering.once!], () => {
      buildRows(codegen, lowering);
      setOnce.forEach(node =>
        located(node, () => setNode(codegen, node, lowering)),
      );
      builtOnce.forEach(({node, vector}) => draw(node, vector));
    });
  }

  // The nodes, then the drawings.
  for (const node of updated) {
    located(node, () =>
      whenRead(nodeReads(node), () => setNode(codegen, node, lowering)),
    );
  }
  for (const {node, vector} of drawings) {
    if (updatedVectorOf(node)) {
      whenRead(vectorReads(vector), () => draw(node, vector));
    }
  }
  for (const flag of pending.values()) {
    setConstant(codegen, flag, 0);
  }

  // Each dep-driven effect checks its deps when it runs, each in turn.
  for (const check of lowering.effectFunctions.checks) {
    builder.abx('EFFECT', 0, check);
  }

  // The first mount effect, then each one after a mount effect that set state.
  if (mountNext) {
    const {mounts} = lowering.effectFunctions;
    const queued = output.mountEffects.flatMap((_, index) =>
      index === 0 || mountUpdates(output.mountEffects[index - 1])
        ? [index]
        : [],
    );
    for (const index of queued) {
      codegen.scoped(() => {
        const skip = builder.label();
        const next = builder.register('int');
        const isNext = builder.register('bool');
        builder.abx('GET32', next.index, mountNext.index);
        builder.abc(
          'EQ',
          isNext.index,
          next.index,
          codegen.load('int', index + 1).index,
        );
        builder.branch('JF', skip, isNext);
        builder.abx('EFFECT', 0, mounts[index]);
        builder.place(skip);
      });
    }
    setConstant(codegen, mountNext, 0);
  }
  builder.place(end);
  return builder.finish();
}

/**
 * Toggles the request bit, so the update runs after a function that set state, even when no value changed.
 *
 * @param codegen  The function's codegen.
 * @param lowering  The program being lowered.
 */
function requestUpdate(codegen: Codegen, lowering: Lowering): void {
  const {requested} = lowering;
  if (!requested) {
    return;
  }
  const {builder} = codegen;
  codegen.scoped(() => {
    const flag = builder.register('bool');
    builder.abx('GET32', flag.index, requested.index);
    builder.abc('NOT', flag.index, flag.index);
    builder.abx('SET32', flag.index, requested.index);
  });
}

/**
 * A handler's function.
 *
 * @param handler  The handler.
 * @param lowering  The program being lowered.
 *
 * @returns The function.
 */
function handlerBody(
  handler: HandlerFunction,
  lowering: Lowering,
): ProgramFunction {
  const builder = new FunctionBuilder(handler.event, `handler ${handler.name}`);
  // A row's handler is entered with its row, in its first register.
  const row = handler.inRow ? builder.reserved('int') : undefined;
  const codegen = new Codegen(builder, lowering.slots, {
    input: handler.input,
    row,
  });
  if (handler.test) {
    return builder.finish(0, codegen.condition(handler.test));
  }
  if (handler.fade) {
    startPressFade(codegen, handler.fade, lowering.slots.animations);
  }
  const end = builder.label();
  if (handler.gesture) {
    keepGesture(codegen, handler.gesture, 'before', end, lowering);
  }
  codegen.statements(handler.callback.body);
  if (handler.callback.updatesState) {
    requestUpdate(codegen, lowering);
  }
  if (handler.gesture) {
    keepGesture(codegen, handler.gesture, 'after', end, lowering);
  }
  builder.place(end);
  return builder.finish(row ? 1 : 0);
}

/**
 * What a PanResponder event's handler keeps of the gesture around the app's callback: the grant
 * anchors it, unless it is already granted; release and terminate clear it after the callback; those and move
 * do nothing unless it is granted.
 *
 * @param codegen  The handler's codegen.
 * @param gesture  The responder's gesture state, and the handler's role.
 * @param when  Before the callback, or after it.
 * @param end  The end of the handler, where it returns.
 * @param lowering  The program being lowered.
 */
function keepGesture(
  codegen: Codegen,
  gesture: NonNullable<HandlerFunction['gesture']>,
  when: 'before' | 'after',
  end: Label,
  lowering: Lowering,
): void {
  const {builder} = codegen;
  const slots = lowering.slots.gestures.get(gesture.statePrefix)!;
  const {role} = gesture;
  codegen.scoped(() => {
    if (when === 'before' && role !== 'reject') {
      const granted = codegen.get(slots.granted);
      builder.branch(role === 'grant' ? 'JT' : 'JF', end, granted);
    }
    if (when === 'before' && role === 'grant') {
      codegen.set(slots.granted, codegen.load('bool', 1));
      const fields: [Slot, number][] = [
        [slots.baseDeltaX, EventField.dx],
        [slots.baseDeltaY, EventField.dy],
        [slots.x0, EventField.x],
        [slots.y0, EventField.y],
      ];
      for (const [slot, field] of fields) {
        const value = builder.register('int');
        builder.abc('EVT', value.index, field);
        codegen.set(slot, value);
      }
    }
    if (when === 'after' && (role === 'release' || role === 'terminate')) {
      codegen.set(slots.granted, codegen.load('bool', 0));
      const zero = codegen.load('int', 0);
      codegen.set(slots.baseDeltaX, zero);
      codegen.set(slots.baseDeltaY, zero);
    }
  });
}

/**
 * A setTimeout or setInterval callback's function.
 *
 * @param name  The callback's name.
 * @param callback  The callback.
 * @param lowering  The program being lowered.
 *
 * @returns The function.
 */
function timerBody(
  name: string,
  callback: IrCallback,
  lowering: Lowering,
): ProgramFunction {
  const builder = new FunctionBuilder(FunctionKind.timer, `timer ${name}`);
  const codegen = new Codegen(builder, lowering.slots);
  codegen.statements(callback.body);
  if (callback.updatesState) {
    requestUpdate(codegen, lowering);
  }
  return builder.finish();
}

/**
 * An animation completion's helper: entered with what it was passed, which the body reads as `finished`.
 *
 * @param completion  The completion.
 * @param lowering  The program being lowered.
 *
 * @returns The function.
 */
function completionBody(
  completion: IrCompletion,
  lowering: Lowering,
): ProgramFunction {
  const builder = new FunctionBuilder(
    FunctionKind.helper,
    `completion ${completion.name}`,
  );
  const finished = builder.reserved('bool');
  const codegen = new Codegen(builder, lowering.slots, {finished});
  codegen.statements(completion.body);
  if (completion.updatesState) {
    requestUpdate(codegen, lowering);
  }
  return builder.finish(1);
}

/**
 * The register-stack cells each function's deepest call path takes, its own frame included, as the runtime's
 * verifier works it out. A function only calls later ones.
 *
 * @param functions  The functions.
 *
 * @returns The deepest.
 */
function stackCells(functions: ProgramFunction[]): number {
  const depths: number[] = [];
  for (let index = functions.length - 1; index >= 0; index--) {
    const {code, types} = functions[index];
    let deepest = 0;
    for (let at = 0; at < code.length; at += wordsOf(code[at])) {
      if ((code[at] & 0xff) === OP.CALL.code) {
        deepest = Math.max(deepest, depths[code[at] >>> 16]);
      }
    }
    depths[index] = FRAME_CELLS + types.length + deepest;
  }
  return Math.max(0, ...depths);
}

/**
 * How many of some instructions a function has.
 *
 * @param programFunction  The function.
 * @param names  The opcodes.
 *
 * @returns The count.
 */
function countOps(programFunction: ProgramFunction, names: string[]): number {
  const codes = new Set(names.map(name => OP[name].code));
  let count = 0;
  for (
    let at = 0;
    at < programFunction.code.length;
    at += wordsOf(programFunction.code[at])
  ) {
    count += codes.has(programFunction.code[at] & 0xff) ? 1 : 0;
  }
  return count;
}

/**
 * An instruction's length, from its first word.
 *
 * @param word  The word.
 *
 * @returns Its length in words.
 */
const wordsOf = (word: number): number =>
  Object.values(OP).find(opcode => opcode.code === (word & 0xff))!.words;

/**
 * A dep-driven effect's run: the last run's cleanup, if that run reached it, then the body.
 *
 * @param effect  The effect.
 * @param codegen  The function's codegen.
 * @param lowering  The program being lowered.
 */
function runEffect(
  effect: IrDepEffect,
  codegen: Codegen,
  lowering: Lowering,
): void {
  const {builder} = codegen;
  if (effect.hasCleanup) {
    const armed = lowering.slots.armed.get(armedFlag(effect.id))!;
    const skip = builder.label();
    codegen.scoped(() => {
      const flag = builder.register('bool');
      builder.abx('GET32', flag.index, armed.index);
      builder.branch('JF', skip, flag);
    });
    setConstant(codegen, armed, 0);
    codegen.statements(effect.cleanup ?? []);
    builder.place(skip);
  }
  codegen.statements(effect.body);
}

/**
 * The function a dep-driven effect runs in after an update pass: it copies each dep that changed,
 * and runs the effect if one did.
 *
 * @param effect  The effect.
 * @param lowering  The program being lowered.
 *
 * @returns The function.
 */
function checkedEffect(
  effect: IrDepEffect,
  lowering: Lowering,
): ProgramFunction {
  const builder = new FunctionBuilder(
    FunctionKind.effect,
    `effect ${effect.id}`,
  );
  const end = builder.label();
  const codegen = new Codegen(builder, lowering.slots, {onReturn: end});
  const copies = lowering.effects.copies.get(effect)!;
  const changed = builder.register('bool');
  builder.abx('LOADI', changed.index, 0);
  effect.dependencies.forEach((dependency, index) => {
    const {slot, type} = copies[index];
    codegen.scoped(() => {
      const value = codegen.valueAs(dependency, type);
      const same = builder.label();
      builder.branch('JT', same, codegen.same(value, codegen.get(slot)));
      codegen.set(slot, value);
      builder.abx('LOADI', changed.index, 1);
      builder.place(same);
    });
  });
  builder.branch('JF', end, changed);
  runEffect(effect, codegen, lowering);
  builder.place(end);
  return builder.finish();
}

/**
 * The function a mount effect runs in, after the first update. One that set
 * state has the next pass queue the mount effect after it; any other queues it itself.
 *
 * @param index  The mount effect's place in the order.
 * @param lowering  The program being lowered.
 *
 * @returns The function.
 */
function mountEffect(index: number, lowering: Lowering): ProgramFunction {
  const {output} = lowering.app;
  const mount = output.mountEffects[index];
  const builder = new FunctionBuilder(
    FunctionKind.effect,
    `mount effect ${index}`,
  );
  const end = builder.label();
  const codegen = new Codegen(builder, lowering.slots, {onReturn: end});
  switch (mount.kind) {
    case 'inline':
      codegen.statements(mount.callback.body);
      break;
    case 'call': {
      const called = output.effectFunctions.find(
        effectFunction =>
          effectFunction.kind === 'mount' && effectFunction.name === mount.name,
      );
      codegen.statements(called?.kind === 'mount' ? called.body : []);
      break;
    }
    case 'depEffect':
      runEffect(mount.effect, codegen, lowering);
      break;
  }
  builder.place(end);

  // A dep-driven effect keeps a copy of each dep, taken after it ran.
  if (mount.kind === 'depEffect') {
    const copies = lowering.effects.copies.get(mount.effect)!;
    mount.effect.dependencies.forEach((dependency, dependencyIndex) =>
      codegen.scoped(() =>
        codegen.set(
          copies[dependencyIndex].slot,
          codegen.valueAs(dependency, copies[dependencyIndex].type),
        ),
      ),
    );
  }

  const hasNext = index + 1 < output.mountEffects.length;
  if (mountUpdates(mount)) {
    requestUpdate(codegen, lowering);
    if (hasNext) {
      setConstant(codegen, lowering.effects.mountNext!, index + 2);
    }
  } else if (hasNext) {
    builder.abx('EFFECT', 0, lowering.effectFunctions.mounts[index + 1]);
  }
  return builder.finish();
}

/**
 * Appends a list's first items: a missing field is 0 or "", and a whole
 * number field is the value rounded.
 *
 * @param list  The list.
 * @param codegen  The boot function's codegen.
 * @param lowering  The program being lowered.
 */
function appendInitialItems(
  list: ListState,
  codegen: Codegen,
  lowering: Lowering,
): void {
  const {builder} = codegen;
  const {fields} = list.struct;
  for (const item of list.items) {
    codegen.scoped(() => {
      const row = builder.consecutive(
        fields.map(field => (field.kind === 'string' ? 'string' : field.kind)),
      );
      fields.forEach((field, index) => {
        const value = item[field.key];
        if (field.kind === 'string') {
          builder.abx(
            'LOADS',
            row[index].index,
            lowering.slots.tables.string(String(value ?? '')),
          );
          return;
        }
        const number = Number(value) || 0;
        codegen.scoped(() =>
          builder.abc(
            'MOVE',
            row[index].index,
            codegen.load(
              field.kind as 'int' | 'float',
              field.kind === 'float' ? number : Math.round(number),
            ).index,
          ),
        );
      });
      builder.abx(
        'APPEND',
        row[0].index,
        lowering.slots.lists.get(list)!.slot.index,
      );
    });
  }
}

/**
 * The boot function: the values worked out from the panel size, in double precision, and
 * each list's first items.
 *
 * @param bootSlots  The values.
 * @param lowering  The program being lowered.
 *
 * @returns The function, or null when the app has none.
 */
function bootFunction(
  bootSlots: IrBootSlot[],
  lowering: Lowering,
): ProgramFunction | null {
  const lists = lowering.app.state.filter(
    (state): state is ListState =>
      state.kind === 'list' && state.items.length > 0,
  );
  // A slot read only by a state's start is among bootSlots.
  if (!bootSlots.length && !lists.length) {
    return null;
  }
  const builder = new FunctionBuilder(FunctionKind.boot, 'the boot values');
  const codegen = new Codegen(builder, lowering.slots, {inDoubles: true});
  for (const list of lists) {
    appendInitialItems(list, codegen, lowering);
  }
  for (const bootSlot of bootSlots) {
    const slot = lowering.slots.boot.get(bootSlot)!;
    const {initializer} = bootSlot;
    codegen.scoped(() => {
      if (initializer.kind === 'screen') {
        const size = builder.register('int');
        builder.abx(
          'SCREEN',
          size.index,
          initializer.dimension === 'width' ? 0 : 1,
        );
        codegen.set(slot, size);
      } else {
        codegen.set(slot, codegen.value(initializer.expression));
      }
    });
  }

  // A state that starts at a value worked out at boot, after every value it can read.
  for (const state of lowering.app.state) {
    if (state.kind !== 'scalar' || !state.bootInitial) continue;
    const {bootInitial} = state;
    codegen.scoped(() =>
      codegen.set(
        lowering.slots.states.get(state)!,
        codegen.value({
          kind: 'boot',
          slot: bootInitial,
          type: bootInitial.type,
        }),
      ),
    );
  }
  return builder.finish();
}

/**
 * The keyboard table: setKeyboardConfig's look and layout, each size and key field cut to the bytes the engine's
 * ERKeyboardConfig keeps it in.
 *
 * @param keyboard  The keyboard.
 * @param tables  The program's tables.
 *
 * @returns The table.
 */
function keyboardTable(
  keyboard: IrKeyboard,
  tables: ProgramTables,
): ProgramKeyboard {
  const byte = (value: number | null): number => (value ?? 0) & 0xff;
  const string = (text: string | null): number =>
    text == null ? NONE : tables.string(text);
  return {
    layers: (keyboard.layers ?? []).map(layer =>
      layer.map(row =>
        row.map(key => ({
          label: string(key.label),
          text: string(key.text),
          type: ENGINE_VALUES.get(KEY_TYPES[key.kind])!,
          layer: byte(key.layer),
          span: byte(key.span),
          highlight: byte(key.highlightLayer ?? NEVER_HIGHLIGHTED),
        })),
      ),
    ),
    rowHeight: (keyboard.rowHeight ?? 0) & 0xffff,
    gridColumns: byte(keyboard.gridColumns),
    keyGap: byte(keyboard.keyGap),
    keyRadius: byte(keyboard.keyRadius),
    fontSize: byte(keyboard.fontSize),
    panelColor: keyboard.panelColor ?? 0,
    keyColor: keyboard.keyColor ?? 0,
    keyActiveColor: keyboard.keyActiveColor ?? 0,
    labelColor: keyboard.labelColor ?? 0,
  };
}

/*----------------------------------------------------------------------------------------------------------------------
 - Program
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Lowers an app to a program for one panel.
 *
 * @param app  The lowered app.
 * @param options  The compile's options: the panel, and the string cap.
 *
 * @returns The program.
 */
export function lowerPackage(app: IrApp, options: ResolvedOptions): Program {
  const {screen} = options;
  const tables = new ProgramTables();
  const lowering: Lowering = {
    app,
    ...planSlots(app, tables, options.caps),
    images: new Set(app.images.map(image => image.name)),
    handlers: new Map(),
    bindings: [],
    templates: [],
    effectFunctions: {checks: [], mounts: []},
  };
  const {output} = app;

  // A row's handlers are compared by their code before the functions are numbered, so each timer they start
  // stands in by its own place until then.
  output.timerFunctions.forEach((timer, index) =>
    lowering.slots.timerFunctions.set(timer.name, index),
  );

  // A root the app's tree fills. The runtime sizes it to the real panel.
  const nodes: ProgramTemplateNode[] = [
    {
      type: ENGINE_VALUES.get('ER_NODE_VIEW')!,
      children: 1,
      props: tables.propSet([]),
      handle: NONE,
      bindings: 0,
      bindingCount: 0,
    },
  ];
  templateNodes(app.root, lowering, nodes);
  rowTemplates(lowering);

  // Every row is built, whether the list holds it or not.
  const built = [
    ...nodes,
    ...lowering.pools.flatMap(pool =>
      pool.templates.flatMap(
        template => lowering.templates[template - 1].nodes,
      ),
    ),
  ];
  const nodeCount = built.length;
  const vectorType = ENGINE_VALUES.get('ER_NODE_VECTOR');
  const vectorNodes = built.filter(node => node.type === vectorType).length;
  if (nodeCount > MAX_U16) {
    throw aotError(
      `AOT: the app needs ${nodeCount} nodes, but a package holds ${MAX_U16}`,
      'lower ER_AOT_LIST_CAP (the listCap option), or give the list rows fewer nodes.',
    );
  }
  // Handlers first, as the bindings number them, then the timer callbacks, the effects, and the boot and
  // update functions. Each is numbered before any code is written, since code names them.
  let next = lowering.handlers.size;
  for (const timer of output.timerFunctions) {
    lowering.slots.timerFunctions.set(timer.name, next++);
  }
  lowering.effectFunctions = {
    checks: output.dependencyEffects.map(() => next++),
    mounts: output.mountEffects.map(() => next++),
  };
  // Then the animations' done functions, and the completions they and the rest call, after every caller.
  const {animations} = lowering.slots;
  animations.number(next, next + animations.doneFunctions.size);
  const functions = [
    ...[...lowering.handlers.values()].map(handler =>
      handlerBody(handler, lowering),
    ),
    ...output.timerFunctions.map(timer =>
      timerBody(timer.name, timer.callback!, lowering),
    ),
    ...output.dependencyEffects.map(effect => checkedEffect(effect, lowering)),
    ...output.mountEffects.map((_, index) => mountEffect(index, lowering)),
    ...doneFunctions(
      animations,
      builder => new Codegen(builder, lowering.slots),
    ),
    ...[...animations.completions.values()].map(completion =>
      completionBody(completion, lowering),
    ),
  ];
  const boot = bootFunction(app.boot.slots, lowering);
  const update = updateFunction(lowering);
  const bootIndex = boot ? functions.push(boot) - 1 : NONE;
  const updateIndex = update ? functions.push(update) - 1 : NONE;

  // A completion that arrives while a function runs waits in the queue: each running tree's, and each one a
  // single function's starts and stops can end at once.
  if (animations.entries) {
    const ends = Math.max(
      0,
      ...functions.map(programFunction =>
        countOps(programFunction, ['ASTART', 'AVSET', 'ASTOP']),
      ),
    );
    lowering.slots.arena.eventQueue(animations.entries + 2 * ends);
  }
  const {regions, bytes} = lowering.slots.arena.regions(stackCells(functions));

  // Host values, by name, for erb_input_find().
  const inputs = app.state
    .filter(
      (state): state is ScalarState => state.kind === 'scalar' && state.host,
    )
    .sort((left, right) =>
      left.name < right.name ? -1 : left.name > right.name ? 1 : 0,
    )
    .map(state => ({
      name: tables.string(state.name),
      slot: lowering.slots.states.get(state)!.index,
    }));

  return {
    flags: FLAG_DEBUG,
    panels: [
      {
        width: screen.width,
        height: screen.height,
        check: lowerCheck(app.boot.assumptions, screen),
        arenaBytes: bytes,
        nodeCount,
        rootTemplate: 0,
        bootFunction: bootIndex,
        updateFunction: updateIndex,
      },
    ],
    needs: [
      ...(vectorNodes
        ? [{resource: Resource.vectorNodes, amount: vectorNodes}]
        : []),
      // Each animated value runs one engine animation at a time.
      ...(animations.values.size
        ? [
            {
              resource: Resource.animationValues,
              amount: animations.values.size,
            },
            {resource: Resource.animations, amount: animations.values.size},
          ]
        : []),
      ...(app.images.length
        ? [{resource: Resource.images, amount: app.images.length}]
        : []),
    ],
    strings: tables.strings,
    const32: tables.const32,
    const64: tables.const64,
    regions,
    slots: lowering.slots.arena.slots,
    enums: [...lowering.slots.enums.values()].map(({values}) =>
      values.map(value => tables.string(value)),
    ),
    rows: lowering.rows,
    propSets: tables.propSets,
    templates: [{kind: TemplateKind.root, nodes}, ...lowering.templates],
    bindings: lowering.bindings,
    functions,
    animations: animations.animations,
    inputs,
    interpolations: animations.interpolations,
    tapes: lowering.slots.vectors.tapes,
    keyboard: app.keyboard ? keyboardTable(app.keyboard, tables) : null,
  };
}
