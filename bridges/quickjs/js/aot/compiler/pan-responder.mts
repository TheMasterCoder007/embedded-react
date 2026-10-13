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

import {aotError} from './diagnostics.mts';
import {lowerExpr} from './expressions.mts';
import {isFn} from './collect.mts';
import {lowerHandler} from './handlers.mts';
import type * as t from '@babel/types';
import type {Out} from './out.mts';
import type {GestureField, IrExpr, IrGesture} from './ir/expressions.mts';
import type {
  IrPanEvent,
  IrPanResponder,
  IrPanRole,
} from './ir/pan-responder.mts';
import type {
  Env,
  PanCallback,
  PanResponderRecord,
  StateTable,
} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** RN PanResponder config key → the engine responder QUERY it lowers to. */
const PAN_QUERIES: Record<string, string> = {
  onStartShouldSetPanResponder: 'ER_QUERY_START_SHOULD_SET',
  onStartShouldSetPanResponderCapture: 'ER_QUERY_START_SHOULD_SET_CAPTURE',
  onMoveShouldSetPanResponder: 'ER_QUERY_MOVE_SHOULD_SET',
  onMoveShouldSetPanResponderCapture: 'ER_QUERY_MOVE_SHOULD_SET_CAPTURE',
  onPanResponderTerminationRequest: 'ER_QUERY_TERMINATION_REQUEST',
};

/** RN PanResponder config key → the engine responder EVENT it lowers to. */
const PAN_EVENTS: Record<string, string> = {
  onPanResponderGrant: 'ER_EVENT_RESPONDER_GRANT',
  onPanResponderMove: 'ER_EVENT_RESPONDER_MOVE',
  onPanResponderRelease: 'ER_EVENT_RESPONDER_RELEASE',
  onPanResponderTerminate: 'ER_EVENT_RESPONDER_TERMINATE',
  onPanResponderReject: 'ER_EVENT_RESPONDER_REJECT',
};

/** The fields a gestureState carries. */
const GESTURE_FIELDS = new Set([
  'dx',
  'dy',
  'moveX',
  'moveY',
  'x0',
  'y0',
  'vx',
  'vy',
  'numberActiveTouches',
  'stateID',
]);

/** Config keys the Flow A module acts on that have no Flow B lowering — rejected by name, not ignored. */
const PAN_FLOW_A_ONLY: Record<string, string> = {
  onPanResponderStart:
    'it reports EXTRA fingers joining a gesture already in flight; Flow B lowers one gesture per responder.',
  onPanResponderEnd:
    'it reports a finger lifting while others stay down; Flow B lowers one gesture per responder.',
};

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * True for a `PanResponder.create({…})` call.
 *
 * @param node  Any node, or nothing.
 *
 * @returns Whether it is such a call.
 */
export const isPanCreate = (
  node: t.Node | null | undefined,
): node is t.CallExpression =>
  node?.type === 'CallExpression' &&
  node.callee.type === 'MemberExpression' &&
  !node.callee.computed &&
  node.callee.object.type === 'Identifier' &&
  node.callee.object.name === 'PanResponder' &&
  (node.callee.property as t.Identifier).name === 'create';

/**
 * Lowers one `gestureState` field of a responder's callback.
 *
 * @param field  The field's name.
 * @param pan  The responder whose callback reads it.
 *
 * @returns The field.
 */
export function lowerGestureField(
  field: string,
  pan: PanResponderRecord | null | undefined,
): IrGesture {
  if (GESTURE_FIELDS.has(field)) {
    return {
      kind: 'gesture',
      field: field as GestureField,
      statePrefix: pan!.cPrefix,
      stateID: pan!.stateID,
      cType: field === 'vx' || field === 'vy' ? 'float' : 'int',
    };
  }

  throw aotError(
    `AOT: unknown gestureState field "${field}" in a PanResponder callback`,
    'a gestureState carries dx / dy / moveX / moveY / x0 / y0 / vx / vy / numberActiveTouches / stateID.',
  );
}

/**
 * Validates one `PanResponder.create({…})` config and builds the descriptor the emitter works from.
 *
 * @param name  The JS name the responder is bound to.
 * @param createCall  The `PanResponder.create(…)` call.
 * @param prefix  The C name prefix: none for the App, `c<n>_` for an inlined instance.
 *
 * @returns The responder.
 */
function buildPanDescriptor(
  name: string,
  createCall: t.CallExpression,
  prefix: string,
): PanResponderRecord {
  // The config must be an object literal, so every callback it holds is known at compile time.
  const config = createCall.arguments[0];
  if (!config || config.type !== 'ObjectExpression') {
    throw aotError(
      'AOT: PanResponder.create(...) needs an object literal of callbacks',
      'write the config inline — `PanResponder.create({ onStartShouldSetPanResponder: () => ' +
        'true, … })` — so the AOT can see which callbacks exist at compile time.',
    );
  }

  // File each callback under the engine query or event its key lowers to, rejecting keys Flow B cannot lower.
  const queries = new Map<string, PanCallback>();
  const events = new Map<string, PanCallback>();
  for (const prop of config.properties) {
    if (prop.type !== 'ObjectProperty' && prop.type !== 'ObjectMethod') {
      throw aotError(
        'AOT: a spread inside PanResponder.create({…}) is not supported',
        'list the callbacks explicitly.',
      );
    }

    if (prop.computed) {
      throw aotError(
        'AOT: a computed key in PanResponder.create({…}) is not supported',
        'name each callback literally — the AOT decides at compile time which engine query or event a key becomes.',
      );
    }

    // A shorthand method (`onPanResponderMove(e, g) { … }`) IS the function node; a property holds it.
    const key =
      (prop.key as t.Identifier).name ?? (prop.key as t.StringLiteral).value;
    const callback = prop.type === 'ObjectMethod' ? prop : prop.value;
    if (PAN_FLOW_A_ONLY[key]) {
      throw aotError(
        `AOT: PanResponder "${key}" is not supported in Flow B`,
        `${PAN_FLOW_A_ONLY[key]} Use onPanResponderGrant / Release / Terminate instead, or ` +
          `build this screen for Flow A.`,
      );
    }

    const query = PAN_QUERIES[key];
    const event = PAN_EVENTS[key];
    if (!query && !event) {
      throw aotError(
        `AOT: unknown PanResponder config key "${key}"`,
        `supported: ${[...Object.keys(PAN_QUERIES), ...Object.keys(PAN_EVENTS)].join(', ')}.`,
      );
    }

    if (prop.type !== 'ObjectMethod' && !isFn(callback)) {
      throw aotError(
        `AOT: PanResponder "${key}" must be a function`,
        'pass an inline arrow — `(e, g) => …` — so it can be compiled into the generated C.',
      );
    }

    if (query) {
      queries.set(query, callback as PanCallback);
    } else {
      events.set(event, callback as PanCallback);
    }
  }

  return {
    name,
    cPrefix: `s_pan_${prefix}${name}`,
    fnPrefix: `er_pan_${prefix}${name}`,
    stateID: 0, // assigned when the responder is first wired to a node
    queries,
    events,
    lowered: null,
  };
}

/**
 * Collects `const pan = useRef(PanResponder.create({…})).current` → Map (name → descriptor). The
 * `useRef(…)` form (read back as `pan.current.panHandlers`) is accepted too; a bare `create(…)` is not,
 * because it would re-create the recognizer every render in Flow A and the two flows must agree.
 *
 * @param fnBody  The component's body.
 * @param prefix  The C name prefix: none for the App, `c<n>_` for an inlined instance.
 *
 * @returns The responders, by name.
 */
export function collectPanResponders(
  fnBody: t.BlockStatement | t.Expression,
  prefix = '',
): Map<string, PanResponderRecord> {
  // Only the component body's own top-level declarations can hold a responder.
  const pans = new Map<string, PanResponderRecord>();
  if (fnBody.type !== 'BlockStatement') return pans;

  for (const statement of fnBody.body) {
    if (statement.type !== 'VariableDeclaration') continue;

    for (const decl of statement.declarations) {
      const init = decl.init;
      if (decl.id.type !== 'Identifier' || !init) continue;

      // Find the useRef call in either `useRef(…).current` or a bare `useRef(…)`.
      const viaCurrent =
        init.type === 'MemberExpression' &&
        !init.computed &&
        (init.property as t.Identifier).name === 'current' &&
        init.object.type === 'CallExpression' &&
        (init.object.callee as t.Identifier).name === 'useRef'
          ? init.object
          : null;
      const viaRef =
        init.type === 'CallExpression' &&
        (init.callee as t.Identifier).name === 'useRef'
          ? init
          : null;
      const useRefCall = viaCurrent || viaRef;

      // Skip anything that is not a useRef around PanResponder.create(…); an unwrapped create is an error.
      if (!useRefCall || !isPanCreate(useRefCall.arguments[0])) {
        if (isPanCreate(init)) {
          throw aotError(
            'AOT: PanResponder.create(...) must be kept in a useRef',
            `write \`const ${decl.id.name} = useRef(PanResponder.create({…})).current;\` — the ` +
              `recognizer owns the live gesture, so a fresh one per render would throw the drag ` +
              `away (the same rule Flow A enforces).`,
          );
        }

        continue;
      }

      // Build the responder's descriptor and record which way the app must spell its spread.
      const pan = buildPanDescriptor(
        decl.id.name,
        useRefCall.arguments[0] as t.CallExpression,
        prefix,
      );
      pan.viaRefCurrent = !viaCurrent; // how the app spells the spread: pan.current.panHandlers
      pans.set(decl.id.name, pan);
    }
  }

  return pans;
}

/**
 * Lowers a should-set predicate to a single condition: the engine calls these synchronously inside
 * hit-testing, before anyone owns the gesture, so they may only READ.
 *
 * @param fnNode  The predicate.
 * @param env  The expression environment.
 * @param pan  The responder it belongs to.
 *
 * @returns The condition.
 */
function lowerPanQuery(
  fnNode: PanCallback,
  env: Env,
  pan: PanResponderRecord,
): IrExpr {
  // The predicate reads the event and gestureState through its own parameter names.
  const eventParam =
    fnNode.params[0]?.type === 'Identifier' ? fnNode.params[0].name : null;
  const gestureParam =
    fnNode.params[1]?.type === 'Identifier' ? fnNode.params[1].name : null;
  const queryEnv: Env = {...env, pan};
  if (eventParam) {
    queryEnv.event = eventParam;
  }

  if (gestureParam) {
    queryEnv.gesture = gestureParam;
  }

  // Its body must be one expression: an expression body, or a block that only returns one.
  const body = fnNode.body;
  const expr =
    body.type === 'BlockStatement'
      ? body.body.length === 1 && body.body[0].type === 'ReturnStatement'
        ? body.body[0].argument
        : null
      : body;
  if (!expr) {
    throw aotError(
      'AOT: a PanResponder should-set predicate must be a single boolean expression',
      'write it as `() => true` or `(e, g) => Math.abs(g.dx) > 8`. The engine asks these ' +
        'mid-hit-test, so they may only read (state, event/gesture fields, constants) — never ' +
        'set state.',
    );
  }

  return lowerExpr(expr, queryEnv);
}

/**
 * Lowers a PanResponder onto the engine's responder system, not a transpiled copy of it: the engine already runs
 * RN's state machine in C. Capture/bubble negotiation (er_responder_query_set) picks an owner, then GRANT / MOVE /
 * RELEASE / TERMINATE fire on the owner alone, carrying cumulative travel and velocity. So the should-set
 * predicates become QUERIES and the callbacks become responder EVENT handlers: no gesture math is duplicated into
 * the generated C, and a granted pan owns the gesture, so a ScrollView ancestor cannot auto-scroll out from under
 * it.
 *
 * The one thing with no engine counterpart is RN's anchor: `g.dx` is measured from the grant, while the engine's
 * `data->dx` runs from touch-down, so a claim that needed 10 px of slop would otherwise open with a 10 px jump.
 * That anchor, plus `x0`/`y0`, is a handful of file-scope ints per responder, kept by the grant, release and
 * terminate handlers. Those three are ALWAYS lowered, even with no app callback; move and reject only with one.
 *
 * @param pan  The responder; its gesture id must be assigned.
 * @param env  The expression environment.
 * @param state  The component's state table.
 * @param out  Everything emitted so far.
 *
 * @returns The responder.
 */
export function lowerPanResponder(
  pan: PanResponderRecord,
  env: Env,
  state: StateTable,
  out: Out,
): IrPanResponder {
  const panEnv: Env = {...env, pan};
  const event = (name: string, role: IrPanRole): IrPanEvent => ({
    event: name,
    role,
    callback: pan.events.has(name)
      ? lowerHandler(pan.events.get(name)!, panEnv, state, out, pan)
      : null,
  });

  const events = [
    event('ER_EVENT_RESPONDER_GRANT', 'grant'),
    event('ER_EVENT_RESPONDER_RELEASE', 'release'),
    event('ER_EVENT_RESPONDER_TERMINATE', 'terminate'),
  ];
  if (pan.events.has('ER_EVENT_RESPONDER_MOVE')) {
    events.push(event('ER_EVENT_RESPONDER_MOVE', 'move'));
  }
  if (pan.events.has('ER_EVENT_RESPONDER_REJECT')) {
    events.push(event('ER_EVENT_RESPONDER_REJECT', 'reject'));
  }

  return {
    name: pan.name,
    statePrefix: pan.cPrefix,
    fnPrefix: pan.fnPrefix,
    stateID: pan.stateID,
    events,
    queries: [...pan.queries].map(([query, predicate]) => ({
      query,
      test: lowerPanQuery(predicate, panEnv, pan),
    })),
  };
}

/**
 * Lowers one PanResponder once, however many nodes spread it (see lowerPanResponder), and adds its event
 * handlers to the output. Each node that spreads it wires them in its own build lines.
 *
 * @param pan  The responder.
 * @param out  Everything emitted so far.
 * @param env  The expression environment.
 * @param state  The component's state table.
 *
 * @returns The lowered responder.
 */
export function emitPanResponder(
  pan: PanResponderRecord,
  out: Out,
  env: Env,
  state: StateTable,
): IrPanResponder {
  if (!pan.lowered) {
    pan.stateID = ++out.panN;
    pan.lowered = lowerPanResponder(pan, env, state, out);
    out.panResponders.push(pan.lowered);
    for (const panEvent of pan.lowered.events) {
      out.handlers.push({kind: 'panEvent', pan: pan.lowered, event: panEvent});
    }
  }

  return pan.lowered;
}

/**
 * Resolves a JSX spread to the PanResponder it spreads, or null when it is some other spread. A name
 * that IS a responder but is spelled the other way around (`.current` where the app already unwrapped it,
 * or vice versa) is an error rather than a silent miss — Flow A would throw at runtime on the same code.
 *
 * @param argument  The spread's argument.
 * @param env  The expression environment.
 *
 * @returns The responder, or null when the spread is not `<responder>.panHandlers`.
 */
export function panSpreadTarget(
  argument: t.Node | null | undefined,
  env: Env,
): PanResponderRecord | null {
  // Only `<name>.panHandlers` or `<name>.current.panHandlers` can spread a responder.
  if (
    argument?.type !== 'MemberExpression' ||
    argument.computed ||
    (argument.property as t.Identifier).name !== 'panHandlers'
  ) {
    return null;
  }

  const handlersOwner = argument.object;
  const usesCurrent =
    handlersOwner.type === 'MemberExpression' &&
    !handlersOwner.computed &&
    (handlersOwner.property as t.Identifier).name === 'current' &&
    handlersOwner.object.type === 'Identifier';
  const rootName = usesCurrent
    ? ((handlersOwner as t.MemberExpression).object as t.Identifier).name
    : (handlersOwner as t.Identifier).name;
  const pan =
    handlersOwner.type === 'Identifier' || usesCurrent
      ? env.pans?.get(rootName)
      : null;
  if (!pan) return null;

  // The spread must unwrap `.current` exactly when the declaration did not.
  if (pan.viaRefCurrent !== usesCurrent) {
    throw aotError(
      `AOT: "${rootName}" is a PanResponder, but this spread does not match how it was declared`,
      pan.viaRefCurrent
        ? `it was declared as \`useRef(PanResponder.create({…}))\`, so spread \`{...${rootName}.current.panHandlers}\`.`
        : `it was declared as \`useRef(PanResponder.create({…})).current\`, so spread ` +
            `\`{...${rootName}.panHandlers}\`.`,
    );
  }

  return pan;
}
