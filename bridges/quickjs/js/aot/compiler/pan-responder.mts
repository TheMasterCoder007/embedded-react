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
import {emitExpr} from './expressions.mts';
import {isFn} from './collect.mts';
import {compileHandler} from './handlers.mts';
import type * as t from '@babel/types';
import type {Out} from './out.mts';
import type {
  CExpr,
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

/** Query constant → the suffix of the C function emitted for it. */
const PAN_QUERY_SUFFIX: Record<string, string> = {
  ER_QUERY_START_SHOULD_SET: 'start_should_set',
  ER_QUERY_START_SHOULD_SET_CAPTURE: 'start_should_set_capture',
  ER_QUERY_MOVE_SHOULD_SET: 'move_should_set',
  ER_QUERY_MOVE_SHOULD_SET_CAPTURE: 'move_should_set_capture',
  ER_QUERY_TERMINATION_REQUEST: 'termination_request',
};

/** RN PanResponder config key → the engine responder EVENT it lowers to. */
const PAN_EVENTS: Record<string, string> = {
  onPanResponderGrant: 'ER_EVENT_RESPONDER_GRANT',
  onPanResponderMove: 'ER_EVENT_RESPONDER_MOVE',
  onPanResponderRelease: 'ER_EVENT_RESPONDER_RELEASE',
  onPanResponderTerminate: 'ER_EVENT_RESPONDER_TERMINATE',
  onPanResponderReject: 'ER_EVENT_RESPONDER_REJECT',
};

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
 * One `gestureState` field → the C expression that reads it. Travel is rebased onto the grant (RN's
 * anchor); the point and the velocity come straight off the engine payload; the finger count is the
 * engine's own, which is why er_touch_active_count() exists.
 *
 * @param field  The field's name.
 * @param pan  The responder whose callback reads it.
 *
 * @returns The C expression.
 */
export function panGestureField(
  field: string,
  pan: PanResponderRecord | null | undefined,
): CExpr {
  switch (field) {
    case 'dx':
      return {code: `(data->dx - ${pan!.cPrefix}_base_dx)`, cType: 'int'};
    case 'dy':
      return {code: `(data->dy - ${pan!.cPrefix}_base_dy)`, cType: 'int'};
    case 'moveX':
      return {code: 'data->x', cType: 'int'};
    case 'moveY':
      return {code: 'data->y', cType: 'int'};
    case 'x0':
      return {code: `${pan!.cPrefix}_x0`, cType: 'int'};
    case 'y0':
      return {code: `${pan!.cPrefix}_y0`, cType: 'int'};
    case 'vx':
    case 'vy':
      return {code: `data->${field}`, cType: 'float'};
    case 'numberActiveTouches':
      return {code: 'er_touch_active_count()', cType: 'int'};
    case 'stateID':
      return {code: String(pan!.stateID), cType: 'int'};
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
    emitted: null,
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
 * Lowers a should-set predicate to a single C boolean expression: the engine calls these synchronously
 * inside hit-testing, before anyone owns the gesture, so they may only READ.
 *
 * @param fnNode  The predicate.
 * @param env  The expression environment.
 * @param pan  The responder it belongs to.
 *
 * @returns The C condition.
 */
function compilePanQuery(
  fnNode: PanCallback,
  env: Env,
  pan: PanResponderRecord,
): string {
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

  return `(${emitExpr(expr, queryEnv).code}) != 0`;
}

/**
 * Emits one PanResponder's gesture state + C callbacks (once, however many nodes spread it), then wires
 * them onto a node. The responder is lowered onto the engine's responder system, not transpiled: the engine
 * already runs RN's state machine in C. Capture/bubble negotiation (er_responder_query_set) picks an owner,
 * then GRANT / MOVE / RELEASE / TERMINATE fire on the owner alone, carrying cumulative travel and velocity.
 * So the should-set predicates become QUERIES and the callbacks become responder EVENT handlers: no gesture
 * math is duplicated into the generated C, and a granted pan owns the gesture, so a ScrollView ancestor
 * cannot auto-scroll out from under it.
 *
 * The one thing with no engine counterpart is RN's anchor: `g.dx` is measured from the grant, while the
 * engine's `data->dx` runs from touch-down, so a claim that needed 10 px of slop would otherwise open with a
 * 10 px jump. That anchor, plus `x0`/`y0`, is a handful of file-scope ints per responder, kept by the
 * grant/release/terminate handlers. Those are ALWAYS emitted, even with no user callback.
 *
 * @param pan  The responder.
 * @param nodeId  The node's variable.
 * @param out  Everything emitted so far.
 * @param env  The expression environment.
 * @param state  The component's state table.
 */
export function emitPanResponder(
  pan: PanResponderRecord,
  nodeId: string,
  out: Out,
  env: Env,
  state: StateTable,
): void {
  // Emit the responder's state and C functions the first time a node spreads it.
  if (!pan.emitted) {
    pan.stateID = ++out.panN;
    const cPrefix = pan.cPrefix;
    const panEnv: Env = {...env, pan};

    // userBody compiles an event's app callback; grantedGuard returns unless this responder owns the gesture.
    const userBody = (event: string): string[] =>
      pan.events.has(event)
        ? compileHandler(pan.events.get(event)!, panEnv, state, out, pan)
        : [];
    const grantedGuard = [
      '    if (!' + cPrefix + '_granted)',
      '    {',
      '        return;',
      '    }',
    ];

    // The gesture state: whether the gesture is granted, the anchor, and RN's x0/y0.
    out.panDecls.push(
      `/* PanResponder "${pan.name}" (gesture ${pan.stateID}) — RN's gestureState. The grant anchors the`,
      `   travel so g.dx opens at 0 however much slop the claim cost; data->dx runs from touch-down. */`,
      `static int ${cPrefix}_granted = 0;`,
      `static int ${cPrefix}_base_dx = 0;`,
      `static int ${cPrefix}_base_dy = 0;`,
      `static int ${cPrefix}_x0 = 0;`,
      `static int ${cPrefix}_y0 = 0;`,
    );

    // Every emitted handler and query is recorded, so each node that spreads the responder is wired to it.
    pan.emitted = {events: [], queries: []};
    const addEventHandler = (event: string, suffix: string, body: string[]) => {
      const handlerName = `${pan.fnPrefix}_${suffix}`;
      out.handlers.push({name: handlerName, body});
      pan.emitted!.events.push([event, handlerName]);
    };

    // Grant anchors the gesture where the claim happened, then runs the app's callback.
    addEventHandler('ER_EVENT_RESPONDER_GRANT', 'grant', [
      `    if (${cPrefix}_granted)`,
      '    {',
      '        return; /* a second finger re-granted this node: one gesture, not two */',
      '    }',
      `    ${cPrefix}_granted = 1;`,
      `    ${cPrefix}_base_dx = data->dx;`,
      `    ${cPrefix}_base_dy = data->dy;`,
      `    ${cPrefix}_x0 = data->x;`,
      `    ${cPrefix}_y0 = data->y;`,
      ...userBody('ER_EVENT_RESPONDER_GRANT'),
    ]);

    // Release and terminate end the gesture: run the callback while g.dx's anchor is still valid, then clear it.
    for (const [event, suffix] of [
      ['ER_EVENT_RESPONDER_RELEASE', 'release'],
      ['ER_EVENT_RESPONDER_TERMINATE', 'terminate'],
    ]) {
      addEventHandler(event, suffix, [
        ...grantedGuard,
        ...userBody(event),
        `    ${cPrefix}_granted = 0;`,
        `    ${cPrefix}_base_dx = 0;`,
        `    ${cPrefix}_base_dy = 0;`,
      ]);
    }

    // Move is emitted only for an app callback and runs it only while this responder owns the gesture.
    if (pan.events.has('ER_EVENT_RESPONDER_MOVE')) {
      addEventHandler('ER_EVENT_RESPONDER_MOVE', 'move', [
        ...grantedGuard,
        ...userBody('ER_EVENT_RESPONDER_MOVE'),
      ]);
    }

    // Reject fires on a node that ASKED for the gesture and was refused — it never owned it, so no guard.
    if (pan.events.has('ER_EVENT_RESPONDER_REJECT')) {
      addEventHandler(
        'ER_EVENT_RESPONDER_REJECT',
        'reject',
        userBody('ER_EVENT_RESPONDER_REJECT'),
      );
    }

    // Each should-set predicate becomes a C query function.
    for (const [query, predicate] of pan.queries) {
      const queryName = `${pan.fnPrefix}_${PAN_QUERY_SUFFIX[query]}`;
      out.queries.push({
        name: queryName,
        expr: compilePanQuery(predicate, panEnv, pan),
      });
      pan.emitted.queries.push([query, queryName]);
    }
  }

  // Wire the responder's handlers and queries onto this node.
  for (const [event, handlerName] of pan.emitted.events) {
    out.build.push(
      `    er_event_set(${nodeId}, ${event}, ${handlerName}, NULL);`,
    );
  }

  for (const [query, queryName] of pan.emitted.queries) {
    out.build.push(
      `    er_responder_query_set(${nodeId}, ${query}, ${queryName}, NULL);`,
    );
  }
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
