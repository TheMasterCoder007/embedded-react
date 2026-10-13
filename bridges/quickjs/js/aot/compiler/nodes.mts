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

import {checkNumber} from './check.mts';
import {NODE_TYPES} from '../style-map.mts';
import {aotError, withLoc} from './diagnostics.mts';
import {evalStaticOrThrow, evalStatic, foldScope} from './static-eval.mts';
import {lowerExpr} from './expressions.mts';
import {isFn} from './collect.mts';
import {
  attrExpr,
  collectStyleAssigns,
  EVENT_TYPES,
  lowerNodeText,
  collectTextSpans,
} from './style-text.mts';
import {lowerHandler} from './handlers.mts';
import {emitPanResponder, panSpreadTarget} from './pan-responder.mts';
import {emitComponent, emitChildren} from './control-flow.mts';
import {emitSvg} from './svg.mts';
import {emitDial} from './elements/dial.mts';
import {emitSwitch} from './elements/switch.mts';
import {emitTextInput} from './elements/text-input.mts';
import {emitActivityIndicator} from './elements/activity-indicator.mts';
import {emitModal} from './elements/modal.mts';
import {emitFlatList} from './elements/flat-list.mts';
import {resolveImageAttrs} from './elements/image.mts';
import type * as t from '@babel/types';
import type {AotError} from './diagnostics.mts';
import type {Out} from './out.mts';
import type {AnimBind} from './style-text.mts';
import type {IrElement, IrNode, IrWire} from './ir/nodes.mts';
import type {IrPressFade} from './ir/functions.mts';
import type {
  DynAssign,
  EmitOptions,
  Env,
  PanResponderRecord,
  Scope,
  StateTable,
  StaticAssign,
} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

// JS-only wrappers with no lowering yet, mapped to the tree to write by hand; "unknown element" would mislead.
const FLOW_A_ONLY_COMPONENTS: Record<string, string> = {
  Button:
    '<Pressable style={…} onPress={…}><Text style={…}>title</Text></Pressable>.',
  ImageBackground:
    "<View style={…}><Image source={…} style={{position: 'absolute', top: 0, left: 0, " +
    'right: 0, bottom: 0}} />…children…</View>.',
  SectionList:
    '<ScrollView> with the header element and a data.map(…) per section (a section is not ' +
    'one .map, which is why it has no lowering).',
};

// <TouchableOpacity> press feedback (see touchablePressFades).
const TOUCHABLE_ACTIVE_OPACITY = 0.2; // RN's default
const TOUCHABLE_RESTORE_MS = 250; // RN's fade-back; the dim itself lands with no ramp

/** The press events a <TouchableOpacity disabled> drops — RN presses none of them and dims for none. */
const PRESS_EVENTS = new Set([
  'onPress',
  'onLongPress',
  'onPressIn',
  'onPressOut',
]);

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Resolves a JSX tag to a name, mapping `Animated.View/Text/Image` to their host element.
 *
 * @param openingElement  The element's opening tag.
 *
 * @returns The tag name.
 */
function resolveTag(openingElement: t.JSXOpeningElement): string {
  const nameNode = openingElement.name;
  if (nameNode.type === 'JSXIdentifier') return nameNode.name;

  if (
    nameNode.type === 'JSXMemberExpression' &&
    (nameNode.object as t.JSXIdentifier).name === 'Animated'
  ) {
    return nameNode.property.name; // Animated.View → View
  }

  throw new Error('AOT: unsupported JSX tag expression');
}

/**
 * The node refs `ref={myRef}` stores the element's handle in (a node ref from useRef), marked used.
 *
 * @param openingElement  The element's opening tag.
 * @param env  The expression environment.
 *
 * @returns The refs' C slots.
 */
export function lowerRefBinds(
  openingElement: t.JSXOpeningElement,
  env: Env,
): string[] {
  const refs: string[] = [];
  for (const attr of openingElement.attributes) {
    if (
      attr.type !== 'JSXAttribute' ||
      (attr.name as t.JSXIdentifier).name !== 'ref'
    ) {
      continue;
    }

    const refExpr =
      attr.value?.type === 'JSXExpressionContainer'
        ? attr.value.expression
        : null;
    if (
      refExpr?.type === 'Identifier' &&
      env.refs?.get(refExpr.name)?.kind === 'node'
    ) {
      const nodeRef = env.refs.get(refExpr.name)!;
      nodeRef.used = true;
      refs.push(nodeRef.cVar);
    } else {
      throw new Error(
        'AOT: ref={…} must reference a node ref declared with useRef()',
      );
    }
  }

  return refs;
}

/**
 * A JSX element's named attribute, or undefined.
 *
 * @param openingElement  The element's opening tag.
 * @param name  The attribute's name.
 *
 * @returns The attribute, if the element has it.
 */
function namedAttr(
  openingElement: t.JSXOpeningElement,
  name: string,
): t.JSXAttribute | undefined {
  return openingElement.attributes.find(
    (attr): attr is t.JSXAttribute =>
      attr.type === 'JSXAttribute' && attr.name && attr.name.name === name,
  );
}

/**
 * True for `<TouchableOpacity disabled>` / `disabled={SOME_CONST}`. Must fold: `disabled` decides what
 * the build EMITS — the press handlers and the dim binding — so there is nothing left to decide at runtime.
 *
 * @param element  The <TouchableOpacity>.
 * @param scope  Compile-time constants in reach.
 *
 * @returns Whether it is disabled.
 */
function touchableIsDisabled(element: t.JSXElement, scope: Scope): boolean {
  const attr = namedAttr(element.openingElement, 'disabled');
  if (!attr) return false;

  return !!evalStaticOrThrow(
    attrExpr(attr),
    scope,
    'AOT: <TouchableOpacity disabled> must be a compile-time constant',
    'a disabled one still renders — it lowers to a plain <Pressable>, children, layout ' +
      'and style intact — but its press handlers and its dim are not emitted at all, so the ' +
      'AOT has to know at build time. To gate on state, leave it enabled and return early ' +
      'in the handler (`onPress={() => { if (!ready) return; … }}`).',
  );
}

/**
 * A folded `activeOpacity` as a number in 0..1. Everything else is an error HERE, where the source line
 * is still in reach: it would otherwise reach floatLit and land in the generated C as a literal that
 * either does not compile (`NaNf`) or quietly means something the app never asked for — a bare
 * `activeOpacity` is `true`, so `1.0f`, which is not dim at all; `{false}` is an invisible one.
 *
 * @param value  The folded value.
 * @param attr  The attribute, for the error's location.
 *
 * @returns The opacity.
 */
function opacityLiteralOrThrow(value: unknown, attr: t.JSXAttribute): number {
  if (
    typeof value !== 'number' ||
    !Number.isFinite(value) ||
    value < 0 ||
    value > 1
  ) {
    // JSON.stringify turns NaN into the STRING "null", which reads as a different mistake entirely.
    const shown =
      typeof value === 'number'
        ? String(value)
        : (JSON.stringify(value) ?? String(value));
    const error = aotError(
      `AOT: <TouchableOpacity activeOpacity> must be a number between 0 and 1 (got ${shown})`,
      'it is the opacity the node dims to while held — 0 is invisible, 1 is no dim at all. RN defaults it to 0.2.',
    );
    if (attr.loc) {
      error.aotLoc = attr.loc.start;
    }

    throw error;
  }

  return value;
}

/**
 * Lowers <TouchableOpacity>'s press feedback: a Pressable node plus one synthetic animated value bound to its
 * opacity. Press-in snaps it to activeOpacity, press-out fades it back to whatever the style asks for. Flow A
 * builds the same thing out of hooks (TouchableOpacity.js); here the feedback runs on the engine's native
 * driver with no JS on the device at all. Returns the value's bind, and the two fades for the event loop to
 * fold into its press handlers.
 *
 * @param element  The <TouchableOpacity>.
 * @param nodeId  The node's variable.
 * @param staticAssigns  The style's constant writes.
 * @param dynAssigns  The style's state-driven writes.
 * @param binds  The style's animated-value binds.
 * @param out  Everything emitted so far.
 * @param scope  Compile-time constants in reach.
 *
 * @returns The opacity bind, and the fade to run on onPressIn and the one to run on onPressOut.
 */
function touchablePressFades(
  element: t.JSXElement,
  nodeId: string,
  staticAssigns: StaticAssign[],
  dynAssigns: DynAssign[],
  binds: AnimBind[],
  out: Out,
  scope: Scope,
): {bind: AnimBind; fades: Record<string, IrPressFade>} {
  // The press feedback owns the node's opacity, so the style may not animate it or drive it from state.
  const hasAnimatedOpacity = binds.some(
    bind => bind.prop === 'ER_PROP_OPACITY',
  );
  if (
    hasAnimatedOpacity ||
    dynAssigns.some(assign => assign.field === 'opacity')
  ) {
    throw aotError(
      `AOT: <TouchableOpacity> cannot take ${hasAnimatedOpacity ? 'an Animated' : 'a state-driven'} opacity`,
      "the press feedback owns this node's opacity — a second writer does not blend with " +
        'it, it races it. To animate opacity yourself, use <Pressable>: the dim is just ' +
        'onPressIn/onPressOut driving an Animated.Value (see TouchableOpacity.js for the ' +
        'whole of it).',
    );
  }

  // lowerStyle scaled static opacity to 0–255 (the resting value); leave a NaN one for the C compiler to reject.
  const opacityAssign = staticAssigns.find(
    assign => assign.field === 'opacity',
  );
  const restingRaw = opacityAssign ? Number(opacityAssign.expr) / 255 : 1;
  const restingOpacity = Number.isFinite(restingRaw) ? restingRaw : 1;

  // The dim target: a folded activeOpacity, or RN's default.
  const activeAttr = namedAttr(element.openingElement, 'activeOpacity');
  const activeOpacity = activeAttr
    ? opacityLiteralOrThrow(
        evalStaticOrThrow(
          attrExpr(activeAttr),
          scope,
          'AOT: <TouchableOpacity activeOpacity> must be a compile-time constant',
          'the dim target is baked into the generated handler, so it cannot come from state.',
        ),
        activeAttr,
      )
    : TOUCHABLE_ACTIVE_OPACITY;

  // Bind a fresh animated value, starting at rest, to the node's opacity.
  const cVar = `s_av_press_${nodeId}`;
  checkNumber(restingOpacity);
  out.childAnimations.push({cVar, initial: restingOpacity});

  // Each fade is one timing animation of that value.
  return {
    bind: {cVar, prop: 'ER_PROP_OPACITY'},
    fades: {
      onPressIn: {value: cVar, opacity: activeOpacity, durationMs: 0},
      onPressOut: {
        value: cVar,
        opacity: restingOpacity,
        durationMs: TOUCHABLE_RESTORE_MS,
      },
    },
  };
}

/**
 * Lowers one JSX element to its engine node (see emitNode, the located entry point): dispatches the typed
 * elements (Svg, Switch, Dial, TextInput, ActivityIndicator, Modal, FlatList) and function components to
 * their emitters, and builds a generic host node (View / Text / Pressable / TouchableOpacity / Image /
 * ScrollView) itself: style, text, events, refs, and children.
 *
 * @param element  The element.
 * @param scope  Compile-time constants in reach.
 * @param out  Everything emitted so far.
 * @param env  The expression environment.
 * @param state  The component's state table.
 * @param opts  How the element is placed.
 *
 * @returns What the element lowered to.
 */
function emitNodeImpl(
  element: t.JSXElement,
  scope: Scope,
  out: Out,
  env: Env,
  state: StateTable,
  opts: EmitOptions = {},
): IrElement {
  // Drop props that fold to undefined, then hand each typed element to its own emitter.
  element = omitUndefinedAttrs(element, scope, env);
  const tag = resolveTag(element.openingElement);
  const typed = (node: IrNode): IrElement => ({
    kind: 'node',
    id: node.id,
    node,
  });
  if (tag === 'Svg') {
    return typed(emitSvg(element, scope, out, env, state, opts));
  }
  if (tag === 'Switch') {
    return typed(emitSwitch(element, scope, out, env, state));
  }
  if (tag === 'Dial') return typed(emitDial(element, scope, out, env, state));
  if (tag === 'TextInput') {
    return typed(emitTextInput(element, scope, out, env, state));
  }
  if (tag === 'ActivityIndicator') {
    return typed(emitActivityIndicator(element, scope, out, env));
  }
  if (tag === 'Modal') {
    return typed(emitModal(element, scope, out, env, state));
  }
  if (tag === 'FlatList') {
    return emitFlatList(element, scope, out, env, state, opts);
  }

  // Anything else must be a host node or a function component defined in this file.
  const nodeType = NODE_TYPES[tag];
  if (!nodeType) {
    if (out.components.has(tag)) {
      return emitComponent(element, scope, out, env, state, opts);
    }

    // A package export with no lowering yet gets its own error; "unknown element" would read like a typo.
    const replacement = FLOW_A_ONLY_COMPONENTS[tag];
    if (replacement) {
      throw aotError(
        `AOT: <${tag}> is not supported in Flow B yet`,
        `it renders in Flow A (the simulator and the QuickJS runtime) but the AOT has no ` +
          `lowering for it. Write it out by hand for the device build: ${replacement}`,
      );
    }

    throw aotError(
      `AOT: unknown element <${tag}> (not a built-in or a component in this file)`,
      `<${tag}> must be a built-in (View / Text / Pressable / TouchableOpacity / Image / ` +
        `ScrollView / Svg + shapes / Animated.*) or a function component defined in THIS ` +
        `file. Check the import / spelling or define the component here.`,
    );
  }

  // A disabled <TouchableOpacity> is a plain Pressable with no handlers or dim; the engine has no disabled flag.
  const isTouchableDisabled =
    tag === 'TouchableOpacity' && touchableIsDisabled(element, scope);

  // Reject spreads, which the loops below would silently drop, except {...pan.panHandlers}.
  const panSpreads: PanResponderRecord[] = [];
  for (const spread of element.openingElement.attributes) {
    if (spread.type !== 'JSXSpreadAttribute') continue;

    let panResponder;
    try {
      panResponder = panSpreadTarget(spread.argument, env);
    } catch (error) {
      if (spread.loc && !(error as AotError).aotLoc) {
        (error as AotError).aotLoc = spread.loc.start;
      }
      throw error;
    }

    if (panResponder) {
      panSpreads.push(panResponder);
      continue;
    }

    const spreadError = aotError(
      `AOT: a spread {...} on <${tag}> is not supported`,
      `list each prop explicitly (e.g. style={…} onPress={…}). The only spread a host ` +
        `element understands is a PanResponder's ({...pan.panHandlers}); otherwise spread ` +
        `props are supported on a function component instance whose spread object folds to a ` +
        `compile-time constant.`,
    );
    if (spread.loc) {
      spreadError.aotLoc = spread.loc.start;
    }

    throw spreadError;
  }

  // Allocate the node and collect its style, text content, and image attributes.
  const nodeId = `n${out.allocateNodeId()}`;
  const {staticAssigns, dynAssigns, binds} = collectStyleAssigns(
    element.openingElement,
    scope,
    env,
  );
  // A <Text> with a nested <Text> becomes inline SPANS; otherwise a single (possibly dynamic) string.
  const spans =
    tag === 'Text' ? collectTextSpans(element.children, scope, env) : null;
  const text =
    tag === 'Text' && !spans
      ? lowerNodeText(element.children, scope, env)
      : null;
  // An <Image>'s resize/tint are static, so fold them into staticAssigns for both the static and dynamic paths.
  const image = tag === 'Image' ? resolveImageAttrs(element, env, out) : null;
  if (image?.resizeMode) {
    staticAssigns.push({field: 'resize_mode', expr: image.resizeMode});
  }
  if (image?.tintColor) {
    staticAssigns.push({field: 'tint_color', expr: image.tintColor});
  }

  // `visible` is `display` as in Flow A (style `display` wins); not in collectStyleAssigns, which <Modal> shares.
  const visibleAttr = element.openingElement.attributes.find(
    (attr): attr is t.JSXAttribute =>
      attr.type === 'JSXAttribute' && attr.name && attr.name.name === 'visible',
  );
  if (
    visibleAttr &&
    !staticAssigns.some(assign => assign.field === 'display') &&
    !dynAssigns.some(assign => assign.field === 'display')
  ) {
    const expr = visibleAttr.value == null ? null : attrExpr(visibleAttr); // bare `visible` === true
    if (expr == null) {
      staticAssigns.push({field: 'display', expr: 'ER_DISPLAY_FLEX'});
    } else {
      try {
        staticAssigns.push({
          field: 'display',
          expr: evalStatic(expr, scope) ? 'ER_DISPLAY_FLEX' : 'ER_DISPLAY_NONE',
        });
      } catch {
        dynAssigns.push({
          field: 'display',
          value: {kind: 'visible', value: lowerExpr(expr, env)},
        });
      }
    }
  }

  // delayLongPress must fold since it is baked into props; 0 becomes 1 ms because 0 means "default" to the engine.
  const delayAttr = namedAttr(element.openingElement, 'delayLongPress');
  if (delayAttr) {
    const delayError = aotError(
      `AOT: <${tag} delayLongPress> must fold to a number`,
      'pass a literal or a module-level constant in milliseconds, e.g. delayLongPress={800}.',
    );
    if (delayAttr.value == null) {
      throw delayError;
    }

    let delayMs;
    try {
      delayMs = Number(evalStatic(attrExpr(delayAttr), scope));
    } catch {
      throw delayError;
    }

    if (!Number.isFinite(delayMs)) {
      throw delayError;
    }

    staticAssigns.push({
      field: 'long_press_ms',
      expr: String(Math.min(65535, Math.max(1, Math.round(delayMs)))),
    });
  }

  // A state-driven conditional flips `display` in app_update; pushed last to beat the element's visible/display.
  if (opts.show) {
    dynAssigns.push({
      field: 'display',
      value: {kind: 'shown', show: opts.show},
    });
  }

  // <TouchableOpacity>'s own opacity binding, on top of any the style asked for.
  const press =
    tag === 'TouchableOpacity' && !isTouchableDisabled
      ? touchablePressFades(
          element,
          nodeId,
          staticAssigns,
          dynAssigns,
          binds,
          out,
          scope,
        )
      : null;

  // A host node wires its binds (the press feedback's last), then its refs, events and PanResponders.
  const wiring: IrWire[] = binds.map((bind, index) => ({
    kind: 'bind',
    bind,
    index,
  }));
  if (press) {
    wiring.push({kind: 'bind', bind: press.bind, index: binds.length});
  }
  for (const slot of lowerRefBinds(element.openingElement, env)) {
    wiring.push({kind: 'ref', slot});
  }

  const node: IrNode = {
    id: nodeId,
    type: nodeType,
    constantProps: staticAssigns,
    stateProps: dynAssigns,
    placeholder: null,
    text,
    image:
      image?.imageNameDyn != null
        ? {kind: 'value', value: image.imageNameDyn}
        : image?.imageName != null
          ? {kind: 'static', name: image.imageName, resetOnUpdate: false}
          : null,
    spans,
    wiring,
    children: [],
  };

  // Compile each event prop into a handler; a disabled <TouchableOpacity> drops its press events.
  const fadesWired = new Set<string>();
  for (const attr of element.openingElement.attributes) {
    if (attr.type !== 'JSXAttribute') continue;

    const eventType = EVENT_TYPES[(attr.name as t.JSXIdentifier).name];
    if (!eventType) continue;

    if (
      isTouchableDisabled &&
      PRESS_EVENTS.has((attr.name as t.JSXIdentifier).name)
    ) {
      continue;
    }

    const fn = attrExpr(attr);
    let handlerName;
    if (fn.type === 'Identifier' && env.callbacks?.has(fn.name)) {
      // A useCallback compiles to one shared handler, namespaced by cbPrefix so each child instance gets its own.
      const callbackKey = `${env.cbPrefix ?? ''}${fn.name}`;
      handlerName = out.cbEmitted.get(callbackKey);
      if (!handlerName) {
        handlerName = `er_cb_${callbackKey}`;
        out.cbEmitted.set(callbackKey, handlerName);
        out.handlers.push({
          kind: 'callback',
          name: handlerName,
          callback: lowerHandler(env.callbacks.get(fn.name)!, env, state, out),
        });
      }
    } else if (fn.type === 'Identifier' && env.fnProps?.has(fn.name)) {
      // Inline a callback prop's function, compiled in the caller's env/state so its setters and locals resolve.
      const callbackProp = env.fnProps.get(fn.name)!;
      handlerName = `er_handler_${out.handlers.length}`;
      out.handlers.push({
        kind: 'callback',
        name: handlerName,
        callback: lowerHandler(
          callbackProp.node,
          callbackProp.env,
          callbackProp.state,
          out,
        ),
      });
    } else if (isFn(fn)) {
      handlerName = `er_handler_${out.handlers.length}`;
      out.handlers.push({
        kind: 'callback',
        name: handlerName,
        callback: lowerHandler(fn, env, state, out),
      });
    } else {
      throw aotError(
        `AOT: ${(attr.name as t.JSXIdentifier).name} must be an inline function, a useCallback, or a callback prop`,
        `pass an inline arrow (onPress={() => setX(…)}), a useCallback identifier, or a ` +
          `function prop received by this component.`,
      );
    }

    // Wrap the app's press-in/out with the fade; prepending would dim every other element sharing a useCallback.
    const pressFade = press?.fades[(attr.name as t.JSXIdentifier).name];
    if (pressFade) {
      const wrapperName = `er_handler_${out.handlers.length}`;
      out.handlers.push({
        kind: 'pressFade',
        name: wrapperName,
        fade: pressFade,
        then: handlerName,
      });
      handlerName = wrapperName;
      fadesWired.add((attr.name as t.JSXIdentifier).name);
    }

    wiring.push({kind: 'event', event: eventType, handler: handlerName});
  }

  // Whichever end of the press the app did not handle itself is the fade on its own.
  if (press) {
    for (const eventName of ['onPressIn', 'onPressOut']) {
      if (fadesWired.has(eventName)) continue;
      const fadeHandlerName = `er_handler_${out.handlers.length}`;
      out.handlers.push({
        kind: 'pressFade',
        name: fadeHandlerName,
        fade: press.fades[eventName],
        then: null,
      });
      wiring.push({
        kind: 'event',
        event: EVENT_TYPES[eventName],
        handler: fadeHandlerName,
      });
    }
  }

  // A PanResponder is lowered on the first node that spreads it.
  for (const panResponder of panSpreads) {
    wiring.push({
      kind: 'responder',
      pan: emitPanResponder(panResponder, out, env, state),
    });
  }

  checkInterpolations(node);

  // A <Text>'s children are its text, already consumed above.
  if (tag !== 'Text') {
    node.children = emitChildren(element.children, scope, out, env, state);
  }

  return {kind: 'node', id: nodeId, node};
}

/**
 * `element` without the attributes whose value folds to `undefined` in this scope — typically a prop a
 * child component was never given. Flow A omits an undefined prop, so every reader has to see it as absent;
 * read as a value, `visible` would hide the node and `placeholder` would print the word "undefined". This
 * is the one place that rule lives: every element reaches its reader through here. A copy is returned only
 * when something is dropped, because the same JSX node is emitted once per scope (each .map row, each
 * component instance) and one scope's answer must not leak into another's.
 *
 * @param element  The element.
 * @param scope  Compile-time constants in reach.
 * @param env  The expression environment.
 *
 * @returns The element, or a copy of it without those attributes.
 */
function omitUndefinedAttrs(
  element: t.JSXElement,
  scope: Scope,
  env: Env,
): t.JSXElement {
  const attrs = element.openingElement.attributes;
  // Built lazily: the runtime-aware scope plus the global `undefined`, so `SHOW ? true : undefined` folds too.
  let foldingScope: Scope | null = null;
  const keptAttrs = attrs.filter(attr => {
    if (
      attr.type !== 'JSXAttribute' ||
      attr.value?.type !== 'JSXExpressionContainer'
    ) {
      return true;
    }

    foldingScope ??= Object.assign(Object.create(foldScope(env, scope)), {
      undefined,
    });
    try {
      return evalStatic(attr.value.expression, foldingScope!) !== undefined;
    } catch {
      return true; // not a compile-time value — its reader decides
    }
  });

  return keptAttrs.length === attrs.length
    ? element
    : {
        ...element,
        openingElement: {...element.openingElement, attributes: keptAttrs},
      };
}

/**
 * Refuses an interpolation table holding a number C cannot write, at the element that binds it rather than when
 * its build lines are written.
 *
 * @param node  The node.
 */
export function checkInterpolations(node: IrNode): void {
  for (const wire of node.wiring) {
    if (wire.kind !== 'bind' || !wire.bind.interp) continue;
    for (const point of [
      ...wire.bind.interp.input,
      ...wire.bind.interp.output,
    ]) {
      checkNumber(point);
    }
  }
}

/** Lowers one JSX element to its engine node; an AOT error it throws is pinned to the element. */
export const emitNode = withLoc(emitNodeImpl);
