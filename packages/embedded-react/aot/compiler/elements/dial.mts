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

import {check, checkNumber, DIAL_MAX_GRADIENT_STOPS} from '../check.mts';
import {floatConstant, parseColorValue} from '../../style-map.mts';
import {aotError, currentLoc} from '../diagnostics.mts';
import {evalStaticOrThrow, evalStatic} from '../static-eval.mts';
import {lowerExpr} from '../expressions.mts';
import {isFn} from '../collect.mts';
import {
  attrExpr,
  lowerColorValue,
  lowerEnumValue,
  styleWrites,
  collectStyleAssigns,
} from '../style-text.mts';
import {lowerValueHandler} from '../handlers.mts';
import {emitChildren} from '../control-flow.mts';
import {checkInterpolations, lowerRefBinds} from '../nodes.mts';
import {at, elementSpec} from './props.mts';
import {imageNameFromSource} from './image.mts';
import {FLOAT} from '../ir/types.mts';
import type * as t from '@babel/types';
import type {Out} from '../out.mts';
import type {EngineName, Env, Scope, StateTable} from '../types.mts';
import type {IrExpr} from '../ir/expressions.mts';
import type {IrNode} from '../ir/nodes.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

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
  {field: string; table: Record<string, EngineName>}
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

/** A <Dial>'s width and height when its style sets neither. */
const DEFAULT_SIZE = 120;

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * <Dial value={v} min max startAngle sweepAngle step thickness bandThickness trackColor indicatorColor
 *       indicatorGradient bandColor cap segments gapAngle knob knobSize knobColor knobBorderColor
 *       knobBorderWidth knobImage adjustable onChange={(v) => setV(v)} style=… />
 * → ER_NODE_ARC, the engine's native arc widget. Numbers and colors are static literals or state-driven
 * (recomputed in the update); `value` may also be a useAnimatedValue handle, which binds ER_PROP_ARC_VALUE
 * natively so a ramp costs no update at all. onChange lowers to ER_EVENT_VALUE_CHANGE with its param
 * bound to data->value (the quantized value the built-in drag produced). Default 120x120 box.
 *
 * @param element  The <Dial>.
 * @param scope  Compile-time constants in reach.
 * @param output  Everything emitted so far.
 * @param environment  The expression environment.
 * @param state  The component's state table.
 *
 * @returns The node's variable.
 */
export function emitDial(
  element: t.JSXElement,
  scope: Scope,
  output: Out,
  environment: Env,
  state: StateTable,
): IrNode {
  // get node id
  const nodeID = `n${output.allocateNodeId()}`;

  // collect style assignments
  const {staticAssigns, dynamicAssigns, binds} = collectStyleAssigns(
    element.openingElement,
    scope,
    environment,
    elementSpec('Dial')!,
  );
  const hasField = (field: string) =>
    styleWrites(staticAssigns, dynamicAssigns, field);
  if (!hasField('width')) {
    staticAssigns.push({field: 'width', value: DEFAULT_SIZE});
  }
  if (!hasField('height')) {
    staticAssigns.push({field: 'height', value: DEFAULT_SIZE});
  }

  // Fold numeric props when possible; otherwise emit runtime assignments for the update.
  const numeric = (
    field: string,
    node: t.Node,
    isFloat: boolean,
    attribute: t.JSXAttribute,
  ): void => {
    let number: unknown;
    try {
      number = evalStatic(node, scope, 'strict');
    } catch {
      const value = lowerExpr(node, environment);
      dynamicAssigns.push({
        field,
        value: isFloat ? {kind: 'float', value} : {kind: 'dimension', value},
      });
      return;
    }

    if (typeof number !== 'number') {
      throw at(
        aotError(
          `AOT: <Dial ${attribute.name.name as string}> must be a number (got ${JSON.stringify(number) ?? String(number)})`,
        ),
        attribute,
      );
    }

    // floatConstant checks a float field itself; an int field needs the same check before it is rounded.
    if (!isFloat) {
      checkNumber(number);
    }

    staticAssigns.push({
      field,
      value: isFloat ? floatConstant(number) : Math.round(number),
    });
  };

  // Fold static colors now; emit dynamic color expressions for the update.
  const colour = (
    field: string,
    node: t.Node,
    attribute: t.JSXAttribute,
  ): void => {
    let color: unknown;
    try {
      color = evalStatic(node, scope, 'strict');
    } catch {
      dynamicAssigns.push({field, value: lowerColorValue(node, environment)});
      return;
    }

    if (typeof color !== 'string') {
      throw at(
        aotError(
          `AOT: <Dial ${attribute.name.name as string}> must be a color string (got ${JSON.stringify(color) ?? String(color)})`,
        ),
        attribute,
      );
    }

    staticAssigns.push({field, value: parseColorValue(color)});
  };

  // Process <Dial> props, splitting them into static props, dynamic updates, binds, assets, and handlers.
  let onChangeFunction: t.Node | null | undefined = null;
  let knobImage: string | null = null; // static asset name
  for (const attribute of element.openingElement.attributes) {
    // Ensure all props are explicit named attributes.
    // Spread props cannot be analyzed statically
    if (attribute.type !== 'JSXAttribute') {
      throw aotError('AOT: spread props on <Dial> are not supported');
    }

    // skip non-props
    const name = (attribute.name as t.JSXIdentifier).name;
    if (name === 'style' || name === 'ref' || name === 'key') continue;

    // process props
    const node = attrExpr(attribute);
    if (
      (name === 'value' || name === 'valueStart') &&
      node?.type === 'Identifier' &&
      environment.animations?.has(node.name)
    ) {
      binds.push({
        slotName: environment.animations.get(node.name)!.slotName,
        prop:
          name === 'value' ? 'ER_PROP_ARC_VALUE' : 'ER_PROP_ARC_VALUE_START',
      });
    } else if (DIAL_FLOAT_PROPS[name]) {
      numeric(DIAL_FLOAT_PROPS[name], node, true, attribute);
    } else if (DIAL_INT_PROPS[name]) {
      numeric(DIAL_INT_PROPS[name], node, false, attribute);
    } else if (DIAL_COLOR_PROPS[name]) {
      colour(DIAL_COLOR_PROPS[name], node, attribute);
    } else if (DIAL_ENUM_PROPS[name]) {
      const {field, table} = DIAL_ENUM_PROPS[name];
      let token: unknown = null;
      try {
        token = evalStatic(node, scope, 'strict');
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
        staticAssigns.push({field, value: table[token]});
      } else {
        dynamicAssigns.push({
          field,
          value: {
            kind: 'byte',
            value: lowerEnumValue(node, table, environment),
          },
        });
      }
    } else if (name === 'adjustable' || name === 'range') {
      const field = name === 'range' ? 'arc_range' : 'arc_adjustable';
      try {
        staticAssigns.push({
          field,
          value: evalStatic(node, scope, 'strict') ? 1 : 0,
        });
      } catch {
        dynamicAssigns.push({
          field,
          value: {
            kind: 'flag',
            test: lowerExpr(node, environment),
            truthiness: 'value',
            negate: false,
            whenTrue: 1,
          },
        });
      }
    } else if (name === 'knobImage') {
      knobImage = imageNameFromSource(node, environment);
      if (knobImage == null) {
        throw aotError(
          'AOT: <Dial knobImage> must resolve to a static asset name',
          "use an imported image (`import knob from './knob.png'` → knobImage={knob}) or a string asset name.",
        );
      }
      const path = environment.imageNames?.get(knobImage);
      if (path) {
        output.images.set(knobImage, path);
      }
    } else if (name === 'indicatorGradient') {
      // Allow indicatorGradient to be statically defined but conditionally enabled with a ternary against null/undefined.
      let gradientNode: t.Node = node;
      let gradientCondition: {test: IrExpr; negate: boolean} | null = null;
      if (node?.type === 'ConditionalExpression') {
        const nullish = (candidate: t.Node | null | undefined) =>
          candidate?.type === 'NullLiteral' ||
          (candidate?.type === 'Identifier' && candidate.name === 'undefined');
        if (nullish(node.alternate)) {
          gradientNode = node.consequent;
          gradientCondition = {
            test: lowerExpr(node.test, environment),
            negate: false,
          };
        } else if (nullish(node.consequent)) {
          gradientNode = node.alternate;
          gradientCondition = {
            test: lowerExpr(node.test, environment),
            negate: true,
          };
        }
      }

      // Validate and normalize the static gradient definition, keeping only the engine-supported stop count.
      const gradient = evalStaticOrThrow(
        gradientNode,
        scope,
        'AOT: <Dial indicatorGradient> must be a static object (optionally behind a ternary against null)',
        "indicatorGradient={{ type: 'conic', stops: [{ color: '#00f' }, { color: '#f00' }] }}, or " +
          'indicatorGradient={on ? {…} : null}',
      ) as
        | {type?: unknown; stops?: {color?: unknown; offset?: unknown}[]}
        | null
        | undefined;
      const allStops = Array.isArray(gradient?.stops) ? gradient!.stops! : [];
      check({kind: 'dialGradientStops', count: allStops.length});
      const stops = allStops.slice(0, DIAL_MAX_GRADIENT_STOPS);
      staticAssigns.push({
        field: 'gradient_type',
        value:
          gradient!.type === 'radial'
            ? 'ER_GRADIENT_RADIAL'
            : 'ER_GRADIENT_CONIC',
      });

      // Emit the stop count separately so a ternary can enable/disable the static gradient at runtime.
      if (gradientCondition) {
        dynamicAssigns.push({
          field: 'gradient_stop_count',
          value: {
            kind: 'flag',
            test: gradientCondition.test,
            truthiness: 'value',
            negate: gradientCondition.negate,
            whenTrue: stops.length,
          },
        });
      } else {
        staticAssigns.push({
          field: 'gradient_stop_count',
          value: stops.length,
        });
      }

      // Emit each fixed stop, filling in missing offsets with evenly spaced positions.
      stops.forEach((stop, index) => {
        const offset =
          typeof stop.offset === 'number'
            ? stop.offset
            : index / (stops.length - 1);
        staticAssigns.push({
          field: `gradient_stops[${index}].color`,
          value: parseColorValue(String(stop.color)),
        });
        staticAssigns.push({
          field: `gradient_stops[${index}].position`,
          value: floatConstant(offset),
        });
      });
    } else if (name === 'onChange') {
      onChangeFunction = node;
    } else {
      throw aotError(`AOT: <Dial> prop "${name}" is not supported`);
    }
  }

  // Animated bindings: style props plus an animated `value` (ER_PROP_ARC_VALUE).
  const node: IrNode = {
    id: nodeID,
    location: currentLoc(),
    type: 'ER_NODE_ARC',
    constantProps: staticAssigns,
    stateProps: dynamicAssigns,
    placeholder: null,
    text: null,
    image: knobImage != null ? {kind: 'static', name: knobImage} : null,
    spans: null,
    children: [],
    wiring: binds.map((bind, index) => ({kind: 'bind', bind, index})),
  };

  // Compile <Dial onChange> into an ER_EVENT_VALUE_CHANGE handler.
  if (onChangeFunction) {
    // A useCallback identifier resolves to its arrow, the same way the generic host-node event path does —
    // a dial's change handler is exactly the kind of thing an app wraps in useCallback.
    if (
      onChangeFunction.type === 'Identifier' &&
      environment.callbacks?.has(onChangeFunction.name)
    ) {
      onChangeFunction = environment.callbacks.get(onChangeFunction.name);
    }

    // Require a function node so lowerValueHandler can bind the dial's value parameters.
    if (!isFn(onChangeFunction)) {
      throw aotError(
        'AOT: <Dial onChange> must be an inline function or a useCallback',
        'onChange={(v) => setValue(v)}',
      );
    }

    const handlerName = `handler_${output.handlers.length}`;
    output.handlers.push({
      kind: 'callback',
      name: handlerName,
      callback: lowerValueHandler(
        onChangeFunction,
        environment,
        state,
        output,
        'float',
        false,
        {kind: 'event', field: 'value', type: FLOAT},
        {kind: 'event', field: 'value_start', type: FLOAT},
      ),
    });
    node.wiring.push({
      kind: 'event',
      event: 'ER_EVENT_VALUE_CHANGE',
      handler: handlerName,
    });
  }

  for (const slot of lowerRefBinds(element.openingElement, environment)) {
    node.wiring.push({kind: 'ref', slot});
  }

  checkInterpolations(node);
  node.children = emitChildren(
    element.children,
    scope,
    output,
    environment,
    state,
  );
  return node;
}
