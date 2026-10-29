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
 * bytecode (codegen.mts). The update re-sets a node when a state it reads changed, as app_update() does, sets
 * the props of a node that reads only boot values once, in its first pass, and queues the effects. What isn't
 * lowered yet (animations, responders, drawings, the keyboard) is refused with what it is.
 *
 * A list is a pool of rows in the arena. Its `.map` is a row template, built once per row in the update's first
 * pass, as er_app_build() builds every row; a row shows while the list holds it. Rows whose nodes and handlers
 * are the same share a template, and a row handler reads its item through the row it was built for.
 *
 * Effects run as the C output runs them. An effect runs inside app_update(), so the state it sets shows at
 * the next app_update(), not at once. The VM runs a pass whenever state changed, so a pass that only an
 * effect's write started keeps what changed in a pending flag, and the next pass a handler, a timer or the
 * load started sets it. A dep-driven effect checks its deps itself, when it runs, as app_update() checks
 * each in turn. The mount effects run one after another, after the first pass; one that set state has the
 * pass after it queue the next.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {traverse} from '@babel/core';
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
} from '../ir/nodes.mts';
import {nodeReads} from '../ir/reads.mts';
import {armedFlag} from '../c/functions.mts';
import {Arena} from './arena.mts';
import {FunctionBuilder} from './bytecode.mts';
import {internedStates} from './enums.mts';
import {Codegen, notYet} from './codegen.mts';
import {
  BindingKind,
  ENGINE_VALUES,
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
import type {Program as BabelProgram, Node} from '@babel/types';
import type {Slot} from './arena.mts';
import type {Register} from './bytecode.mts';
import type {CType, InputText, ProgramSlots} from './codegen.mts';
import type {SourcePosition} from '../diagnostics.mts';
import type {IrApp} from '../ir/app.mts';
import type {IrBootSlot} from '../ir/boot.mts';
import type {IrExpr} from '../ir/expressions.mts';
import type {IrDepEffect, IrMountEffect} from '../ir/functions.mts';
import type {IrCallback, IrStmt} from '../ir/statements.mts';
import type {IrElement, IrNode, IrWire} from '../ir/nodes.mts';
import type {ResolvedOptions} from '../options.mts';
import type {TextSpan} from '../style-text.mts';
import type {ListState, ScalarState, StateRecord} from '../types.mts';
import type {
  Program,
  ProgramBinding,
  ProgramFunction,
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
  event: number;
  name: string;
  /** For a <TextInput>'s onChangeText, the input it reads. */
  input: InputText | undefined;
  /** Bound in a list row: entered with the row it was built for. */
  inRow: boolean;
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

/** A dep-driven effect's copy of one dep, and the type C keeps it in. */
interface DepCopy {
  slot: Slot;
  type: CType;
}

/** What the effects keep between runs, and the passes that run them. */
interface EffectSlots {
  /** Each dep-driven effect's copy of each dep. */
  copies: Map<IrDepEffect, DepCopy[]>;
  /** Each state an effect sets, by its slot: set while a change no app_update() has set yet waits. */
  pending: Map<Slot, Slot>;
  /** The bits that start an app_update(): a handler's or timer's request, the load, and the host. */
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
  /** Toggled by a handler that set state, so the update runs as app_update() would. */
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

/** What each wiring a node can have needs, as a refusal names it. */
const WIRING_USES: Record<IrWire['kind'], string> = {
  bind: 'animations',
  ref: 'refs',
  event: 'event handlers',
  responder: 'PanResponder',
  vectorTables: '<Svg>',
  stateDrivenVector: '<Svg>',
};

/** The span fields a TextSpan spells, in its ABI ids' order. */
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
 * Refuses what the package output can't lower yet, pointing at where it is.
 *
 * @param what  What the app uses.
 * @param loc  Where in the source, when it is known.
 */
function notYetAt(what: string, loc?: SourcePosition): never {
  try {
    notYet(what);
  } catch (error) {
    locateError(error, loc);
    throw error;
  }
}

/**
 * Where the module first calls one of the given functions, for a refusal of what has no node of its own.
 *
 * @param program  The parsed module.
 * @param names  The functions: a bare name (`useEffect`), or an object and member (`Animated.timing`), with
 *               `*` for any member.
 *
 * @returns The call's position, or undefined when there is none.
 */
function firstCall(
  program: BabelProgram,
  names: string[],
): SourcePosition | undefined {
  const named = (callee: Node): string | null =>
    callee.type === 'Identifier'
      ? callee.name
      : callee.type === 'MemberExpression' &&
          callee.object.type === 'Identifier' &&
          callee.property.type === 'Identifier'
        ? `${callee.object.name}.${callee.property.name}`
        : null;
  const matches = (name: string | null): boolean =>
    name !== null &&
    names.some(wanted =>
      wanted.endsWith('.*')
        ? name.startsWith(wanted.slice(0, -1))
        : name === wanted || name.endsWith(`.${wanted}`),
    );

  let found: SourcePosition | undefined;
  traverse(program, {
    'CallExpression|NewExpression'(path) {
      const call = path.node as Node & {callee: Node};
      if (matches(named(call.callee))) {
        found = call.loc?.start;
        path.stop();
      }
    },
  });
  return found;
}

/**
 * Refuses what the app uses, beyond its tree and handlers, that the package output doesn't lower yet. What a
 * node uses is refused at the node.
 *
 * @param app  The app.
 */
function checkSupported(app: IrApp): void {
  const {out} = app;
  const uses: [boolean, string, string[]][] = [
    [
      app.animations.length > 0 || out.animCbs.length > 0,
      'animations',
      ['Animated.*', 'useAnimatedValue'],
    ],
    [out.panResponders.length > 0, 'PanResponder', ['PanResponder.create']],
    [out.vectors.length > 0, '<Svg>', []],
    [app.keyboard !== null, 'setKeyboardConfig', ['setKeyboardConfig']],
  ];
  for (const [used, what, calls] of uses) {
    if (used) {
      notYetAt(what, calls.length ? firstCall(out.program, calls) : undefined);
    }
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
 * @param expr  The value.
 *
 * @returns The bytes.
 */
function longestLiteral(expr: IrExpr): number {
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
  walk(expr);
  return Math.max(longest, 1);
}

/**
 * Every string local a statement list declares, with its cap.
 *
 * @param stmts  The statements.
 * @param found  Collects each local's name and cap.
 */
function stringLocals(stmts: IrStmt[], found: Map<string, number>): void {
  for (const stmt of stmts) {
    if (stmt.kind === 'declareLocal' && stmt.init.type.kind === 'string') {
      found.set(
        stmt.name,
        Math.max(found.get(stmt.name) ?? 0, stmt.stringCap - 1),
      );
    } else if (stmt.kind === 'if') {
      stringLocals(stmt.consequent, found);
      stringLocals(stmt.alternate ?? [], found);
    }
  }
}

/**
 * Every state and list a statement list sets.
 *
 * @param stmts  The statements.
 * @param found  Collects the states.
 */
function statesSet(stmts: IrStmt[], found: Set<StateRecord>): void {
  for (const stmt of stmts) {
    if (stmt.kind === 'setState' || stmt.kind === 'setStateText') {
      found.add(stmt.state);
    } else if ('list' in stmt) {
      found.add(stmt.list);
    } else if (stmt.kind === 'if') {
      statesSet(stmt.consequent, found);
      statesSet(stmt.alternate ?? [], found);
    }
  }
}

/**
 * The type C keeps a dep's copy in.
 *
 * @param dep  The dep.
 *
 * @returns Its C type.
 */
function depType(dep: IrExpr): CType {
  switch (dep.type.kind) {
    case 'bool':
    case 'float':
    case 'int64':
    case 'string':
      return dep.type.kind;
    default:
      return 'int';
  }
}

/**
 * Whether a mount effect sets state, so app_update() runs after it.
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
  const {out} = app;
  return [
    ...out.handlers.flatMap(handler =>
      handler.kind === 'callback' ? [handler.callback.body] : [],
    ),
    ...out.timerFns.map(timer => timer.callback!.body),
    ...out.effectFns.map(effectFunction =>
      effectFunction.kind === 'mount'
        ? effectFunction.body
        : effectFunction.kind === 'cleanup'
          ? effectFunction.effect.cleanup!
          : effectFunction.effect.body,
    ),
    ...out.mountEffects.flatMap(mountEffect =>
      mountEffect.kind === 'inline' ? [mountEffect.callback.body] : [],
    ),
  ];
}

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
          notYetAt('a list inside a list row', nested.loc);
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
 * @param caps  The C output's buffer sizes, and the timers that run at once.
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
  const {listStrCap} = caps;
  const {out} = app;
  const arena = new Arena();
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
    const slot = interned
      ? Object.assign(arena.slot(ValueType.enum, 0), {aux: interned.set})
      : state.cType === 'string'
        ? arena.slot(
            ValueType.string,
            tables.string(String(state.initial)),
            listStrCap - 1,
          )
        : state.cType === 'i64'
          ? arena.slot(
              ValueType.int64,
              tables.constant64(BigInt(Math.trunc(Number(state.initial)))),
            )
          : state.cType === 'float'
            ? arena.slot(ValueType.float, floatBits(Number(state.initial)))
            : arena.slot(
                state.isBool ? ValueType.bool : ValueType.int,
                Math.trunc(Number(state.initial)) | 0,
              );
    // A host value an effect's deps may read runs app_update() when it is set, as its C setter does.
    if (state.isWatched || (state.host && out.depEffects.length)) {
      arena.track(slot);
    }
    slots.states.set(state, slot);
  }

  // A list is a pool of rows, empty at load; the boot function appends its first items. Each row field is
  // 4-aligned, and a string field holds what C's char array does.
  const rows: ProgramRow[] = [];
  for (const state of app.state) {
    if (state.kind !== 'list') {
      continue;
    }
    if (!state.struct.fields.length) {
      notYet(`list state "${state.name}", whose items have no fields`);
    }
    let size = 0;
    const fields = state.struct.fields.map(field => {
      const cap = field.kind === 'string' ? listStrCap - 1 : 0;
      const entry = {type: ValueType[field.kind], cap, offset: size};
      size += field.kind === 'string' ? (cap + 1 + 3) & ~3 : 4;
      return entry;
    });
    const slot = arena.list(rows.length, size, state.cap);
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
          arena.slot(ValueType.string, undefined, listStrCap - 1),
        ]),
    );
    slots.lists.set(state, {slot, buffers});
  }

  for (const ref of app.refs) {
    if (ref.kind !== 'value') {
      continue;
    }
    slots.refs.set(
      ref,
      ref.cType === 'i64'
        ? arena.slot(
            ValueType.int64,
            tables.constant64(BigInt(Math.trunc(ref.initial))),
          )
        : ref.cType === 'float'
          ? arena.slot(ValueType.float, floatBits(ref.initial))
          : arena.slot(ValueType.int, Math.trunc(ref.initial) | 0),
    );
  }

  // Boot values: C keeps a fraction as a double.
  for (const bootSlot of app.boot.slots) {
    const slot =
      bootSlot.type.kind === 'float'
        ? arena.slot(ValueType.double)
        : bootSlot.type.kind === 'string'
          ? arena.slot(
              ValueType.string,
              undefined,
              bootSlot.init.kind === 'expr'
                ? longestLiteral(bootSlot.init.expr)
                : 1,
            )
          : arena.slot(
              bootSlot.type.kind === 'bool' ? ValueType.bool : ValueType.int,
            );
    slot.flags |= SlotFlag.bootOnly;
    slots.boot.set(bootSlot, slot);
  }

  // The effect locals a cleanup reads, and each effect's armed flag and copy of each dep, kept as C keeps them.
  for (const decl of out.effectDecls) {
    if (decl.kind === 'local') {
      slots.hoisted.set(
        decl.name,
        decl.cType === 'string'
          ? arena.slot(ValueType.string, undefined, listStrCap - 1)
          : arena.slot(
              decl.cType === 'i64'
                ? ValueType.int64
                : decl.cType === 'float'
                  ? ValueType.float
                  : ValueType.int,
            ),
      );
    } else if (decl.kind === 'armed') {
      slots.armed.set(armedFlag(decl.effect.id), arena.slot(ValueType.bool));
    }
  }
  const copies = new Map<IrDepEffect, DepCopy[]>();
  for (const effect of out.depEffects) {
    copies.set(
      effect,
      effect.deps.map(dep => {
        const type = depType(dep);
        return {
          type,
          slot:
            type === 'string'
              ? arena.slot(ValueType.string, undefined, listStrCap - 1)
              : arena.slot(ValueType[type]),
        };
      }),
    );
  }

  const locals = new Map<string, number>();
  for (const body of bodies(app)) {
    stringLocals(body, locals);
  }
  for (const [name, cap] of locals) {
    if (!slots.hoisted.has(name)) {
      slots.stringLocals.set(
        name,
        arena.slot(ValueType.string, undefined, cap),
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
  const reached = (node: IrNode): boolean =>
    isStateDriven(node) || isSetOnce(node) || readsInput(node);

  // A row's node keeps its handle in an array by the row, one array for each place in the row that needs one.
  const pools: Pool[] = [];
  poolsOf(app.root, pools);
  const rowHandles = new Map<IrNode, RowHandle>();
  for (const pool of pools) {
    const arrays: Slot[] = [];
    pool.rows.forEach((rowElement, row) =>
      nodesOf(rowElement).forEach((node, place) => {
        if (readsInput(node)) {
          notYetAt('onChangeText in a list row', node.loc);
        }
        if (reached(node)) {
          arrays[place] ??= arena.slot(ValueType.node, 0, pool.list.cap);
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
    if (!rowHandles.has(node) && (reached(node) || builtUnder.has(node))) {
      handles.set(node, arena.slot(ValueType.node));
    }
  }
  const inputText = nodes.some(readsInput)
    ? arena.slot(ValueType.string, undefined, ENGINE_STRING_MAX.ER_TEXT_MAX)
    : null;

  const requestsUpdate =
    out.handlers.some(
      handler => handler.kind === 'callback' && handler.callback.updatesState,
    ) ||
    out.timerFns.some(timer => timer.callback!.updatesState) ||
    out.mountEffects.some(mountUpdates);
  const requested = requestsUpdate
    ? arena.track(arena.slot(ValueType.bool))
    : null;
  const hasEffects = out.depEffects.length > 0 || out.mountEffects.length > 0;
  const once =
    hasEffects ||
    pools.length ||
    nodes.some(node => isStateDriven(node) || isSetOnce(node))
      ? arena.track(arena.slot(ValueType.bool))
      : null;

  // A state an effect sets that a node reads waits for the next app_update() in a pending flag.
  const effectWrites = new Set<StateRecord>();
  for (const effect of out.depEffects) {
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
  const mountNext = out.mountEffects.length
    ? arena.slot(ValueType.int, 1)
    : null;

  if (out.timerFns.length) {
    arena.timerTable(caps.maxTimers);
  }
  // A pass queues each dep-driven effect and the next mount effect, and a mount effect the one after it.
  if (hasEffects) {
    arena.eventQueue(out.depEffects.length + 1);
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
 * A node's constant props as a prop set, in the order the C output sets them: its style, then a placeholder,
 * text and image known at compile time.
 *
 * @param node  The node.
 * @param lowering  The program being lowered.
 *
 * @returns The set's entries.
 */
function constantProps(node: IrNode, lowering: Lowering): ProgramPropEntry[] {
  const {tables} = lowering.slots;
  const entries = node.constantProps.map(prop =>
    constantPropEntry(prop.field, prop.expr),
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
 * input, since it reads its own. Each row has its own handler, as C has; rows whose handlers have the same
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
  const handler = lowering.app.out.handlers.find(
    candidate => candidate.kind !== 'panEvent' && candidate.name === name,
  );
  if (!handler || handler.kind === 'panEvent') {
    throw new Error(`AOT internal: no handler "${name}"`);
  }
  if (handler.kind === 'pressFade') {
    return notYet('the press fade of a <TouchableOpacity>');
  }
  const readsInput = eventName === 'ER_EVENT_CHANGE_TEXT';
  const made: Omit<HandlerFunction, 'index'> = {
    callback: handler.callback,
    event: ENGINE_VALUES.get(eventName)!,
    name,
    input: readsInput
      ? {node: lowering.handles.get(node)!, text: lowering.inputText!}
      : undefined,
    inRow,
  };
  const key = inRow
    ? `row ${JSON.stringify(handlerBody({...made, index: NONE}, lowering))}`
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
 * An element and everything under it as template nodes, each before its children. A conditional's branches
 * are all built, and the update shows the chosen one. A list's rows are row templates of their own. What a node
 * needs that the package output can't lower yet is refused at the node's element.
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
      if (wire.kind === 'event') {
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
      } else if (wire.kind !== 'ref') {
        // A ref needs nothing itself; the code that reads one is refused on its own.
        notYet(WIRING_USES[wire.kind]);
      }
    }

    children = node.children.flatMap(child =>
      child.kind === 'element'
        ? [child.element]
        : child.kind === 'conditional'
          ? branchElements(child)
          : [],
    );

    // A node the update sets starts with no props, as er_app_build() leaves it.
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
    locateError(error, node.loc);
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
 * A C string literal's text, as the C output spells a span's.
 *
 * @param code  The literal.
 *
 * @returns The text.
 */
function readCString(code: string): string {
  const match = /^"((?:[^"\\]|\\.)*)"$/s.exec(code.trim());
  if (!match) {
    return notYet(`the span text \`${code}\``);
  }
  return match[1].replace(/\\(.)/g, (_, escaped: string) =>
    escaped === 'n' ? '\n' : escaped === 't' ? '\t' : escaped,
  );
}

/**
 * Sets a node's props again: its constant set, what state drives, its image and its text, then spans when it
 * has them and the update sets it once.
 *
 * @param codegen  The update function's codegen.
 * @param node  The node.
 * @param lowering  The program being lowered.
 */
function setNode(codegen: Codegen, node: IrNode, lowering: Lowering): void {
  const {fn} = codegen;
  const {tables} = lowering.slots;
  const spans = isSetOnce(node) ? node.spans : null;
  if (spans && spans.length > MAX_SPANS) {
    notYet(`more than ${MAX_SPANS} nested <Text> spans`);
  }

  // Span styles are props too; each value is as the C output spells it.
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
  fn.abx('PROPS', 0, tables.propSet(entries));

  for (const {field, value} of node.stateProps) {
    codegen.setProp(field, value);
  }
  if (node.image?.kind === 'value') {
    const image = node.image.value;
    codegen.scoped(() =>
      fn.abx(
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
      fn.abc('FMTTEXT');
    });
  }
  spans?.forEach((span, index) => {
    fn.abx('FMTK', 0, tables.string(readCString(span.text)));
    fn.abc('FMTSPAN', index);
  });
  if (spans) {
    fn.abc('SPANS', spans.length);
  }

  codegen.scoped(() =>
    fn.abc('COMMIT', nodeHandle(codegen, node, lowering).index),
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
  const {fn} = codegen;
  const handle = fn.register('node');
  const inRow = lowering.rowHandles.get(node);
  if (inRow) {
    fn.abc(
      'GETNI',
      handle.index,
      codegen.load('int', inRow.row).index,
      0,
      inRow.array.index,
    );
  } else {
    fn.abx('GET64', handle.index, lowering.handles.get(node)!.index);
  }
  return handle;
}

/**
 * Builds every list row, in order, under its parent and before the node after it, as er_app_build() builds them.
 *
 * @param codegen  The update function's codegen.
 * @param lowering  The program being lowered.
 */
function buildRows(codegen: Codegen, lowering: Lowering): void {
  const {fn} = codegen;
  for (const pool of lowering.pools) {
    codegen.scoped(() => {
      const parent = nodeHandle(codegen, pool.parent, lowering);
      // BUILD before the parent itself puts the row last.
      const before = pool.anchor
        ? nodeHandle(codegen, pool.anchor, lowering)
        : parent;
      pool.templates.forEach((template, row) =>
        codegen.scoped(() =>
          fn.abc(
            'BUILD',
            fn.register('node').index,
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
 * @param fn  The update function.
 * @param slots  The slots whose bits guard it.
 * @param body  Emits what it guards.
 * @param pending  The pending flags that guard it too.
 */
function whenDirty(
  fn: FunctionBuilder,
  slots: Slot[],
  body: () => void,
  pending: Slot[] = [],
): void {
  const run = fn.label();
  const skip = fn.label();
  for (const [word, mask] of dirtyMasks(slots)) {
    fn.branchIfDirty(word, mask, run);
  }
  for (const flag of pending) {
    const checkpoint = fn.checkpoint();
    const set = fn.register('bool');
    fn.abx('GET32', set.index, flag.index);
    fn.branch('JT', run, set);
    fn.release(checkpoint);
  }
  fn.branch('JMP', skip);
  fn.place(run);
  body();
  fn.place(skip);
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
    codegen.fn.abx(
      'SET32',
      codegen.load(slot.type === ValueType.bool ? 'bool' : 'int', value).index,
      slot.index,
    ),
  );
}

/**
 * The update function: what app_update() sets again, each node when a state it reads changed, and the nodes set
 * once in the first pass; then the effects it runs, queued.
 *
 * @param lowering  The program being lowered.
 *
 * @returns The function, or null when nothing needs it.
 */
function updateFunction(lowering: Lowering): ProgramFunction | null {
  const {out} = lowering.app;
  const nodes = nodesOf(lowering.app.root);
  const updated = nodes.filter(isStateDriven);
  const setOnce = nodes.filter(isSetOnce);
  if (
    !updated.length &&
    !setOnce.length &&
    !out.depEffects.length &&
    !out.mountEffects.length
  ) {
    return null;
  }

  const fn = new FunctionBuilder(FunctionKind.update, 'the update');
  const codegen = new Codegen(fn, lowering.slots);
  const {pending, realBits, mountNext} = lowering.effects;
  const located = (node: IrNode, lower: () => void): void => {
    try {
      lower();
    } catch (error) {
      locateError(error, node.loc);
      throw error;
    }
  };

  // A pass only an effect's writes started is no app_update(): it keeps what they changed for the next one.
  const end = fn.label();
  if (pending.size) {
    const real = fn.label();
    for (const [word, mask] of dirtyMasks(realBits)) {
      fn.branchIfDirty(word, mask, real);
    }
    for (const [state, flag] of pending) {
      whenDirty(fn, [state], () => setConstant(codegen, flag, 1));
    }
    fn.branch('JMP', end);
    fn.place(real);
  }

  if (setOnce.length || lowering.pools.length) {
    whenDirty(fn, [lowering.once!], () => {
      buildRows(codegen, lowering);
      setOnce.forEach(node =>
        located(node, () => setNode(codegen, node, lowering)),
      );
    });
  }

  for (const node of updated) {
    located(node, () => {
      // A node that reads something no state write flags (a ref, the clock) is set on every pass.
      const reads = nodeReads(node);
      const bits = reads
        ? [...reads].map(state => stateSlotOf(state, lowering.slots))
        : [];
      if (bits.length && bits.every(slot => slot.flags & SlotFlag.tracked)) {
        whenDirty(
          fn,
          bits,
          () => setNode(codegen, node, lowering),
          bits.flatMap(slot => pending.get(slot) ?? []),
        );
      } else {
        setNode(codegen, node, lowering);
      }
    });
  }
  for (const flag of pending.values()) {
    setConstant(codegen, flag, 0);
  }

  // Each dep-driven effect checks its deps when it runs, as app_update() checks each in turn.
  for (const check of lowering.effectFunctions.checks) {
    fn.abx('EFFECT', 0, check);
  }

  // The first mount effect, then each one after a mount effect that set state.
  if (mountNext) {
    const {mounts} = lowering.effectFunctions;
    const queued = out.mountEffects.flatMap((_, index) =>
      index === 0 || mountUpdates(out.mountEffects[index - 1]) ? [index] : [],
    );
    for (const index of queued) {
      codegen.scoped(() => {
        const skip = fn.label();
        const next = fn.register('int');
        const isNext = fn.register('bool');
        fn.abx('GET32', next.index, mountNext.index);
        fn.abc(
          'EQ',
          isNext.index,
          next.index,
          codegen.load('int', index + 1).index,
        );
        fn.branch('JF', skip, isNext);
        fn.abx('EFFECT', 0, mounts[index]);
        fn.place(skip);
      });
    }
    setConstant(codegen, mountNext, 0);
  }
  fn.place(end);
  return fn.finish();
}

/**
 * Toggles the request bit, so the update runs after a function that set state, even when no value changed, as
 * the C output calls app_update().
 *
 * @param codegen  The function's codegen.
 * @param lowering  The program being lowered.
 */
function requestUpdate(codegen: Codegen, lowering: Lowering): void {
  const {requested} = lowering;
  if (!requested) {
    return;
  }
  const {fn} = codegen;
  codegen.scoped(() => {
    const flag = fn.register('bool');
    fn.abx('GET32', flag.index, requested.index);
    fn.abc('NOT', flag.index, flag.index);
    fn.abx('SET32', flag.index, requested.index);
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
  const fn = new FunctionBuilder(handler.event, `handler ${handler.name}`);
  // A row's handler is entered with its row, in its first register.
  const row = handler.inRow ? fn.reserved('int') : undefined;
  const codegen = new Codegen(fn, lowering.slots, {input: handler.input, row});
  codegen.statements(handler.callback.body);
  if (handler.callback.updatesState) {
    requestUpdate(codegen, lowering);
  }
  return fn.finish(row ? 1 : 0);
}

/**
 * A setTimeout or setInterval callback's function.
 *
 * @param name  The callback's C name.
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
  const fn = new FunctionBuilder(FunctionKind.timer, `timer ${name}`);
  const codegen = new Codegen(fn, lowering.slots);
  codegen.statements(callback.body);
  if (callback.updatesState) {
    requestUpdate(codegen, lowering);
  }
  return fn.finish();
}

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
  const {fn} = codegen;
  if (effect.hasCleanup) {
    const armed = lowering.slots.armed.get(armedFlag(effect.id))!;
    const skip = fn.label();
    codegen.scoped(() => {
      const flag = fn.register('bool');
      fn.abx('GET32', flag.index, armed.index);
      fn.branch('JF', skip, flag);
    });
    setConstant(codegen, armed, 0);
    codegen.statements(effect.cleanup ?? []);
    fn.place(skip);
  }
  codegen.statements(effect.body);
}

/**
 * The function a dep-driven effect runs in after an update pass: it copies each dep that changed, as
 * app_update() does, and runs the effect if one did.
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
  const fn = new FunctionBuilder(FunctionKind.effect, `effect ${effect.id}`);
  const end = fn.label();
  const codegen = new Codegen(fn, lowering.slots, {onReturn: end});
  const copies = lowering.effects.copies.get(effect)!;
  const changed = fn.register('bool');
  fn.abx('LOADI', changed.index, 0);
  effect.deps.forEach((dep, index) => {
    const {slot, type} = copies[index];
    codegen.scoped(() => {
      const value = codegen.valueAs(dep, type);
      const same = fn.label();
      fn.branch('JT', same, codegen.same(value, codegen.get(slot)));
      codegen.set(slot, value);
      fn.abx('LOADI', changed.index, 1);
      fn.place(same);
    });
  });
  fn.branch('JF', end, changed);
  runEffect(effect, codegen, lowering);
  fn.place(end);
  return fn.finish();
}

/**
 * The function a mount effect runs in, as er_app_build() runs it after the first app_update(). One that set
 * state has the next pass queue the mount effect after it; any other queues it itself.
 *
 * @param index  The mount effect's place in the order.
 * @param lowering  The program being lowered.
 *
 * @returns The function.
 */
function mountEffect(index: number, lowering: Lowering): ProgramFunction {
  const {out} = lowering.app;
  const mount = out.mountEffects[index];
  const fn = new FunctionBuilder(FunctionKind.effect, `mount effect ${index}`);
  const end = fn.label();
  const codegen = new Codegen(fn, lowering.slots, {onReturn: end});
  switch (mount.kind) {
    case 'inline':
      codegen.statements(mount.callback.body);
      break;
    case 'call': {
      const called = out.effectFns.find(
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
  fn.place(end);

  // A dep-driven effect keeps a copy of each dep, taken after it ran.
  if (mount.kind === 'depEffect') {
    const copies = lowering.effects.copies.get(mount.effect)!;
    mount.effect.deps.forEach((dep, depIndex) =>
      codegen.scoped(() =>
        codegen.set(
          copies[depIndex].slot,
          codegen.valueAs(dep, copies[depIndex].type),
        ),
      ),
    );
  }

  const hasNext = index + 1 < out.mountEffects.length;
  if (mountUpdates(mount)) {
    requestUpdate(codegen, lowering);
    if (hasNext) {
      setConstant(codegen, lowering.effects.mountNext!, index + 2);
    }
  } else if (hasNext) {
    fn.abx('EFFECT', 0, lowering.effectFunctions.mounts[index + 1]);
  }
  return fn.finish();
}

/**
 * Appends a list's first items, as C's initializer fills its array: a missing field is 0 or "", and a whole
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
  const {fn} = codegen;
  const {fields} = list.struct;
  for (const item of list.items) {
    codegen.scoped(() => {
      const row = fn.consecutive(
        fields.map(field => (field.kind === 'string' ? 'string' : field.kind)),
      );
      fields.forEach((field, index) => {
        const value = item[field.key];
        if (field.kind === 'string') {
          fn.abx(
            'LOADS',
            row[index].index,
            lowering.slots.tables.string(String(value ?? '')),
          );
          return;
        }
        const number = Number(value) || 0;
        codegen.scoped(() =>
          fn.abc(
            'MOVE',
            row[index].index,
            codegen.load(
              field.kind as 'int' | 'float',
              field.kind === 'float' ? number : Math.round(number),
            ).index,
          ),
        );
      });
      fn.abx(
        'APPEND',
        row[0].index,
        lowering.slots.lists.get(list)!.slot.index,
      );
    });
  }
}

/**
 * The boot function: the values worked out from the panel size, in double precision as C works them out, and
 * each list's first items.
 *
 * @param bootSlots  The values.
 * @param lowering  The program being lowered.
 * @param screen  The panel.
 *
 * @returns The function, or null when the app has none.
 */
function bootFunction(
  bootSlots: IrBootSlot[],
  lowering: Lowering,
  screen: {width: number; height: number},
): ProgramFunction | null {
  const lists = lowering.app.state.filter(
    (state): state is ListState =>
      state.kind === 'list' && state.items.length > 0,
  );
  if (!bootSlots.length && !lists.length) {
    return null;
  }
  const fn = new FunctionBuilder(FunctionKind.boot, 'the boot values');
  const codegen = new Codegen(fn, lowering.slots, {inDoubles: true});
  for (const list of lists) {
    appendInitialItems(list, codegen, lowering);
  }
  for (const bootSlot of bootSlots) {
    const slot = lowering.slots.boot.get(bootSlot)!;
    const {init} = bootSlot;
    codegen.scoped(() =>
      codegen.set(
        slot,
        init.kind === 'screen'
          ? codegen.load('int', screen[init.dimension])
          : codegen.value(init.expr),
      ),
    );
  }
  return fn.finish();
}

/*----------------------------------------------------------------------------------------------------------------------
 - Program
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Lowers an app to a program for one panel.
 *
 * @param app  The lowered app.
 * @param opts  The compile's options: the panel, and the string cap.
 *
 * @returns The program.
 */
export function lowerPackage(app: IrApp, opts: ResolvedOptions): Program {
  const {screen} = opts;
  const tables = new ProgramTables();
  const lowering: Lowering = {
    app,
    ...planSlots(app, tables, opts.caps),
    images: new Set(app.images.map(image => image.name)),
    handlers: new Map(),
    bindings: [],
    templates: [],
    effectFunctions: {checks: [], mounts: []},
  };
  const {out} = app;

  // A row's handlers are compared by their code before the functions are numbered, so each timer they start
  // stands in by its own place until then.
  out.timerFns.forEach((timer, index) =>
    lowering.slots.timerFunctions.set(timer.name, index),
  );

  // A panel-sized root the app's tree fills, as er_app_build() makes one.
  const nodes: ProgramTemplateNode[] = [
    {
      type: ENGINE_VALUES.get('ER_NODE_VIEW')!,
      children: 1,
      props: tables.propSet([
        constantPropEntry('width', String(screen.width)),
        constantPropEntry('height', String(screen.height)),
      ]),
      handle: NONE,
      bindings: 0,
      bindingCount: 0,
    },
  ];
  templateNodes(app.root, lowering, nodes);
  rowTemplates(lowering);
  checkSupported(app);

  // Handlers first, as the bindings number them, then the timer callbacks, the effects, and the boot and
  // update functions. Each is numbered before any code is written, since code names them.
  let next = lowering.handlers.size;
  for (const timer of out.timerFns) {
    lowering.slots.timerFunctions.set(timer.name, next++);
  }
  lowering.effectFunctions = {
    checks: out.depEffects.map(() => next++),
    mounts: out.mountEffects.map(() => next++),
  };
  const functions = [
    ...[...lowering.handlers.values()].map(handler =>
      handlerBody(handler, lowering),
    ),
    ...out.timerFns.map(timer =>
      timerBody(timer.name, timer.callback!, lowering),
    ),
    ...out.depEffects.map(effect => checkedEffect(effect, lowering)),
    ...out.mountEffects.map((_, index) => mountEffect(index, lowering)),
  ];
  const boot = bootFunction(app.boot.slots, lowering, screen);
  const update = updateFunction(lowering);
  const bootIndex = boot ? functions.push(boot) - 1 : NONE;
  const updateIndex = update ? functions.push(update) - 1 : NONE;

  // The register stack holds the largest frame: no function calls another.
  const stackCells = Math.max(
    0,
    ...functions.map(fn => FRAME_CELLS + fn.types.length),
  );
  const {regions, bytes} = lowering.slots.arena.regions(stackCells);

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
        arenaBytes: bytes,
        // Every row is built, whether the list holds it or not.
        nodeCount:
          nodes.length +
          lowering.pools.reduce(
            (count, pool) =>
              count +
              pool.templates.reduce(
                (rowNodes, template) =>
                  rowNodes + lowering.templates[template - 1].nodes.length,
                0,
              ),
            0,
          ),
        rootTemplate: 0,
        bootFunction: bootIndex,
        updateFunction: updateIndex,
      },
    ],
    needs: app.images.length
      ? [{resource: Resource.images, amount: app.images.length}]
      : [],
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
    inputs,
  };
}
