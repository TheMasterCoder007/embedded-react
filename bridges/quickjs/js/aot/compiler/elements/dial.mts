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

import {colorLiteral} from '../../style-map.mts';
import {aotError} from '../diagnostics.mts';
import {evalStaticOrThrow, evalStatic} from '../static-eval.mts';
import {floatLit} from '../c-syntax.mts';
import {lowerExpr} from '../expressions.mts';
import {isFn} from '../collect.mts';
import {
  attrExpr,
  lowerColorValue,
  lowerEnumValue,
  styleWrites,
  collectStyleAssigns,
} from '../style-text.mts';
import {compileValueHandler} from '../handlers.mts';
import {emitChildren} from '../control-flow.mts';
import {emitIrNode, lowerRefBinds} from '../nodes.mts';
import {imageNameFromSource} from './image.mts';
import type * as t from '@babel/types';
import type {Out} from '../out.mts';
import type {Env, Scope, StateTable} from '../types.mts';
import type {IrExpr} from '../ir/expressions.mts';
import type {IrNode} from '../ir/nodes.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

const props: string[] = [
  'value',
  'min',
  'max',
  'startAngle',
  'sweepAngle',
  'step',
  'thickness',
  'bandThickness',
  'trackColor',
  'indicatorColor',
  'indicatorGradient',
  'bandColor',
  'cap',
  'segments',
  'gapAngle',
  'knob',
  'knobSize',
  'minSpan',
  'knobColor',
  'knobBorderColor',
  'knobBorderWidth',
  'knobImage',
  'adjustable',
  'range',
  'valueStart',
  'onChange',
  'style',
];
const SUPPORTED_PROPS = `supported props: ${props.join(', ')}.`;

/** <Dial> prop → ERProps field tables (see emitDial). */
const DIAL_FLOAT_PROPS: Record<string, string> = {
  value: 'arc_value',
  valueStart: 'arc_value_start',
  minSpan: 'arc_min_span',
  min: 'arc_min',
  max: 'arc_max',
  startAngle: 'arc_start_angle',
  sweepAngle: 'arc_sweep_angle',
  step: 'arc_step',
  gapAngle: 'arc_gap_angle',
};
const DIAL_INT_PROPS: Record<string, string> = {
  thickness: 'arc_width',
  bandThickness: 'arc_band_width',
  knobSize: 'arc_knob_size',
  knobBorderWidth: 'arc_knob_border_width',
  segments: 'arc_segments',
};
const DIAL_COLOR_PROPS: Record<string, string> = {
  trackColor: 'arc_track_color',
  indicatorColor: 'arc_indicator_color',
  bandColor: 'arc_band_color',
  knobColor: 'arc_knob_color',
  knobBorderColor: 'arc_knob_border_color',
};
const DIAL_ENUM_PROPS: Record<
  string,
  {field: string; table: Record<string, string>}
> = {
  cap: {
    field: 'arc_cap',
    table: {butt: 'ER_ARC_CAP_BUTT', round: 'ER_ARC_CAP_ROUND'},
  },
  knob: {
    field: 'arc_knob',
    table: {
      none: 'ER_ARC_KNOB_NONE',
      circle: 'ER_ARC_KNOB_CIRCLE',
      image: 'ER_ARC_KNOB_IMAGE',
      child: 'ER_ARC_KNOB_CHILD',
    },
  },
};

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * <Dial value={v} min max startAngle sweepAngle step thickness bandThickness trackColor indicatorColor
 *       indicatorGradient bandColor cap segments gapAngle knob knobSize knobColor knobBorderColor
 *       knobBorderWidth knobImage adjustable onChange={(v) => setV(v)} style=… />
 * → ER_NODE_ARC, the engine's native arc widget. Numbers and colors are static literals or state-driven
 * (recomputed in app_update); `value` may also be a useAnimatedValue handle, which binds ER_PROP_ARC_VALUE
 * natively so a ramp costs no app_update at all. onChange lowers to ER_EVENT_VALUE_CHANGE with its param
 * bound to data->value (the quantized value the built-in drag produced). Default 120x120 box.
 *
 * @param element  The <Dial>.
 * @param scope  Compile-time constants in reach.
 * @param out  Everything emitted so far.
 * @param env  The expression environment.
 * @param state  The component's state table.
 *
 * @returns The node's variable.
 */
export function emitDial(
  element: t.JSXElement,
  scope: Scope,
  out: Out,
  env: Env,
  state: StateTable,
): IrNode {
  // get node id
  const nodeID = `n${out.allocateNodeId()}`;

  // collect style assignments
  const {staticAssigns, dynAssigns, binds} = collectStyleAssigns(
    element.openingElement,
    scope,
    env,
  );
  const hasField = (field: string) =>
    styleWrites(staticAssigns, dynAssigns, field);
  if (!hasField('width')) {
    staticAssigns.push({field: 'width', expr: '120'});
  }
  if (!hasField('height')) {
    staticAssigns.push({field: 'height', expr: '120'});
  }

  // Fold numeric props when possible; otherwise emit runtime assignments for app_update().
  const numeric = (field: string, node: t.Node, isFloat: boolean): void => {
    try {
      const number = evalStatic(node, scope);
      if (typeof number === 'number') {
        staticAssigns.push({
          field,
          expr: isFloat ? floatLit(number) : String(Math.round(number)),
        });
        return;
      }
    } catch {
      const value = lowerExpr(node, env);
      dynAssigns.push({
        field,
        value: isFloat ? {kind: 'float', value} : {kind: 'dimension', value},
      });
    }
  };

  // Fold static colors now; emit dynamic color expressions for app_update().
  const colour = (field: string, node: t.Node): void => {
    try {
      const color = evalStatic(node, scope);
      if (typeof color === 'string') {
        staticAssigns.push({field, expr: colorLiteral(color)});
      }
      return;
    } catch {
      dynAssigns.push({field, value: lowerColorValue(node, env)});
    }
  };

  // Process <Dial> props, splitting them into static props, dynamic updates, binds, assets, and handlers.
  let onChangeFn: t.Node | null | undefined = null;
  let knobImage: string | null = null; // static asset name
  for (const attr of element.openingElement.attributes) {
    // Ensure all props are explicit named attributes.
    // Spread props cannot be analyzed statically
    if (attr.type !== 'JSXAttribute') {
      throw aotError('AOT: spread props on <Dial> are not supported');
    }

    // skip non-props
    const name = (attr.name as t.JSXIdentifier).name;
    if (name === 'style' || name === 'ref' || name === 'key') continue;

    // process props
    const node = attrExpr(attr);
    if (
      (name === 'value' || name === 'valueStart') &&
      node?.type === 'Identifier' &&
      env.animations?.has(node.name)
    ) {
      binds.push({
        cVar: env.animations.get(node.name)!.cVar,
        prop:
          name === 'value' ? 'ER_PROP_ARC_VALUE' : 'ER_PROP_ARC_VALUE_START',
      });
    } else if (DIAL_FLOAT_PROPS[name]) {
      numeric(DIAL_FLOAT_PROPS[name], node, true);
    } else if (DIAL_INT_PROPS[name]) {
      numeric(DIAL_INT_PROPS[name], node, false);
    } else if (DIAL_COLOR_PROPS[name]) {
      colour(DIAL_COLOR_PROPS[name], node);
    } else if (DIAL_ENUM_PROPS[name]) {
      const {field, table} = DIAL_ENUM_PROPS[name];
      let token: unknown = null;
      try {
        token = evalStatic(node, scope);
      } catch {
        /* state-driven — handled below */
      }

      if (typeof token === 'string') {
        if (!table[token]) {
          throw aotError(
            `AOT: unsupported <Dial ${name}> "${token}"`,
            `${name} must be one of: ${Object.keys(table).join(' / ')}.`,
          );
        }
        staticAssigns.push({field, expr: table[token]});
      } else {
        dynAssigns.push({
          field,
          value: {kind: 'byte', value: lowerEnumValue(node, table, env)},
        });
      }
    } else if (name === 'adjustable' || name === 'range') {
      const field = name === 'range' ? 'arc_range' : 'arc_adjustable';
      try {
        staticAssigns.push({field, expr: evalStatic(node, scope) ? '1' : '0'});
      } catch {
        dynAssigns.push({
          field,
          value: {
            kind: 'flag',
            test: lowerExpr(node, env),
            truthiness: 'value',
            negate: false,
            whenTrue: 1,
          },
        });
      }
    } else if (name === 'knobImage') {
      knobImage = imageNameFromSource(node, env);
      if (knobImage == null) {
        throw aotError(
          'AOT: <Dial knobImage> must resolve to a static asset name',
          "use an imported image (`import knob from './knob.png'` → knobImage={knob}) or a string asset name.",
        );
      }
      const path = env.imageNames?.get(knobImage);
      if (path) {
        out.images.set(knobImage, path);
      }
    } else if (name === 'indicatorGradient') {
      // Allow indicatorGradient to be statically defined but conditionally enabled with a ternary against null/undefined.
      let gradNode: t.Node = node;
      let gradCond: {test: IrExpr; negate: boolean} | null = null;
      if (node?.type === 'ConditionalExpression') {
        const nullish = (n: t.Node | null | undefined) =>
          n?.type === 'NullLiteral' ||
          (n?.type === 'Identifier' && n.name === 'undefined');
        if (nullish(node.alternate)) {
          gradNode = node.consequent;
          gradCond = {test: lowerExpr(node.test, env), negate: false};
        } else if (nullish(node.consequent)) {
          gradNode = node.alternate;
          gradCond = {test: lowerExpr(node.test, env), negate: true};
        }
      }

      // Validate and normalize the static gradient definition, keeping only the engine-supported stop count.
      const gradient = evalStaticOrThrow(
        gradNode,
        scope,
        'AOT: <Dial indicatorGradient> must be a static object (optionally behind a ternary against null)',
        "indicatorGradient={{ type: 'conic', stops: [{ color: '#00f' }, { color: '#f00' }] }}, or " +
          'indicatorGradient={on ? {…} : null}',
      ) as
        | {type?: unknown; stops?: {color?: unknown; offset?: unknown}[]}
        | null
        | undefined;
      const stops = Array.isArray(gradient?.stops)
        ? gradient!.stops!.slice(0, 4)
        : [];
      if (stops.length < 2) {
        throw aotError(
          'AOT: <Dial indicatorGradient> needs at least two stops (max 4)',
        );
      }
      staticAssigns.push({
        field: 'gradient_type',
        expr:
          gradient!.type === 'radial'
            ? 'ER_GRADIENT_RADIAL'
            : 'ER_GRADIENT_CONIC',
      });

      // Emit the stop count separately so a ternary can enable/disable the static gradient at runtime.
      if (gradCond) {
        dynAssigns.push({
          field: 'gradient_stop_count',
          value: {
            kind: 'flag',
            test: gradCond.test,
            truthiness: 'value',
            negate: gradCond.negate,
            whenTrue: stops.length,
          },
        });
      } else {
        staticAssigns.push({
          field: 'gradient_stop_count',
          expr: String(stops.length),
        });
      }

      // Emit each fixed stop, filling in missing offsets with evenly spaced positions.
      stops.forEach((st, i) => {
        const off =
          typeof st.offset === 'number' ? st.offset : i / (stops.length - 1);
        staticAssigns.push({
          field: `gradient_stops[${i}].color`,
          expr: colorLiteral(String(st.color)),
        });
        staticAssigns.push({
          field: `gradient_stops[${i}].position`,
          expr: floatLit(off),
        });
      });
    } else if (name === 'onChange') {
      onChangeFn = node;
    } else {
      throw aotError(
        `AOT: <Dial> prop "${name}" is not supported`,
        SUPPORTED_PROPS,
      );
    }
  }

  // Animated bindings: style props plus an animated `value` (ER_PROP_ARC_VALUE).
  const node: IrNode = {
    id: nodeID,
    type: 'ER_NODE_ARC',
    constantProps: staticAssigns,
    stateProps: dynAssigns,
    placeholder: null,
    text: null,
    image:
      knobImage != null
        ? {kind: 'static', name: knobImage, resetOnUpdate: true}
        : null,
    spans: null,
    children: [],
    wiring: binds.map((bind, index) => ({kind: 'bind', bind, index})),
  };

  // Compile <Dial onChange> into an ER_EVENT_VALUE_CHANGE handler.
  if (onChangeFn) {
    // A useCallback identifier resolves to its arrow, the same way the generic host-node event path does —
    // a dial's change handler is exactly the kind of thing an app wraps in useCallback.
    if (
      onChangeFn.type === 'Identifier' &&
      env.callbacks?.has(onChangeFn.name)
    ) {
      onChangeFn = env.callbacks.get(onChangeFn.name);
    }

    // Require a function node so compileValueHandler can bind the dial's value parameters.
    if (!isFn(onChangeFn)) {
      throw aotError(
        'AOT: <Dial onChange> must be an inline function or a useCallback',
        'onChange={(v) => setValue(v)}',
      );
    }

    const handlerName = `er_handler_${out.handlers.length}`;
    out.handlers.push({
      name: handlerName,
      body: compileValueHandler(
        onChangeFn,
        'data->value',
        env,
        state,
        out,
        'float',
        'data->value_start',
      ),
    });
    node.wiring.push({
      kind: 'event',
      event: 'ER_EVENT_VALUE_CHANGE',
      handler: handlerName,
    });
  }

  for (const slot of lowerRefBinds(element.openingElement, env)) {
    node.wiring.push({kind: 'ref', slot});
  }

  emitIrNode(node, out);
  node.children = emitChildren(
    element.children,
    nodeID,
    scope,
    out,
    env,
    state,
  );
  return node;
}
