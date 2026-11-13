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

import {attemptLowering, check} from './check.mts';
import {shapeOf} from './typing.mts';
import {
  BootReadError,
  bootEnvOf,
  bootSlotOf,
  foldBoot,
  isBoot,
} from './boot.mts';
import {
  lowerStyle,
  isStyleKey,
  STYLE_KEYS,
  DYN_FIELDS,
  parseColorValue,
} from '../style-map.mts';
import {parseColor} from '../../src/embedded-react/svg-ops.js';
import {aotError, checkEngineString} from './diagnostics.mts';
import {
  at,
  checkAnimatedStyleKey,
  checkAttributes,
  checkStyleKey,
  inlineFragments,
  TEXT_SPAN,
} from './elements/props.mts';
import {evalStatic, foldScope, withUndefined} from './static-eval.mts';
import {jsxChildText, lowerExpr, lowerText} from './expressions.mts';
import {
  ANIMATED_COLOR_KEYS,
  ANIMATED_STYLE_PROPS,
  ANIMATED_TRANSFORM_PROPS,
  parseInterp,
} from './animations.mts';
import type * as t from '@babel/types';
import type {BootValue} from './boot.mts';
import type {Interpolation} from './animations.mts';
import type {AotError} from './diagnostics.mts';
import type {IrPropValue} from './ir/nodes.mts';
import type {IrText, IrTextPart} from './ir/text.mts';
import type {ElementSpec} from './elements/props.mts';
import type {
  DynAssign,
  EngineName,
  Env,
  PropConstant,
  Scope,
  StaticAssign,
} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** An animated value bound to a node property on the engine's native driver, optionally through a mapping. */
export interface AnimBind {
  /** The animated value's handle. */
  slotName: string;
  /** The ER_PROP_* it drives. */
  prop: string;
  interpolation?: Interpolation;
}

/** An element's merged style: constant field writes, state-driven ones, and animated-value binds. */
export interface StyleAssigns {
  staticAssigns: StaticAssign[];
  dynamicAssigns: DynAssign[];
  binds: AnimBind[];
}

/** One inline segment of a multi-span <Text>: its text, and the constant for each ERTextSpan field. */
export interface TextSpan {
  text: string;
  color: PropConstant;
  font_size: PropConstant;
  font_weight: PropConstant;
  font_style: PropConstant;
  text_decoration: PropConstant;
  letter_spacing: PropConstant;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** Pixel style keys state cannot drive but a value worked out at boot still sets, with their ERProps fields. */
const BOOT_PIXEL_FIELDS: Record<string, string> = {
  left: 'left',
  top: 'top',
  right: 'right',
  bottom: 'bottom',
  flexBasis: 'flex_basis',
};

/** A span's weight, style or decoration that takes its parent's. */
const SPAN_INHERIT = 0xff;

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * A JSX attribute's value as an expression: a bare `name` is `true`, `name="x"` the string, `name={expr}` the
 * expression.
 *
 * @param attribute  The attribute.
 *
 * @returns Its value.
 */
export function attrExpr(
  attribute: t.JSXAttribute,
): t.Expression | t.JSXEmptyExpression {
  const attributeValue = attribute.value;
  if (!attributeValue) return {type: 'BooleanLiteral', value: true};
  if (attributeValue.type === 'StringLiteral') return attributeValue;
  if (attributeValue.type === 'JSXExpressionContainer')
    return attributeValue.expression;

  return attributeValue;
}

/**
 * A static ARGB8888 value from a CSS color string or number.
 *
 * @param value  The color.
 *
 * @returns Its value.
 */
export const argbValue = (value: unknown): number =>
  parseColor(String(value)) >>> 0;

/**
 * Refuses an animated binding whose values do not fit the key: a color key takes only an interpolation onto
 * colors, and every other key only numbers.
 *
 * @param key  The style or transform key.
 * @param interpolation  The binding's interpolation, or undefined for the value itself.
 * @param node  The node to pin an error to.
 */
function checkAnimatedColor(
  key: string,
  interpolation: Interpolation | undefined,
  node: t.Node,
): void {
  if (ANIMATED_COLOR_KEYS.has(key) && !interpolation?.colors) {
    throw at(
      aotError(
        `AOT: an animated "${key}" needs colors`,
        "bind value.interpolate({inputRange: [0, 1], outputRange: ['#000', '#fff']}) rather than a number.",
      ),
      node,
    );
  }
  if (!ANIMATED_COLOR_KEYS.has(key) && interpolation?.colors) {
    throw at(
      aotError(
        `AOT: an animated "${key}" takes numbers, not colors`,
        'an outputRange of colors drives backgroundColor or color.',
      ),
      node,
    );
  }
}

/**
 * Lowers a dynamic (state-referencing) color expression to an ARGB value: a color, or a ternary of them.
 *
 * @param node  The color expression.
 * @param environment  The expression environment.
 *
 * @returns The value.
 */
export function lowerColorValue(node: t.Node, environment: Env): IrPropValue {
  if (node.type === 'StringLiteral') {
    return {kind: 'constant', value: parseColorValue(node.value)};
  }

  if (node.type === 'ConditionalExpression') {
    return {
      kind: 'select',
      test: lowerExpr(node.test, environment),
      whenTrue: lowerColorValue(node.consequent, environment),
      whenFalse: lowerColorValue(node.alternate, environment),
    };
  }

  // A const worked out at boot picks its color at boot, from its own expression.
  const bootAlias = bootAliasOf(node, environment);
  if (bootAlias) {
    const picked = tryLowering(() =>
      lowerColorValue(bootAlias.node, bootAlias.environment),
    );
    if (picked) return picked;
  }

  // A statically resolvable color (a const string, or a theme token like `theme.card`) folds to a literal.
  try {
    const color = evalStatic(
      node,
      foldScope(environment, environment.constants ?? {}),
    );
    if (typeof color === 'string') {
      return {kind: 'constant', value: parseColorValue(color)};
    }
  } catch {
    /* not static — fall through to the error below */
  }

  throw new Error(
    'AOT: a dynamic color must be a color string literal or a ternary of them',
  );
}

/**
 * Lowers a dynamic enum-style expression (e.g. `flexDirection: row ? 'row' : 'column'`) to its ER_* constant
 * (or a ternary of them), looking values up in the style key's enum `table`.
 *
 * @param node  The enum expression.
 * @param table  The RN value → ER_* constant table.
 * @param environment  The expression environment.
 *
 * @returns The value.
 */
export function lowerEnumValue(
  node: t.Node,
  table: Record<string, EngineName>,
  environment: Env,
): IrPropValue {
  if (node.type === 'StringLiteral') {
    const enumConstant = Object.hasOwn(table, node.value)
      ? table[node.value]
      : undefined;
    if (!enumConstant) {
      throw aotError(
        `AOT: unsupported enum value "${node.value}"`,
        `one of: ${Object.keys(table).join(', ')}`,
      );
    }

    return {kind: 'constant', value: enumConstant};
  }
  if (node.type === 'ConditionalExpression') {
    return {
      kind: 'select',
      test: lowerExpr(node.test, environment),
      whenTrue: lowerEnumValue(node.consequent, table, environment),
      whenFalse: lowerEnumValue(node.alternate, table, environment),
    };
  }
  // A const worked out at boot picks its value at boot, from its own expression.
  const bootAlias = bootAliasOf(node, environment);
  if (bootAlias) {
    const picked = tryLowering(() =>
      lowerEnumValue(bootAlias.node, table, bootAlias.environment),
    );
    if (picked) return picked;
  }

  // A statically resolvable enum (a const string) folds to its constant.
  try {
    const enumValue = evalStatic(
      node,
      foldScope(environment, environment.constants ?? {}),
    );
    if (typeof enumValue === 'string' && Object.hasOwn(table, enumValue)) {
      return {kind: 'constant', value: table[enumValue]};
    }
  } catch {
    /* not static — fall through */
  }

  throw aotError(
    'AOT: a state-driven enum style must be a string literal or a ternary of them',
    "e.g. flexDirection: wide ? 'row' : 'column'",
  );
}

/**
 * The diagnostic for a style key the AOT has no lowering for, static or dynamic.
 *
 * @param key  The style key.
 *
 * @returns The error, for the caller to throw.
 */
const unknownStyleKey = (key: string): AotError =>
  aotError(
    `AOT: style "${key}" is not supported by the AOT (no ERProps lowering)`,
    `Flow A may accept it; Flow B lowers these: ${STYLE_KEYS.join(', ')}.`,
  );

/**
 * Lowers one STATIC style key/value, telling the two failures apart: a key the AOT has no lowering for
 * at all, and a known key whose value it rejects. Neither is reported as a state-driven value.
 *
 * @param key  The style key.
 * @param value  Its folded value.
 *
 * @returns The ERProps writes; none for an undefined or null value.
 */
function lowerStyleChecked(key: string, value: unknown): StaticAssign[] {
  // A null or undefined value is a no-op, as in Flow A, so the key is judged only when there is a value to lower.
  if (value === undefined || value === null) return [];

  if (!isStyleKey(key)) {
    throw unknownStyleKey(key);
  }

  try {
    return lowerStyle({[key]: value});
  } catch (error) {
    // style-map's own value errors already name the key in some cases — don't say it twice.
    const message = String((error as Error).message);
    const reason = message.startsWith(`${key}: `)
      ? message.slice(key.length + 2)
      : message;
    throw aotError(`AOT: unsupported value for style "${key}": ${reason}`);
  }
}

/**
 * Lowers one dynamic inline-style value to ERProps field assignment(s).
 *
 * @param key  The style key.
 * @param valueNode  Its state-driven value.
 * @param environment  The expression environment.
 *
 * @returns The ERProps writes.
 */
function lowerDynamicStyleValue(
  key: string,
  valueNode: t.Node,
  environment: Env,
): DynAssign[] {
  // Own properties only, so a key like `toString` cannot resolve to Object.prototype and emit `p.undefined`.
  const dynamicField = Object.hasOwn(DYN_FIELDS, key)
    ? DYN_FIELDS[key]
    : undefined;
  // Report an unknown key as such; advising "make it static" would only lead to the unknown-key error next.
  if (!dynamicField && !isStyleKey(key)) {
    throw unknownStyleKey(key);
  }

  if (!dynamicField) {
    throw aotError(
      `AOT: a state-driven value for style "${key}" is not supported (static only)`,
      `state-driven styles supported: colors, opacity, sizes/margins/padding, and the ` +
        `layout enums (flexDirection, alignItems, alignSelf, justifyContent, position, ` +
        `display). Make "${key}" static, or drive the change another way.`,
    );
  }

  // Colors and enums lower to a constant or a choice of constants.
  if (dynamicField.kind === 'color') {
    return [
      {
        field: dynamicField.field,
        value: lowerColorValue(valueNode, environment),
      },
    ];
  }

  if (dynamicField.kind === 'enum') {
    return [
      {
        field: dynamicField.field,
        value: lowerEnumValue(valueNode, dynamicField.table, environment),
      },
    ];
  }

  // Opacity and every size are numbers.
  const valueExpression = lowerExpr(valueNode, environment);
  if (shapeOf(valueExpression) === 'string') {
    const error = aotError(
      `AOT: style "${key}" needs a number, but the value is a string`,
      "JS would coerce the string; the AOT doesn't. Keep the state numeric — useState(10), not useState('10').",
    );
    if (valueNode.loc) {
      error.aotLoc = valueNode.loc.start;
    }

    throw error;
  }
  if (dynamicField.kind === 'opacity') {
    return [
      {
        field: dynamicField.field,
        value: {kind: 'opacity', value: valueExpression},
      },
    ];
  }

  // A byte field takes a float with its fraction dropped, where a dimension would round it.
  if (dynamicField.kind === 'byte') {
    return [
      {
        field: dynamicField.field,
        value: {kind: 'float', value: valueExpression},
      },
    ];
  }

  return [
    {
      field: dynamicField.field,
      value: {kind: 'dimension', value: valueExpression},
    },
  ];
}

/**
 * The expression and environment of a value worked out at boot that `node` reads as a whole (a const, or an
 * entry of one), for a lowering that picks between literals.
 *
 * @param node  The expression.
 * @param environment  The expression environment.
 *
 * @returns The value's own expression and the environment it lowers in, or null.
 */
function bootAliasOf(
  node: t.Node,
  environment: Env,
): {node: t.Expression; environment: Env} | null {
  let value: unknown;
  try {
    value = evalStatic(
      node,
      foldScope(environment, environment.constants ?? {}),
      'boot',
    );
  } catch {
    return null;
  }

  if (!isBoot(value) || !value.node || value.node === node) return null;
  const bootEnvironment = bootEnvOf(value);
  return bootEnvironment && {node: value.node, environment: bootEnvironment};
}

/**
 * A style entry worked out at boot, as the runtime write of its field: a size or an opacity. Null for a value
 * boot cannot work out, one that is no number, or a key that takes no runtime number, which then fold.
 *
 * @param key  The style key.
 * @param value  The entry.
 *
 * @returns The write, or null.
 */
export function bootStyleAssign(
  key: string,
  value: BootValue,
): DynAssign | null {
  const dynamicField = Object.hasOwn(DYN_FIELDS, key)
    ? DYN_FIELDS[key]
    : undefined;
  const pixelField = Object.hasOwn(BOOT_PIXEL_FIELDS, key)
    ? BOOT_PIXEL_FIELDS[key]
    : undefined;
  // A color or an enum lowers its own expression (a ternary of literals) against the names it reads.
  if (dynamicField?.kind === 'color' || dynamicField?.kind === 'enum') {
    const environment = bootEnvOf(value);
    const valueNode = value.node;
    if (!environment || !valueNode) return null;
    const propValue = tryLowering(() =>
      dynamicField.kind === 'enum'
        ? lowerEnumValue(valueNode, dynamicField.table, environment)
        : lowerColorValue(valueNode, environment),
    );
    return propValue && {field: dynamicField.field, value: propValue};
  }

  const isNumeric =
    pixelField ||
    dynamicField?.kind === 'opacity' ||
    dynamicField?.kind === 'byte' ||
    dynamicField?.kind === 'num';
  if (!isNumeric) return null;
  value.name ??= key;

  const slot = bootSlotOf(value);
  if (!slot || (slot.type.kind !== 'int' && slot.type.kind !== 'float')) {
    return null;
  }

  return {
    field: pixelField ?? dynamicField!.field,
    value: {
      kind:
        dynamicField?.kind === 'opacity'
          ? 'opacity'
          : dynamicField?.kind === 'byte'
            ? 'float'
            : 'dimension',
      value: {kind: 'boot', slot, type: slot.type},
    },
  };
}

/**
 * Lowers a style value worked out at boot: as a state-driven one, or, for an inset or flexBasis, which state
 * cannot drive, as the pixel size it is.
 *
 * @param key  The style key.
 * @param valueNode  The value expression.
 * @param environment  The expression environment.
 *
 * @returns The runtime writes.
 */
function lowerBootStyleValue(
  key: string,
  valueNode: t.Node,
  environment: Env,
): DynAssign[] {
  if (!Object.hasOwn(BOOT_PIXEL_FIELDS, key)) {
    return lowerDynamicStyleValue(key, valueNode, environment);
  }

  const value = lowerExpr(valueNode, environment);
  if (shapeOf(value) !== 'number') {
    throw new Error(`AOT: style "${key}" needs a number`);
  }

  return [{field: BOOT_PIXEL_FIELDS[key], value: {kind: 'dimension', value}}];
}

/**
 * Runs a lowering whose failure picks a fallback.
 *
 * @param attempt  The lowering.
 *
 * @returns What it returns, or null when it throws.
 */
function tryLowering<Result>(attempt: () => Result): Result | null {
  try {
    return attemptLowering(attempt);
  } catch {
    return null;
  }
}

/**
 * Whether a style already writes `field`, counting its percentage twin: `width: '50%'` lowers to
 * `width_percent`, and the author has still set the width. Used by the components that fall back to a
 * built-in size or inset when the style leaves one out — without the twin they inject a pixel default
 * ALONGSIDE the percentage, and the engine prefers the pixel value for a size.
 *
 * @param staticAssigns  Static field writes from collectStyleAssigns().
 * @param dynamicAssigns  State-driven field writes from collectStyleAssigns().
 * @param field  ERProps field to look for.
 *
 * @returns Whether either list writes the field or its `_percent` twin.
 */
export const styleWrites = (
  staticAssigns: {field: string}[],
  dynamicAssigns: {field: string}[],
  field: string,
): boolean =>
  [field, `${field}_percent`].some(
    fieldName =>
      staticAssigns.some(assign => assign.field === fieldName) ||
      dynamicAssigns.some(assign => assign.field === fieldName),
  );

/**
 * Collects an element's merged style into static field assigns and dynamic (state-driven) field assigns.
 * Inline object values are tried statically first; a value that references state becomes a dynAssign.
 * Later style sources override earlier ones per field (RN merge), kept in `fields` by ERProps field. A key the
 * element's node neither lays out nor draws with is refused.
 *
 * @param openingElement  The element's opening tag.
 * @param scope  Compile-time constants in reach.
 * @param environment  The expression environment.
 * @param spec  The element, for the style keys it takes.
 * @param bootValues  How a value worked out at boot is set: once at build, or taken at the build's panel.
 *
 * @returns The style's constant writes, state-driven writes, and animated-value binds.
 */
export function collectStyleAssigns(
  openingElement: t.JSXOpeningElement,
  scope: Scope,
  environment: Env,
  spec: ElementSpec,
  bootValues: 'boot' | 'fold' = 'boot',
): StyleAssigns {
  // ERProps field → its last write: a constant, or a state-driven value.
  const fields = new Map<
    string,
    {constant: PropConstant} | {value: IrPropValue}
  >();
  const binds: AnimBind[] = []; // animated values bound to node properties (native driver)

  // A size in pixels and its percentage twin are one style key, so a later write of either replaces both.
  const setField = (
    field: string,
    write: {constant: PropConstant} | {value: IrPropValue},
  ): void => {
    fields.delete(
      field.endsWith('_percent')
        ? field.slice(0, -'_percent'.length)
        : `${field}_percent`,
    );
    fields.set(field, write);
  };

  // The handle of an animated value used directly as a style value, or null.
  const animRef = (node: t.Node | null | undefined): string | null =>
    node?.type === 'Identifier' && environment.animations?.has(node.name)
      ? environment.animations.get(node.name)!.slotName
      : null;
  // `<animValue>.interpolate({ inputRange, outputRange, extrapolate })` → { slotName, interp } for a mapped bind.
  const animInterpRef = (
    node: t.Node | null | undefined,
  ): {slotName: string; interpolation: Interpolation} | null => {
    if (
      node?.type === 'CallExpression' &&
      node.callee.type === 'MemberExpression' &&
      !node.callee.computed &&
      (node.callee.property as t.Identifier).name === 'interpolate' &&
      node.callee.object.type === 'Identifier' &&
      environment.animations?.has(node.callee.object.name)
    ) {
      return {
        slotName: environment.animations.get(node.callee.object.name)!.slotName,
        interpolation: parseInterp(node.arguments[0], environment),
      };
    }

    return null;
  };

  // A transform array's binds, or null when no entry is animated. Flow B has no static transform, so once one
  // entry binds an animated value, every other entry must too; one left behind would be dropped.
  const animatedTransform = (array: t.ArrayExpression): AnimBind[] | null => {
    const transformBinds: AnimBind[] = [];
    const leftOver: t.Node[] = [];
    for (const transformEntry of array.elements) {
      if (transformEntry?.type !== 'ObjectExpression') {
        leftOver.push(transformEntry ?? array);
        continue;
      }

      for (const transformProp of transformEntry.properties) {
        if (transformProp.type !== 'ObjectProperty') {
          leftOver.push(transformProp);
          continue;
        }

        const transformKey =
          (transformProp.key as t.Identifier).name ??
          (transformProp.key as t.StringLiteral).value;
        const direct = animRef(transformProp.value);
        const mapped = direct ? null : animInterpRef(transformProp.value);
        if (!direct && !mapped) {
          leftOver.push(transformProp);
          continue;
        }

        // An animated entry the engine has no property for is refused, not skipped.
        if (!Object.hasOwn(ANIMATED_TRANSFORM_PROPS, transformKey)) {
          throw at(
            aotError(
              `AOT: transform "${transformKey}" cannot be animated in Flow B`,
              `animated transforms: ${Object.keys(ANIMATED_TRANSFORM_PROPS).join(', ')}.`,
            ),
            transformProp,
          );
        }
        checkAnimatedColor(transformKey, mapped?.interpolation, transformProp);

        for (const animationProp of ANIMATED_TRANSFORM_PROPS[transformKey]) {
          transformBinds.push(
            mapped
              ? {
                  slotName: mapped.slotName,
                  prop: animationProp,
                  interpolation: mapped.interpolation,
                }
              : {slotName: direct!, prop: animationProp},
          );
        }
      }
    }

    if (!transformBinds.length) return null;
    if (leftOver.length) {
      throw at(
        aotError(
          'AOT: a transform entry that is not an animated value cannot sit beside an animated one',
          'Flow B lowers a transform only as animated values bound on the native driver, so this entry ' +
            'would be dropped. Drive it from an animated value too (useAnimatedValue(45) holds a fixed one).',
        ),
        leftOver[0],
      );
    }

    return transformBinds;
  };

  // Lowers one style source (an array, an inline object, or a static reference) into `fields` and `binds`.
  const applyStyle = (expression: t.Node): void => {
    // RN ignores an undefined style or style-array entry; so does Flow A's flattenStyle.
    if (expression.type === 'Identifier' && expression.name === 'undefined')
      return;

    // A style array applies its entries in order, so later entries win.
    if (expression.type === 'ArrayExpression') {
      for (const styleEntry of expression.elements) {
        if (styleEntry) {
          applyStyle(styleEntry);
        }
      }

      return;
    }

    // Inline object: bind animated values natively; fold other keys statically or re-apply them in the update.
    if (expression.type === 'ObjectExpression') {
      for (const prop of expression.properties) {
        if (prop.type !== 'ObjectProperty') {
          throw new Error(
            'AOT: spread/method in an inline style object not supported',
          );
        }

        const key = (
          prop.computed
            ? evalStatic(prop.key, scope)
            : ((prop.key as t.Identifier).name ??
              (prop.key as t.StringLiteral).value)
        ) as string;

        // `{transform: undefined}` is a no-op in RN and Flow A.
        if (
          prop.value.type === 'Identifier' &&
          prop.value.name === 'undefined'
        ) {
          continue;
        }

        // Bind an animated value to its prop (opacity / backgroundColor / color), directly or via .interpolate().
        const animationValue = animRef(prop.value);
        if (animationValue && ANIMATED_STYLE_PROPS[key]) {
          checkAnimatedStyleKey(spec, key, prop);
          checkAnimatedColor(key, undefined, prop);
          for (const animationProp of ANIMATED_STYLE_PROPS[key]) {
            binds.push({slotName: animationValue, prop: animationProp});
          }
          continue;
        }

        const animationInterpolation = animInterpRef(prop.value);
        if (animationInterpolation && ANIMATED_STYLE_PROPS[key]) {
          checkAnimatedStyleKey(spec, key, prop);
          checkAnimatedColor(key, animationInterpolation.interpolation, prop);
          for (const animationProp of ANIMATED_STYLE_PROPS[key]) {
            binds.push({
              slotName: animationInterpolation.slotName,
              prop: animationProp,
              interpolation: animationInterpolation.interpolation,
            });
          }

          continue;
        }

        // transform: [{ scale: <anim> }, { translateX: <anim>.interpolate(...) }, ...] — bind each entry.
        if (key === 'transform' && prop.value.type === 'ArrayExpression') {
          const transformBinds = animatedTransform(prop.value);
          if (transformBinds) {
            checkAnimatedStyleKey(spec, key, prop);
            binds.push(...transformBinds);
            continue;
          }
        }

        // Only a failed fold falls back to the dynamic path, so an unknown key is not misreported as state-driven.
        let staticValue: {value: unknown} | null;
        let readsBoot = false;
        try {
          staticValue = {
            value: evalStatic(
              prop.value,
              withUndefined(scope),
              bootValues === 'fold' ? 'fold' : 'strict',
            ),
          };
        } catch (error) {
          staticValue = null; // references state — lower it as a dynamic value
          readsBoot = error instanceof BootReadError;
        }

        // A value worked out at boot is set once at build; a key that takes no runtime value folds it instead.
        if (readsBoot) {
          const bootAssigns = tryLowering(() =>
            lowerBootStyleValue(key, prop.value, environment),
          );
          if (bootAssigns) {
            checkStyleKey(spec, key, prop);
            for (const assign of bootAssigns) {
              setField(assign.field, {value: assign.value});
            }
            continue;
          }

          staticValue = {value: evalStatic(prop.value, withUndefined(scope))};
        }

        if (staticValue) {
          if (staticValue.value != null) {
            checkStyleKey(spec, key, prop);
          }

          for (const assign of lowerStyleChecked(key, staticValue.value)) {
            setField(assign.field, {constant: assign.value});
          }
        } else {
          checkStyleKey(spec, key, prop);
          for (const assign of lowerDynamicStyleValue(
            key,
            prop.value,
            environment,
          )) {
            setField(assign.field, {value: assign.value});
          }
        }
      }

      return;
    }

    // A static style reference; null, undefined or false (`cond && s`) means no style, as in RN.
    let resolved = evalStatic(expression, withUndefined(scope), 'boot');
    if (isBoot(resolved)) {
      resolved = foldBoot(resolved);
    }

    if (resolved === null || resolved === undefined || resolved === false) {
      return;
    }

    for (const [styleKey, entry] of Object.entries(resolved as object)) {
      // An entry worked out at boot is set once at build, if its key takes a runtime value.
      const bootAssign =
        bootValues === 'boot' && isBoot(entry)
          ? bootStyleAssign(styleKey, entry)
          : null;
      if (bootAssign) {
        checkStyleKey(spec, styleKey, expression);
        setField(bootAssign.field, {value: bootAssign.value});
        continue;
      }

      const styleValue = foldBoot(entry);
      if (styleValue === undefined || styleValue === null) continue;

      checkStyleKey(spec, styleKey, expression);
      for (const assign of lowerStyleChecked(styleKey, styleValue)) {
        setField(assign.field, {constant: assign.value});
      }
    }
  };

  // Apply every style attribute in source order.
  for (const attribute of openingElement.attributes) {
    if (
      attribute.type !== 'JSXAttribute' ||
      (attribute.name as t.JSXIdentifier).name !== 'style'
    ) {
      continue;
    }

    applyStyle(attrExpr(attribute));
  }

  // Split the merged fields into constant writes and state-driven writes.
  const staticAssigns: StaticAssign[] = [];
  const dynamicAssigns: DynAssign[] = [];
  for (const [field, fieldWrite] of fields) {
    if ('value' in fieldWrite) {
      dynamicAssigns.push({field, value: fieldWrite.value});
    } else {
      staticAssigns.push({field, value: fieldWrite.constant});
    }
  }

  return {staticAssigns, dynamicAssigns, binds};
}

/**
 * Lowers a Text node's body. Static interpolations fold into literal text; any that reference state are values
 * set again on every update.
 *
 * @param children  The <Text>'s children.
 * @param scope  Compile-time constants in reach.
 * @param environment  The expression environment.
 *
 * @returns The body.
 */
export function lowerNodeText(
  children: t.JSXElement['children'],
  scope: Scope,
  environment: Env,
): IrText {
  const parts: IrTextPart[] = [];
  for (const child of inlineFragments(children)) {
    if (child.type === 'JSXText') {
      parts.push({kind: 'literal', text: cleanJsxText(child.value)});
    } else if (child.type === 'JSXExpressionContainer') {
      if (child.expression.type === 'JSXEmptyExpression') continue;
      // Constants fold into literal text; anything referencing state is a runtime value.
      parts.push(...lowerText(child.expression, environment, scope).parts);
    } else if (child.type === 'JSXElement') {
      throw new Error(
        'AOT: nested <Text> / element children inside <Text> not yet supported (spans)',
      );
    } else {
      throw unsupportedTextChild(child);
    }
  }

  return {parts};
}

/**
 * The error for a <Text> child that is neither text, a `{…}` value nor a nested <Text>.
 *
 * @param child  The child.
 *
 * @returns The error, for the caller to throw.
 */
const unsupportedTextChild = (child: t.Node): AotError =>
  at(
    aotError(
      `AOT: a ${child.type} cannot be part of a <Text>`,
      'a <Text> holds text, {values} and nested <Text> spans.',
    ),
    child,
  );

/**
 * Normalizes JSX text the way Babel does: trim per-line, drop blank lines, join with single spaces; a
 * same-line leading/trailing space is preserved (so `Hello <b>x</b>` keeps the space before the span).
 *
 * @param value  The JSXText's raw value.
 *
 * @returns The text as React would render it.
 */
export function cleanJsxText(value: string): string {
  // Find the last line with content: it is the only one not followed by a joining space.
  const lines = value.split(/\r\n|\n|\r/);
  let lastContentLine = 0;
  for (let lineIndex = 0; lineIndex < lines.length; lineIndex++) {
    if (/[^ \t]/.test(lines[lineIndex])) {
      lastContentLine = lineIndex;
    }
  }

  // Trim each line where it meets a line break, drop blank lines, and join the rest with single spaces.
  let cleaned = '';
  for (let lineIndex = 0; lineIndex < lines.length; lineIndex++) {
    let line = lines[lineIndex].replace(/\t/g, ' ');
    if (lineIndex !== 0) {
      line = line.replace(/^ +/, '');
    }

    if (lineIndex !== lines.length - 1) {
      line = line.replace(/ +$/, '');
    }

    if (line) {
      cleaned += lineIndex !== lastContentLine ? line + ' ' : line;
    }
  }

  return cleaned;
}

/**
 * Concatenates a (span) <Text>'s static text content (literal + folded {expr}); a nested element throws.
 *
 * @param children  The span's children.
 * @param scope  Compile-time constants in reach.
 *
 * @returns The span's text.
 */
function staticTextContent(
  children: t.JSXElement['children'],
  scope: Scope,
): string {
  let text = '';
  for (const child of inlineFragments(children)) {
    if (child.type === 'JSXText') {
      text += cleanJsxText(child.value);
    } else if (
      child.type === 'JSXExpressionContainer' &&
      child.expression.type !== 'JSXEmptyExpression'
    ) {
      const value = evalStatic(child.expression, scope); // throws if it references state
      text += jsxChildText(value);
    } else if (child.type === 'JSXElement') {
      throw aotError(
        'AOT: a nested <Text> span may not itself contain another <Text> (one level of spans only)',
      );
    } else if (child.type === 'JSXSpreadChild') {
      throw unsupportedTextChild(child);
    }
  }

  return text;
}

/**
 * If a <Text>'s children include a nested <Text>, returns inline SPANS [{text, color, font_size,
 * font_weight, font_style, text_decoration, letter_spacing}] (constants; inherit sentinels for unset).
 * Returns null when there's no nested <Text> (caller uses the single-string buildText path). Static only —
 * a dynamic {…} segment or a state-driven span style throws.
 *
 * @param children  The <Text>'s children.
 * @param scope  Compile-time constants in reach.
 * @param environment  The expression environment.
 *
 * @returns The spans, or null when the <Text> has no nested <Text>.
 */
export function collectTextSpans(
  children: t.JSXElement['children'],
  scope: Scope,
  environment: Env,
): TextSpan[] | null {
  // Without a nested <Text>, the caller uses the single-string buildText path.
  children = inlineFragments(children);
  if (
    !children.some(
      child =>
        child.type === 'JSXElement' &&
        (child.openingElement.name as t.JSXIdentifier).name === 'Text',
    )
  ) {
    return null;
  }

  // Inherit sentinels (see ERTextSpan doc): color 0, font_size 0, weight/style/decoration 0xFF, spacing AUTO.
  const inheritSpan = (text: string): TextSpan => ({
    text,
    color: 0,
    font_size: 0,
    font_weight: SPAN_INHERIT,
    font_style: SPAN_INHERIT,
    text_decoration: SPAN_INHERIT,
    letter_spacing: 'ER_LAYOUT_AUTO',
  });

  // A span's text goes in a fixed engine buffer.
  const spanText = (text: string): string => {
    checkEngineString(text, 'ER_SPAN_TEXT_MAX', 'a <Text> span');
    return text;
  };

  // One span per child: text and static {…} values inherit the parent's style; a nested <Text> carries its own.
  const spans = [];
  for (const child of children) {
    if (child.type === 'JSXText') {
      const text = cleanJsxText(child.value);
      if (text) {
        spans.push(inheritSpan(spanText(text)));
      }
    } else if (child.type === 'JSXExpressionContainer') {
      if (child.expression.type === 'JSXEmptyExpression') continue;

      let value;
      try {
        value = evalStatic(child.expression, scope);
      } catch {
        throw aotError(
          'AOT: a dynamic {…} segment inside a multi-span <Text> is not supported',
          'spans must be static; keep dynamic text in its own single <Text> (no nested <Text> siblings).',
        );
      }

      // React draws nothing for null, undefined, or a boolean child.
      if (value !== undefined && value !== null && typeof value !== 'boolean') {
        spans.push(inheritSpan(spanText(String(value))));
      }
    } else if (
      child.type === 'JSXElement' &&
      (child.openingElement.name as t.JSXIdentifier).name === 'Text'
    ) {
      checkAttributes(TEXT_SPAN, child.openingElement);
      // A span is a static table, so a value worked out at boot is taken at the build's panel instead.
      const {staticAssigns, dynamicAssigns} = collectStyleAssigns(
        child.openingElement,
        scope,
        environment,
        TEXT_SPAN,
        'fold',
      );
      if (dynamicAssigns.length) {
        throw aotError(
          'AOT: a state-driven style on a nested <Text> span is not supported',
          'give the span <Text> a static style.',
        );
      }

      // Each span field is the nested <Text>'s own static style value, or the inherit sentinel.
      const spanStyle = (
        fieldName: string,
        fallback: PropConstant,
      ): PropConstant =>
        staticAssigns.find(assign => assign.field === fieldName)?.value ??
        fallback;
      spans.push({
        text: spanText(staticTextContent(child.children, scope)),
        color: spanStyle('color', 0),
        font_size: spanStyle('font_size', 0),
        font_weight: spanStyle('font_weight', SPAN_INHERIT),
        font_style: spanStyle('font_style', SPAN_INHERIT),
        text_decoration: spanStyle('text_decoration', SPAN_INHERIT),
        letter_spacing: spanStyle('letter_spacing', 'ER_LAYOUT_AUTO'),
      });
    } else {
      throw aotError(
        'AOT: unsupported child inside a multi-span <Text>',
        'a <Text> with a nested <Text> may contain text, {static expressions}, and nested <Text> only.',
      );
    }
  }

  // The engine renders at most ER_TEXT_MAX_SPANS segments; the check pass refuses to silently drop the rest.
  check({
    kind: 'textSpans',
    count: spans.length,
    max: environment.caps.maxTextSpans,
  });

  return spans;
}
