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
 * slots in the arena. Handlers, the boot function and the update function are bytecode (codegen.mts). The
 * update re-sets a node when a state it reads changed, as app_update() does, and sets the props of a node that
 * reads only boot values once, in its first pass. What isn't lowered yet (timers, effects, lists, animations,
 * responders, drawings, the keyboard) is refused with what it is.
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
import type {InputText, ProgramSlots} from './codegen.mts';
import type {SourcePosition} from '../diagnostics.mts';
import type {IrApp} from '../ir/app.mts';
import type {IrBootSlot} from '../ir/boot.mts';
import type {IrExpr} from '../ir/expressions.mts';
import type {IrCallback, IrStmt} from '../ir/statements.mts';
import type {IrElement, IrNode, IrWire} from '../ir/nodes.mts';
import type {ResolvedOptions} from '../options.mts';
import type {TextSpan} from '../style-text.mts';
import type {ScalarState} from '../types.mts';
import type {
  Program,
  ProgramBinding,
  ProgramFunction,
  ProgramPropEntry,
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
}

/** Everything lowering one program shares. */
interface Lowering {
  app: IrApp;
  slots: ProgramSlots;
  images: ReadonlySet<string>;
  /** Each node handle code reaches, by node. */
  handles: Map<IrNode, Slot>;
  /** The buffer a <TextInput>'s onChangeText reads its text into. */
  inputText: Slot | null;
  /** Handlers by name and event, in function order. */
  handlers: Map<string, HandlerFunction>;
  bindings: ProgramBinding[];
  /** Toggled by a handler that set state, so the update runs as app_update() would. */
  requested: Slot | null;
  /** Set only at load, for the props the update sets once. */
  once: Slot | null;
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
    [out.timerFns.length > 0, 'timers', ['setTimeout', 'setInterval']],
    [
      out.effectFns.length > 0 || out.mountEffects.length > 0,
      'effects',
      ['useEffect', 'useLayoutEffect'],
    ],
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
 * Plans every slot the program uses, then lays the arena out.
 *
 * @param app  The app.
 * @param tables  The program's tables.
 * @param listStrCap  The C output's string buffer size, NUL included.
 *
 * @returns The slots, the node handles, and the two bookkeeping bits.
 */
function planSlots(
  app: IrApp,
  tables: ProgramTables,
  listStrCap: number,
): Pick<Lowering, 'slots' | 'handles' | 'inputText' | 'requested' | 'once'> {
  const arena = new Arena();
  const slots: ProgramSlots = {
    arena,
    tables,
    states: new Map(),
    refs: new Map(),
    boot: new Map(),
    stringLocals: new Map(),
    enums: new Map(),
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
    if (state.isWatched) {
      arena.track(slot);
    }
    slots.states.set(state, slot);
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

  const locals = new Map<string, number>();
  for (const handler of app.out.handlers) {
    if (handler.kind === 'callback') {
      stringLocals(handler.callback.body, locals);
    }
  }
  for (const [name, cap] of locals) {
    slots.stringLocals.set(name, arena.slot(ValueType.string, undefined, cap));
  }

  const nodes = nodesOf(app.root);
  // A <TextInput>'s onChangeText reads the input's text, through its handle.
  const handles = new Map<IrNode, Slot>();
  const readsInput = (node: IrNode): boolean =>
    node.wiring.some(
      wire => wire.kind === 'event' && wire.event === 'ER_EVENT_CHANGE_TEXT',
    );
  for (const node of nodes) {
    if (isStateDriven(node) || isSetOnce(node) || readsInput(node)) {
      handles.set(node, arena.slot(ValueType.node));
    }
  }
  const inputText = nodes.some(readsInput)
    ? arena.slot(ValueType.string, undefined, ENGINE_STRING_MAX.ER_TEXT_MAX)
    : null;

  const handlerSetsState = app.out.handlers.some(
    handler => handler.kind === 'callback' && handler.callback.updatesState,
  );
  const requested = handlerSetsState
    ? arena.track(arena.slot(ValueType.bool))
    : null;
  const once = nodes.some(isSetOnce)
    ? arena.track(arena.slot(ValueType.bool))
    : null;

  arena.layOut();
  return {slots, handles, inputText, requested, once};
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
 * input, since it reads its own.
 *
 * @param name  The handler.
 * @param eventName  The event, an ER_EVENT_* name.
 * @param node  The node it is bound to.
 * @param lowering  The program being lowered.
 *
 * @returns The function's index.
 */
function handlerFunction(
  name: string,
  eventName: string,
  node: IrNode,
  lowering: Lowering,
): number {
  const readsInput = eventName === 'ER_EVENT_CHANGE_TEXT';
  const key = `${name} ${eventName}${readsInput ? ` ${node.id}` : ''}`;
  const existing = lowering.handlers.get(key);
  if (existing) {
    return existing.index;
  }
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
  const index = lowering.handlers.size;
  lowering.handlers.set(key, {
    index,
    callback: handler.callback,
    event: ENGINE_VALUES.get(eventName)!,
    name,
    input: readsInput
      ? {node: lowering.handles.get(node)!, text: lowering.inputText!}
      : undefined,
  });
  return index;
}

/**
 * An element and everything under it as template nodes, each before its children. A conditional's branches
 * are all built, and the update shows the chosen one. What a node needs that the package output can't lower
 * yet is refused at the node's element.
 *
 * @param element  The element.
 * @param lowering  The program being lowered.
 * @param nodes  Collects the template nodes.
 */
function templateNodes(
  element: IrElement,
  lowering: Lowering,
  nodes: ProgramTemplateNode[],
): void {
  if (element.kind === 'component') {
    templateNodes(element.root, lowering, nodes);
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
          target: handlerFunction(wire.handler, wire.event, node, lowering),
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
          : notYet('lists of state'),
    );

    // A node the update sets starts with no props, as er_app_build() leaves it.
    const handle = lowering.handles.get(node);
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
    templateNodes(child, lowering, nodes);
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

  codegen.scoped(() => {
    const handle = fn.register('node');
    fn.abx('GET64', handle.index, lowering.handles.get(node)!.index);
    fn.abc('COMMIT', handle.index);
  });
}

/**
 * Runs `body` only when one of a set of dirty bits is set in this update pass.
 *
 * @param fn  The update function.
 * @param slots  The slots whose bits guard it.
 * @param body  Emits what it guards.
 */
function whenDirty(fn: FunctionBuilder, slots: Slot[], body: () => void): void {
  const words = new Map<number, number>();
  for (const slot of slots) {
    const word = slot.dirtyBit >> 5;
    words.set(
      word,
      ((words.get(word) ?? 0) | (1 << (slot.dirtyBit & 31))) >>> 0,
    );
  }
  const run = fn.label();
  const skip = fn.label();
  for (const [word, mask] of words) {
    fn.branchIfDirty(word, mask, run);
  }
  fn.branch('JMP', skip);
  fn.place(run);
  body();
  fn.place(skip);
}

/**
 * The update function: what app_update() sets again, each node when a state it reads changed, and the nodes set
 * once in the first pass.
 *
 * @param lowering  The program being lowered.
 *
 * @returns The function, or null when nothing needs it.
 */
function updateFunction(lowering: Lowering): ProgramFunction | null {
  const nodes = nodesOf(lowering.app.root);
  const updated = nodes.filter(isStateDriven);
  const setOnce = nodes.filter(isSetOnce);
  if (!updated.length && !setOnce.length) {
    return null;
  }

  const fn = new FunctionBuilder(FunctionKind.update, 'the update');
  const codegen = new Codegen(fn, lowering.slots);
  const located = (node: IrNode, lower: () => void): void => {
    try {
      lower();
    } catch (error) {
      locateError(error, node.loc);
      throw error;
    }
  };

  if (setOnce.length) {
    whenDirty(fn, [lowering.once!], () =>
      setOnce.forEach(node =>
        located(node, () => setNode(codegen, node, lowering)),
      ),
    );
  }

  for (const node of updated) {
    located(node, () => {
      // A node that reads something no state write flags (a ref, the clock) is set on every pass.
      const reads = nodeReads(node);
      const bits = reads
        ? [...reads].map(state =>
            state.kind === 'list'
              ? notYet('lists of state')
              : lowering.slots.states.get(state)!,
          )
        : [];
      if (bits.length && bits.every(slot => slot.flags & SlotFlag.tracked)) {
        whenDirty(fn, bits, () => setNode(codegen, node, lowering));
      } else {
        setNode(codegen, node, lowering);
      }
    });
  }
  return fn.finish();
}

/**
 * A handler's function. One that set state toggles the request bit, so the update runs even when no value
 * changed, as the C output calls app_update().
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
  const codegen = new Codegen(fn, lowering.slots, {input: handler.input});
  codegen.statements(handler.callback.body);
  if (handler.callback.updatesState && lowering.requested) {
    const {requested} = lowering;
    codegen.scoped(() => {
      const flag = fn.register('bool');
      fn.abx('GET32', flag.index, requested.index);
      fn.abc('NOT', flag.index, flag.index);
      fn.abx('SET32', flag.index, requested.index);
    });
  }
  return fn.finish();
}

/**
 * The boot function: the values worked out from the panel size, in double precision as C works them out.
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
  if (!bootSlots.length) {
    return null;
  }
  const fn = new FunctionBuilder(FunctionKind.boot, 'the boot values');
  const codegen = new Codegen(fn, lowering.slots, {inDoubles: true});
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
    ...planSlots(app, tables, opts.caps.listStrCap),
    images: new Set(app.images.map(image => image.name)),
    handlers: new Map(),
    bindings: [],
  };

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
  checkSupported(app);

  // Handlers first, as the bindings number them, then the boot and update functions.
  const functions = [...lowering.handlers.values()].map(handler =>
    handlerBody(handler, lowering),
  );
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
        nodeCount: nodes.length,
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
    propSets: tables.propSets,
    templates: [{kind: TemplateKind.root, nodes}],
    bindings: lowering.bindings,
    functions,
    inputs,
  };
}
