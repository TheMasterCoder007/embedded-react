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

import {check} from './check.mts';
import {
  lowerStyle,
  isStyleKey,
  STYLE_KEYS,
  DYN_FIELDS,
  colorLiteral,
} from '../style-map.mts';
import {parseColor} from '../../src/embedded-react/svg-ops.js';
import {aotError} from './diagnostics.mts';
import {
  at,
  checkAnimatedStyleKey,
  checkAttributes,
  checkStyleKey,
  inlineFragments,
  TEXT_SPAN,
} from './elements/props.mts';
import {evalStatic, foldScope, withUndefined} from './static-eval.mts';
import {cstr} from './c-syntax.mts';
import {jsxChildText, lowerExpr, lowerText} from './expressions.mts';
import {printPropValue} from './c/nodes.mts';
import {
  ANIM_STYLE_PROPS,
  ANIM_TRANSFORM_PROPS,
  parseInterp,
} from './animations.mts';
import type * as t from '@babel/types';
import type {Interpolation} from './animations.mts';
import type {AotError} from './diagnostics.mts';
import type {IrPropValue} from './ir/nodes.mts';
import type {IrText, IrTextPart} from './ir/text.mts';
import type {ElementSpec} from './elements/props.mts';
import type {DynAssign, Env, Scope, StaticAssign} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** An animated value bound to a node property on the engine's native driver, optionally through a mapping. */
export interface AnimBind {
  /** The animated value's C handle. */
  cVar: string;
  /** The ER_PROP_* it drives. */
  prop: string;
  interp?: Interpolation;
}

/** An element's merged style: constant field writes, state-driven ones, and animated-value binds. */
export interface StyleAssigns {
  staticAssigns: StaticAssign[];
  dynAssigns: DynAssign[];
  binds: AnimBind[];
}

/** One inline segment of a multi-span <Text>: C expressions for each ERTextSpan field. */
export interface TextSpan {
  text: string;
  color: string;
  font_size: string;
  font_weight: string;
  font_style: string;
  text_decoration: string;
  letter_spacing: string;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * A JSX attribute's value as an expression: a bare `name` is `true`, `name="x"` the string, `name={expr}` the
 * expression.
 *
 * @param attr  The attribute.
 *
 * @returns Its value.
 */
export function attrExpr(
  attr: t.JSXAttribute,
): t.Expression | t.JSXEmptyExpression {
  const attrValue = attr.value;
  if (!attrValue) return {type: 'BooleanLiteral', value: true};
  if (attrValue.type === 'StringLiteral') return attrValue;
  if (attrValue.type === 'JSXExpressionContainer') return attrValue.expression;

  return attrValue;
}

/**
 * A static ARGB8888 C literal (`0xAARRGGBBu`) from a CSS color string or number.
 *
 * @param value  The color.
 *
 * @returns Its C literal.
 */
export function argbLiteral(value: unknown): string {
  return (
    '0x' +
    (parseColor(String(value)) >>> 0)
      .toString(16)
      .padStart(8, '0')
      .toUpperCase() +
    'u'
  );
}

/**
 * Lowers a dynamic (state-referencing) color expression to an ARGB value: a color, or a ternary of them.
 *
 * @param node  The color expression.
 * @param env  The expression environment.
 *
 * @returns The value.
 */
export function lowerColorValue(node: t.Node, env: Env): IrPropValue {
  if (node.type === 'StringLiteral') {
    return {kind: 'constant', code: colorLiteral(node.value)};
  }

  if (node.type === 'ConditionalExpression') {
    return {
      kind: 'select',
      test: lowerExpr(node.test, env),
      whenTrue: lowerColorValue(node.consequent, env),
      whenFalse: lowerColorValue(node.alternate, env),
    };
  }

  // A statically resolvable color (a const string, or a theme token like `theme.card`) folds to a literal.
  try {
    const color = evalStatic(node, foldScope(env, env.consts ?? {}));
    if (typeof color === 'string') {
      return {kind: 'constant', code: colorLiteral(color)};
    }
  } catch {
    /* not static — fall through to the error below */
  }

  throw new Error(
    'AOT: a dynamic color must be a color string literal or a ternary of them',
  );
}

/**
 * A dynamic color expression as C for its ARGB value (see lowerColorValue).
 *
 * @param node  The color expression.
 * @param env  The expression environment.
 *
 * @returns The C.
 */
export const emitColorExpr = (node: t.Node, env: Env): string =>
  printPropValue(lowerColorValue(node, env));

/**
 * Lowers a dynamic enum-style expression (e.g. `flexDirection: row ? 'row' : 'column'`) to its ER_* constant
 * (or a ternary of them), looking values up in the style key's enum `table`.
 *
 * @param node  The enum expression.
 * @param table  The RN value → ER_* constant table.
 * @param env  The expression environment.
 *
 * @returns The value.
 */
export function lowerEnumValue(
  node: t.Node,
  table: Record<string, string>,
  env: Env,
): IrPropValue {
  if (node.type === 'StringLiteral') {
    const enumConstant = table[node.value];
    if (!enumConstant) {
      throw aotError(
        `AOT: unsupported enum value "${node.value}"`,
        `one of: ${Object.keys(table).join(', ')}`,
      );
    }

    return {kind: 'constant', code: enumConstant};
  }
  if (node.type === 'ConditionalExpression') {
    return {
      kind: 'select',
      test: lowerExpr(node.test, env),
      whenTrue: lowerEnumValue(node.consequent, table, env),
      whenFalse: lowerEnumValue(node.alternate, table, env),
    };
  }
  // A statically resolvable enum (a const string) folds to its constant.
  try {
    const enumValue = evalStatic(node, foldScope(env, env.consts ?? {}));
    if (typeof enumValue === 'string' && table[enumValue]) {
      return {kind: 'constant', code: table[enumValue]};
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
 * A dynamic enum-style expression as C for its ER_* value (see lowerEnumValue).
 *
 * @param node  The enum expression.
 * @param table  The RN value → ER_* constant table.
 * @param env  The expression environment.
 *
 * @returns The C.
 */
export const emitEnumExpr = (
  node: t.Node,
  table: Record<string, string>,
  env: Env,
): string => printPropValue(lowerEnumValue(node, table, env));

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
 * @param env  The expression environment.
 *
 * @returns The ERProps writes.
 */
function lowerDynamicStyleValue(
  key: string,
  valueNode: t.Node,
  env: Env,
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

  // Colors and enums lower to a constant or a C ternary of constants.
  if (dynamicField.kind === 'color') {
    return [
      {field: dynamicField.field, value: lowerColorValue(valueNode, env)},
    ];
  }

  if (dynamicField.kind === 'enum') {
    return [
      {
        field: dynamicField.field,
        value: lowerEnumValue(valueNode, dynamicField.table, env),
      },
    ];
  }

  // Opacity and every size are numbers; C would take a char[] here as its address or refuse it.
  const valueExpr = lowerExpr(valueNode, env);
  if (valueExpr.type.kind === 'string') {
    const error = aotError(
      `AOT: style "${key}" needs a number, but the value is a string`,
      "JS would coerce the string; C cannot. Keep the state numeric — useState(10), not useState('10').",
    );
    if (valueNode.loc) {
      error.aotLoc = valueNode.loc.start;
    }

    throw error;
  }
  if (dynamicField.kind === 'opacity') {
    return [
      {field: dynamicField.field, value: {kind: 'opacity', value: valueExpr}},
    ];
  }

  return [
    {field: dynamicField.field, value: {kind: 'dimension', value: valueExpr}},
  ];
}

/**
 * Whether a style already writes `field`, counting its percentage twin: `width: '50%'` lowers to
 * `width_pct`, and the author has still set the width. Used by the components that fall back to a
 * built-in size or inset when the style leaves one out — without the twin they inject a pixel default
 * ALONGSIDE the percentage, and the engine prefers the pixel value for a size.
 *
 * @param staticAssigns  Static field writes from collectStyleAssigns().
 * @param dynAssigns  State-driven field writes from collectStyleAssigns().
 * @param field  ERProps field to look for.
 *
 * @returns Whether either list writes the field or its `_pct` twin.
 */
export const styleWrites = (
  staticAssigns: {field: string}[],
  dynAssigns: {field: string}[],
  field: string,
): boolean =>
  [field, `${field}_pct`].some(
    fieldName =>
      staticAssigns.some(assign => assign.field === fieldName) ||
      dynAssigns.some(assign => assign.field === fieldName),
  );

/**
 * Collects an element's merged style into static field assigns and dynamic (state-driven) field assigns.
 * Inline object values are tried statically first; a value that references state becomes a dynAssign.
 * Later style sources override earlier ones per field (RN merge), kept in `fields` by ERProps field. A key the
 * element's node neither lays out nor draws with is refused.
 *
 * @param openingElement  The element's opening tag.
 * @param scope  Compile-time constants in reach.
 * @param env  The expression environment.
 * @param spec  The element, for the style keys it takes.
 *
 * @returns The style's constant writes, state-driven writes, and animated-value binds.
 */
export function collectStyleAssigns(
  openingElement: t.JSXOpeningElement,
  scope: Scope,
  env: Env,
  spec: ElementSpec,
): StyleAssigns {
  // ERProps field → its last write: a constant's C, or a state-driven value.
  const fields = new Map<string, {code: string} | {value: IrPropValue}>();
  const binds: AnimBind[] = []; // animated values bound to node properties (native driver)

  // A size in pixels and its percentage twin are one style key, so a later write of either replaces both.
  const setField = (
    field: string,
    write: {code: string} | {value: IrPropValue},
  ): void => {
    fields.delete(
      field.endsWith('_pct') ? field.slice(0, -'_pct'.length) : `${field}_pct`,
    );
    fields.set(field, write);
  };

  // The C handle of an animated value used directly as a style value, or null.
  const animRef = (node: t.Node | null | undefined): string | null =>
    node?.type === 'Identifier' && env.animations?.has(node.name)
      ? env.animations.get(node.name)!.cVar
      : null;
  // `<animValue>.interpolate({ inputRange, outputRange, extrapolate })` → { cVar, interp } for a mapped bind.
  const animInterpRef = (
    node: t.Node | null | undefined,
  ): {cVar: string; interp: Interpolation} | null => {
    if (
      node?.type === 'CallExpression' &&
      node.callee.type === 'MemberExpression' &&
      !node.callee.computed &&
      (node.callee.property as t.Identifier).name === 'interpolate' &&
      node.callee.object.type === 'Identifier' &&
      env.animations?.has(node.callee.object.name)
    ) {
      return {
        cVar: env.animations.get(node.callee.object.name)!.cVar,
        interp: parseInterp(node.arguments[0], env),
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
        if (!Object.hasOwn(ANIM_TRANSFORM_PROPS, transformKey)) {
          throw at(
            aotError(
              `AOT: transform "${transformKey}" cannot be animated in Flow B`,
              `animated transforms: ${Object.keys(ANIM_TRANSFORM_PROPS).join(', ')}.`,
            ),
            transformProp,
          );
        }

        for (const animProp of ANIM_TRANSFORM_PROPS[transformKey]) {
          transformBinds.push(
            mapped
              ? {cVar: mapped.cVar, prop: animProp, interp: mapped.interp}
              : {cVar: direct!, prop: animProp},
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
  const applyStyle = (expr: t.Node): void => {
    // RN ignores an undefined style or style-array entry; so does Flow A's flattenStyle.
    if (expr.type === 'Identifier' && expr.name === 'undefined') return;

    // A style array applies its entries in order, so later entries win.
    if (expr.type === 'ArrayExpression') {
      for (const styleEntry of expr.elements) {
        if (styleEntry) {
          applyStyle(styleEntry);
        }
      }

      return;
    }

    // Inline object: bind animated values natively; fold other keys statically or re-apply them in app_update.
    if (expr.type === 'ObjectExpression') {
      for (const prop of expr.properties) {
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
        const animVar = animRef(prop.value);
        if (animVar && ANIM_STYLE_PROPS[key]) {
          checkAnimatedStyleKey(spec, key, prop);
          for (const animProp of ANIM_STYLE_PROPS[key]) {
            binds.push({cVar: animVar, prop: animProp});
          }
          continue;
        }

        const animInterp = animInterpRef(prop.value);
        if (animInterp && ANIM_STYLE_PROPS[key]) {
          checkAnimatedStyleKey(spec, key, prop);
          for (const animProp of ANIM_STYLE_PROPS[key]) {
            binds.push({
              cVar: animInterp.cVar,
              prop: animProp,
              interp: animInterp.interp,
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
        let staticValue: {v: unknown} | null;
        try {
          staticValue = {v: evalStatic(prop.value, withUndefined(scope))};
        } catch {
          staticValue = null; // references state — lower it as a dynamic value
        }

        if (staticValue) {
          if (staticValue.v != null) {
            checkStyleKey(spec, key, prop);
          }

          for (const assign of lowerStyleChecked(key, staticValue.v)) {
            setField(assign.field, {code: assign.expr});
          }
        } else {
          checkStyleKey(spec, key, prop);
          for (const assign of lowerDynamicStyleValue(key, prop.value, env)) {
            setField(assign.field, {value: assign.value});
          }
        }
      }

      return;
    }

    // A static style reference; null, undefined or false (`cond && s`) means no style, as in RN.
    const resolved = evalStatic(expr, withUndefined(scope));
    if (resolved === null || resolved === undefined || resolved === false) {
      return;
    }

    for (const [styleKey, styleValue] of Object.entries(resolved as object)) {
      if (styleValue === undefined || styleValue === null) continue;

      checkStyleKey(spec, styleKey, expr);
      for (const assign of lowerStyleChecked(styleKey, styleValue)) {
        setField(assign.field, {code: assign.expr});
      }
    }
  };

  // Apply every style attribute in source order.
  for (const attr of openingElement.attributes) {
    if (
      attr.type !== 'JSXAttribute' ||
      (attr.name as t.JSXIdentifier).name !== 'style'
    ) {
      continue;
    }

    applyStyle(attrExpr(attr));
  }

  // Split the merged fields into constant writes and state-driven writes.
  const staticAssigns: StaticAssign[] = [];
  const dynAssigns: DynAssign[] = [];
  for (const [field, fieldWrite] of fields) {
    if ('value' in fieldWrite) {
      dynAssigns.push({field, value: fieldWrite.value});
    } else {
      staticAssigns.push({field, expr: fieldWrite.code});
    }
  }

  return {staticAssigns, dynAssigns, binds};
}

/**
 * Lowers a Text node's body. Static interpolations fold into literal text; any that reference state are values
 * set again on every update.
 *
 * @param children  The <Text>'s children.
 * @param scope  Compile-time constants in reach.
 * @param env  The expression environment.
 *
 * @returns The body.
 */
export function lowerNodeText(
  children: t.JSXElement['children'],
  scope: Scope,
  env: Env,
): IrText {
  const parts: IrTextPart[] = [];
  for (const child of inlineFragments(children)) {
    if (child.type === 'JSXText') {
      const text = /\n/.test(child.value)
        ? child.value.replace(/\s+/g, ' ').trim()
        : child.value;
      parts.push({kind: 'literal', text});
    } else if (child.type === 'JSXExpressionContainer') {
      if (child.expression.type === 'JSXEmptyExpression') continue;
      // Constants fold into literal text; anything referencing state is a runtime value.
      parts.push(...lowerText(child.expression, env, scope).parts);
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
function cleanJsxText(value: string): string {
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
 * font_weight, font_style, text_decoration, letter_spacing}] (C-expr fields; inherit sentinels for unset).
 * Returns null when there's no nested <Text> (caller uses the single-string buildText path). Static only —
 * a dynamic {…} segment or a state-driven span style throws.
 *
 * @param children  The <Text>'s children.
 * @param scope  Compile-time constants in reach.
 * @param env  The expression environment.
 *
 * @returns The spans, or null when the <Text> has no nested <Text>.
 */
export function collectTextSpans(
  children: t.JSXElement['children'],
  scope: Scope,
  env: Env,
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
    color: '0u',
    font_size: '0',
    font_weight: '0xFF',
    font_style: '0xFF',
    text_decoration: '0xFF',
    letter_spacing: 'ER_LAYOUT_AUTO',
  });

  // One span per child: text and static {…} values inherit the parent's style; a nested <Text> carries its own.
  const spans = [];
  for (const child of children) {
    if (child.type === 'JSXText') {
      const text = cleanJsxText(child.value);
      if (text) {
        spans.push(inheritSpan(cstr(text)));
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
        spans.push(inheritSpan(cstr(String(value))));
      }
    } else if (
      child.type === 'JSXElement' &&
      (child.openingElement.name as t.JSXIdentifier).name === 'Text'
    ) {
      checkAttributes(TEXT_SPAN, child.openingElement);
      const {staticAssigns, dynAssigns} = collectStyleAssigns(
        child.openingElement,
        scope,
        env,
        TEXT_SPAN,
      );
      if (dynAssigns.length) {
        throw aotError(
          'AOT: a state-driven style on a nested <Text> span is not supported',
          'give the span <Text> a static style.',
        );
      }

      // Each span field is the nested <Text>'s own static style value, or the inherit sentinel.
      const spanStyle = (fieldName: string, fallback: string): string =>
        staticAssigns.find(assign => assign.field === fieldName)?.expr ??
        fallback;
      spans.push({
        text: cstr(staticTextContent(child.children, scope)),
        color: spanStyle('color', '0u'),
        font_size: spanStyle('font_size', '0'),
        font_weight: spanStyle('font_weight', '0xFF'),
        font_style: spanStyle('font_style', '0xFF'),
        text_decoration: spanStyle('text_decoration', '0xFF'),
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
    max: env.caps.maxTextSpans,
  });

  return spans;
}
