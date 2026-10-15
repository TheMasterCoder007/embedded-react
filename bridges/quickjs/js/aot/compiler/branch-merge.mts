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

import type * as t from '@babel/types';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** Node fields that say where a node is or how it was written, not what it is. */
const POSITION_KEYS = new Set([
  'loc',
  'start',
  'end',
  'range',
  'extra',
  'leadingComments',
  'trailingComments',
  'innerComments',
]);

/** Node types a merged value may not contain: their meaning is not a value boot can pick. */
const UNMERGEABLE_TYPES = new Set([
  'JSXElement',
  'JSXFragment',
  'ArrowFunctionExpression',
  'FunctionExpression',
  'CallExpression',
  'NewExpression',
  'AssignmentExpression',
  'UpdateExpression',
]);

/** The Math functions the compiler lowers, and how many arguments each takes. */
const MATH_ARITY: Record<string, number> = {
  sin: 1,
  cos: 1,
  tan: 1,
  sqrt: 1,
  abs: 1,
  round: 1,
  floor: 1,
  ceil: 1,
  trunc: 1,
  min: 2,
  max: 2,
  atan2: 2,
  pow: 2,
};

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Whether two nodes are the same code, wherever each was written.
 *
 * @param first  One node.
 * @param second  The other.
 *
 * @returns True when they are.
 */
export function sameCode(first: unknown, second: unknown): boolean {
  if (first === second) return true;
  if (
    typeof first !== 'object' ||
    typeof second !== 'object' ||
    first === null ||
    second === null
  ) {
    return false;
  }

  if (Array.isArray(first) !== Array.isArray(second)) return false;
  const keysOf = (node: object) =>
    Object.keys(node).filter(
      key =>
        !POSITION_KEYS.has(key) &&
        (node as Record<string, unknown>)[key] !== undefined,
    );
  const firstKeys = keysOf(first);
  const secondKeys = keysOf(second);
  return (
    firstKeys.length === secondKeys.length &&
    firstKeys.every(key =>
      sameCode(
        (first as Record<string, unknown>)[key],
        (second as Record<string, unknown>)[key],
      ),
    )
  );
}

/**
 * Whether a node holds code a merged value cannot pick between: an element, a function, or a call (other than
 * Math's, which boot works out).
 *
 * @param node  The node.
 *
 * @returns True when it does.
 */
function holdsUnmergeable(node: unknown): boolean {
  if (typeof node !== 'object' || node === null) return false;
  if (Array.isArray(node)) return node.some(holdsUnmergeable);
  const typed = node as {type?: string} & Record<string, unknown>;
  if (typed.type === 'CallExpression' && isLowerableMath(typed as t.Node)) {
    return (typed.arguments as t.Node[]).some(holdsUnmergeable);
  }

  if (typed.type && UNMERGEABLE_TYPES.has(typed.type)) return true;
  return Object.keys(typed).some(
    key => !POSITION_KEYS.has(key) && holdsUnmergeable(typed[key]),
  );
}

/**
 * Whether a node is a `Math.<fn>(…)` call the compiler lowers, with the number of arguments it takes.
 *
 * @param node  The node.
 *
 * @returns True when it is.
 */
function isLowerableMath(node: t.Node): boolean {
  if (
    node.type !== 'CallExpression' ||
    node.callee.type !== 'MemberExpression' ||
    node.callee.computed ||
    node.callee.object.type !== 'Identifier' ||
    node.callee.object.name !== 'Math' ||
    node.callee.property.type !== 'Identifier' ||
    node.arguments.some(arg => arg.type === 'SpreadElement')
  ) {
    return false;
  }

  const fn = node.callee.property.name;
  return (
    Object.hasOwn(MATH_ARITY, fn) && MATH_ARITY[fn] === node.arguments.length
  );
}

/**
 * `test ? whenTrue : whenFalse`, entry by entry where both are object or array literals of the same shape, so
 * an entry that reads state stays an expression of its own.
 *
 * @param test  The test.
 * @param whenTrue  The value when it holds.
 * @param whenFalse  The value when it does not.
 *
 * @returns The merged value.
 */
function mergeValues(
  test: t.Expression,
  whenTrue: t.Expression,
  whenFalse: t.Expression,
): t.Expression {
  if (sameCode(whenTrue, whenFalse)) return whenTrue;

  // Two objects with the same keys in the same order merge entry by entry.
  if (
    whenTrue.type === 'ObjectExpression' &&
    whenFalse.type === 'ObjectExpression' &&
    whenTrue.properties.length === whenFalse.properties.length &&
    whenTrue.properties.every((property, index) => {
      const other = whenFalse.properties[index];
      return (
        property.type === 'ObjectProperty' &&
        other.type === 'ObjectProperty' &&
        !property.computed &&
        !other.computed &&
        sameCode(property.key, other.key)
      );
    })
  ) {
    return {
      ...whenTrue,
      properties: whenTrue.properties.map((property, index) => ({
        ...(property as t.ObjectProperty),
        value: mergeValues(
          test,
          (property as t.ObjectProperty).value as t.Expression,
          (whenFalse.properties[index] as t.ObjectProperty)
            .value as t.Expression,
        ),
      })),
    };
  }

  // Two arrays of the same length merge element by element.
  if (
    whenTrue.type === 'ArrayExpression' &&
    whenFalse.type === 'ArrayExpression' &&
    whenTrue.elements.length === whenFalse.elements.length &&
    [...whenTrue.elements, ...whenFalse.elements].every(
      element => element && element.type !== 'SpreadElement',
    )
  ) {
    return {
      ...whenTrue,
      elements: whenTrue.elements.map((element, index) =>
        mergeValues(
          test,
          element as t.Expression,
          whenFalse.elements[index] as t.Expression,
        ),
      ),
    };
  }

  return {
    type: 'ConditionalExpression',
    test,
    consequent: whenTrue,
    alternate: whenFalse,
    loc: whenTrue.loc,
  } as t.ConditionalExpression;
}

/**
 * An attribute's value as an expression, or null for a bare attribute (`<Switch disabled />`).
 *
 * @param attribute  The attribute.
 *
 * @returns The value.
 */
function attributeValue(attribute: t.JSXAttribute): t.Expression | null {
  const {value} = attribute;
  if (!value) return null;
  if (value.type === 'JSXExpressionContainer') {
    return value.expression.type === 'JSXEmptyExpression'
      ? null
      : value.expression;
  }

  return value as t.Expression;
}

/**
 * Two attributes merged, or null when they differ in a way boot cannot pick: their names, or a value that is a
 * handler, a ref, a key, an element or a call.
 *
 * @param test  The test.
 * @param whenTrue  The attribute when it holds.
 * @param whenFalse  The attribute when it does not.
 *
 * @returns The merged attribute, or null.
 */
function mergeAttributes(
  test: t.Expression,
  whenTrue: t.JSXAttribute | t.JSXSpreadAttribute,
  whenFalse: t.JSXAttribute | t.JSXSpreadAttribute,
): t.JSXAttribute | t.JSXSpreadAttribute | null {
  if (sameCode(whenTrue, whenFalse)) return whenTrue;
  if (
    whenTrue.type !== 'JSXAttribute' ||
    whenFalse.type !== 'JSXAttribute' ||
    !sameCode(whenTrue.name, whenFalse.name)
  ) {
    return null;
  }

  const name = whenTrue.name.type === 'JSXIdentifier' ? whenTrue.name.name : '';
  const trueValue = attributeValue(whenTrue);
  const falseValue = attributeValue(whenFalse);
  if (
    name === 'key' ||
    name === 'ref' ||
    /^on[A-Z]/.test(name) ||
    !trueValue ||
    !falseValue ||
    holdsUnmergeable(trueValue) ||
    holdsUnmergeable(falseValue)
  ) {
    return null;
  }

  return {
    ...whenTrue,
    value: {
      type: 'JSXExpressionContainer',
      expression: mergeValues(test, trueValue, falseValue),
      loc: whenTrue.value?.loc,
    } as t.JSXExpressionContainer,
  };
}

/**
 * An element's children, less the empty `{}` a JSX comment leaves.
 *
 * @param element  The element.
 *
 * @returns The children.
 */
const childrenOf = (element: t.JSXElement): t.JSXElement['children'] =>
  element.children.filter(
    child =>
      !(
        child.type === 'JSXExpressionContainer' &&
        child.expression.type === 'JSXEmptyExpression'
      ),
  );

/**
 * JSX text as it renders: text spanning lines collapses its whitespace, as Babel does.
 *
 * @param text  The text.
 *
 * @returns The rendered text.
 */
const renderedText = (text: t.JSXText): string =>
  /\n/.test(text.value) ? text.value.replace(/\s+/g, ' ').trim() : text.value;

/**
 * Two branches of a test that depends on the panel, as one element whose differing values each pick at boot,
 * or null when the branches differ in shape: their elements, attributes, children, handlers or text. A branch
 * that changes only values then needs no per-panel variant.
 *
 * @param test  The test.
 * @param whenTrue  The element when it holds.
 * @param whenFalse  The element when it does not.
 *
 * @returns The merged element, or null.
 */
export function mergeBranchElements(
  test: t.Expression,
  whenTrue: t.JSXElement,
  whenFalse: t.JSXElement,
): t.JSXElement | null {
  if (sameCode(whenTrue, whenFalse)) return whenTrue;
  const trueOpening = whenTrue.openingElement;
  const falseOpening = whenFalse.openingElement;
  if (
    !sameCode(trueOpening.name, falseOpening.name) ||
    trueOpening.attributes.length !== falseOpening.attributes.length
  ) {
    return null;
  }

  // Attributes pair up in order, since a later style or spread wins over an earlier one.
  const attributes: (t.JSXAttribute | t.JSXSpreadAttribute)[] = [];
  for (const [index, attribute] of trueOpening.attributes.entries()) {
    const merged = mergeAttributes(
      test,
      attribute,
      falseOpening.attributes[index],
    );
    if (!merged) return null;
    attributes.push(merged);
  }

  // Children pair up too: elements merge in turn, and a differing {value} picks at boot.
  const trueChildren = childrenOf(whenTrue);
  const falseChildren = childrenOf(whenFalse);
  if (trueChildren.length !== falseChildren.length) return null;
  const children: t.JSXElement['children'] = [];
  for (const [index, child] of trueChildren.entries()) {
    const other = falseChildren[index];
    if (
      sameCode(child, other) ||
      (child.type === 'JSXText' &&
        other.type === 'JSXText' &&
        renderedText(child) === renderedText(other))
    ) {
      children.push(child);
    } else if (child.type === 'JSXElement' && other.type === 'JSXElement') {
      const merged = mergeBranchElements(test, child, other);
      if (!merged) return null;
      children.push(merged);
    } else if (
      child.type === 'JSXExpressionContainer' &&
      other.type === 'JSXExpressionContainer' &&
      !holdsUnmergeable(child.expression) &&
      !holdsUnmergeable(other.expression)
    ) {
      children.push({
        ...child,
        expression: mergeValues(
          test,
          child.expression as t.Expression,
          other.expression as t.Expression,
        ),
      });
    } else {
      return null;
    }
  }

  return {
    ...whenTrue,
    openingElement: {...trueOpening, attributes},
    children,
  };
}
