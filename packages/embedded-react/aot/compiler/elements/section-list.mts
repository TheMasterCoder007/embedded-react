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

import {aotError} from '../diagnostics.mts';
import {componentReturnJSX, isFn} from '../collect.mts';
import {emitChildren, rowEnv} from '../control-flow.mts';
import {foldRuntime} from '../expressions.mts';
import {emitNode} from '../nodes.mts';
import {evalStatic, setBinding} from '../static-eval.mts';
import {attrExpr} from '../style-text.mts';
import {patternNames} from '../updaters.mts';
import {at} from './props.mts';
import {elementOf, expressionChild, refuseSpreads} from './rewrite.mts';
import type * as t from '@babel/types';
import type {Out} from '../out.mts';
import type {IrElement, IrNode} from '../ir/nodes.mts';
import type {
  EmitOptions,
  Env,
  FunctionNode,
  Scope,
  StateTable,
} from '../types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** One section, as the callbacks see it. */
interface Section {
  /** What `section` is bound to. */
  value: Record<string, unknown>;
  /** What its rows map over, or null for none. */
  data: t.Expression | null;
  /** A constant section's rows, mapped over by a name of the compiler's own. */
  constantData: unknown[] | null;
  /** Its own renderItem, which wins over the list's. */
  renderItem: FunctionNode | null;
  /** Where it is written, for errors. */
  node: t.Node;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** A constant section's rows; `@` keeps it from meeting an app's name. */
const SECTION_DATA = 'data@section';

const SECTIONS_HINT =
  'write sections={[{title: …, data: …}, …]} in place, where a data may be list state, or use a constant ' +
  'array. The number of sections is fixed at build time; a list of rows is not.';

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * <SectionList sections renderItem renderSectionHeader renderSectionFooter style /> → a <ScrollView> holding each
 * section's header, rows and footer as flat siblings, as SectionList.js renders it. The sections are unrolled at
 * build time; each section's rows are a `.map` over its data, so a data that is list state is a pool of rows.
 * keyExtractor is ignored (no reconciler).
 *
 * @param element  The <SectionList>, its props already checked.
 * @param scope  Compile-time constants in reach.
 * @param output  Everything emitted so far.
 * @param environment  The expression environment.
 * @param state  The component's state table.
 * @param options  How the element is placed.
 *
 * @returns What the <ScrollView> lowers to.
 */
export function emitSectionList(
  element: t.JSXElement,
  scope: Scope,
  output: Out,
  environment: Env,
  state: StateTable,
  options?: EmitOptions,
): IrElement {
  refuseSpreads(element, 'SectionList');
  const attributes = new Map<string, t.JSXAttribute>();
  for (const attribute of element.openingElement
    .attributes as t.JSXAttribute[]) {
    attributes.set(attribute.name.name as string, attribute);
  }

  const sectionsAttribute = attributes.get('sections');
  if (!sectionsAttribute) {
    throw aotError(
      'AOT: <SectionList> needs a sections prop',
      "sections={[{title: 'A', data: items}]}",
    );
  }

  const renderItem = callbackOf(attributes.get('renderItem'), 'renderItem');
  const header = callbackOf(
    attributes.get('renderSectionHeader'),
    'renderSectionHeader',
  );
  const footer = callbackOf(
    attributes.get('renderSectionFooter'),
    'renderSectionFooter',
  );
  const sections = sectionsOf(sectionsAttribute, scope, environment);

  // The ScrollView first, so its node comes before the rows it holds.
  const styleAttribute = attributes.get('style');
  const scrollView = emitNode(
    elementOf(
      'ScrollView',
      styleAttribute ? [styleAttribute] : [],
      [],
      element,
    ),
    scope,
    output,
    environment,
    state,
    options,
  ) as {kind: 'node'; id: string; node: IrNode};

  // Each callback sees its own name for the section.
  const emitBound = (
    callback: FunctionNode,
    allowed: string[],
    section: Section,
    child: (
      bound: FunctionNode,
      names: Map<string, string>,
    ) => t.JSXElement['children'][number],
  ): void => {
    const names = destructured(callback, allowed);
    const sectionName = names.get('section');
    // A data that is list state has no value to bind, so `section.data` reads it where it is written.
    const bound =
      sectionName && section.data && !Object.hasOwn(section.value, 'data')
        ? ({
            ...callback,
            body: withSectionData(callback.body, sectionName, section.data),
          } as FunctionNode)
        : callback;
    const sectionScope: Scope = {...scope};
    if (sectionName) {
      setBinding(sectionScope, sectionName, section.value);
    }
    if (section.constantData) {
      setBinding(sectionScope, SECTION_DATA, section.constantData);
    }

    scrollView.node.children.push(
      ...emitChildren(
        [child(bound, names)],
        sectionScope,
        output,
        rowEnv(environment, [sectionName], sectionScope),
        state,
      ),
    );
  };

  for (const section of sections) {
    if (header) {
      emitBound(header, ['section'], section, bound => bodyChild(bound));
    }

    const rowRenderer = section.renderItem ?? renderItem;
    const rows =
      section.data ??
      (section.constantData
        ? ({type: 'Identifier', name: SECTION_DATA} as t.Identifier)
        : null);
    if (rows) {
      if (!rowRenderer) {
        throw at(
          aotError(
            'AOT: a <SectionList> section with data needs a renderItem',
            'renderItem={({item}) => <Row item={item} />} on the list, or on the section.',
          ),
          section.node,
        );
      }

      emitBound(
        rowRenderer,
        ['item', 'index', 'section'],
        section,
        (bound, names) => rowsChild(bound, names, rows),
      );
    }

    if (footer) {
      emitBound(footer, ['section'], section, bound => bodyChild(bound));
    }
  }

  return scrollView;
}

/**
 * `node` with each `<sectionName>.data` read replaced by `data`, outside functions that bind the name again.
 * The callback is shared by every section, so what changes is copied.
 *
 * @param node  A node of the callback's body.
 * @param sectionName  The callback's name for the section.
 * @param data  The section's data, as written.
 *
 * @returns The node, or a copy with the reads replaced.
 */
function withSectionData(
  node: unknown,
  sectionName: string,
  data: t.Expression,
): unknown {
  if (Array.isArray(node)) {
    const replaced = node.map(entry =>
      withSectionData(entry, sectionName, data),
    );
    return replaced.some((entry, index) => entry !== node[index])
      ? replaced
      : node;
  }

  if (!node || typeof (node as t.Node).type !== 'string') return node;

  const astNode = node as t.Node;
  if (
    (astNode.type === 'MemberExpression' ||
      astNode.type === 'OptionalMemberExpression') &&
    !astNode.computed &&
    astNode.object.type === 'Identifier' &&
    astNode.object.name === sectionName &&
    astNode.property.type === 'Identifier' &&
    astNode.property.name === 'data'
  ) {
    return data;
  }

  if (
    isFn(astNode) &&
    astNode.params.some(parameter =>
      patternNames(parameter).includes(sectionName),
    )
  ) {
    return node;
  }

  let copy: Record<string, unknown> | null = null;
  for (const [key, value] of Object.entries(astNode)) {
    if (key === 'loc' || key.endsWith('Comments')) continue;
    const replaced = withSectionData(value, sectionName, data);
    if (replaced !== value) {
      copy ??= {...astNode};
      copy[key] = replaced;
    }
  }

  return copy ?? node;
}

/**
 * A render callback prop, which must be an inline function.
 *
 * @param attribute  The prop, if given.
 * @param name  Its name, for the error.
 *
 * @returns The function, or null without the prop.
 */
function callbackOf(
  attribute: t.JSXAttribute | undefined,
  name: string,
): FunctionNode | null {
  if (!attribute) return null;

  const callback = attrExpr(attribute);
  if (!isFn(callback)) {
    throw at(
      aotError(
        `AOT: <SectionList ${name}> must be an inline function`,
        `${name}={({section}) => <Text>{section.title}</Text>}`,
      ),
      attribute,
    );
  }

  return callback;
}

/**
 * The names a render callback destructures its one argument into.
 *
 * @param callback  The callback.
 * @param allowed  The fields it may take.
 *
 * @returns Each field taken, to its local name.
 */
function destructured(
  callback: FunctionNode,
  allowed: string[],
): Map<string, string> {
  const names = new Map<string, string>();
  const parameter = callback.params[0];
  if (!parameter) return names;

  const refusal = () =>
    at(
      aotError(
        `AOT: a <SectionList> callback may destructure only ${allowed.join(' / ')} (to plain names)`,
        `write it as ({${allowed.join(', ')}}) => …`,
      ),
      parameter,
    );
  if (parameter.type !== 'ObjectPattern' || callback.params.length > 1) {
    throw refusal();
  }

  for (const property of parameter.properties) {
    if (
      property.type !== 'ObjectProperty' ||
      property.computed ||
      property.key.type !== 'Identifier' ||
      property.value.type !== 'Identifier' ||
      !allowed.includes(property.key.name)
    ) {
      throw refusal();
    }

    names.set(property.key.name, property.value.name);
  }

  return names;
}

/**
 * A header or footer callback's body as a child: the element a block body returns, or an expression body as is.
 *
 * @param callback  The callback.
 *
 * @returns The child.
 */
function bodyChild(callback: FunctionNode): t.JSXElement['children'][number] {
  if (callback.body.type === 'BlockStatement') {
    return componentReturnJSX(callback);
  }

  return callback.body.type === 'JSXElement'
    ? callback.body
    : expressionChild(callback.body);
}

/**
 * A section's rows as a child: `{rows.map((item, index) => body)}`, the renderItem's body as a `.map` callback.
 *
 * @param renderItem  The renderItem.
 * @param names  The fields it destructures, to their local names.
 * @param rows  What the rows map over.
 *
 * @returns The child.
 */
function rowsChild(
  renderItem: FunctionNode,
  names: Map<string, string>,
  rows: t.Expression,
): t.JSXExpressionContainer {
  const itemName = names.get('item');
  if (!itemName) {
    throw at(
      aotError(
        'AOT: a <SectionList> renderItem must destructure item',
        'renderItem={({item}) => …}',
      ),
      renderItem,
    );
  }

  const parameters: t.Identifier[] = [{type: 'Identifier', name: itemName}];
  const indexName = names.get('index');
  if (indexName) {
    parameters.push({type: 'Identifier', name: indexName});
  }

  return expressionChild({
    type: 'CallExpression',
    callee: {
      type: 'MemberExpression',
      object: rows,
      property: {type: 'Identifier', name: 'map'},
      computed: false,
    },
    arguments: [
      {
        type: 'ArrowFunctionExpression',
        params: parameters,
        body: renderItem.body,
        async: false,
        expression: renderItem.body.type !== 'BlockStatement',
      },
    ],
  });
}

/**
 * The sections, unrolled at build time: written in place, where each field folds and a data may be anything a
 * `.map` takes, or a constant array.
 *
 * @param attribute  The sections prop.
 * @param scope  Compile-time constants in reach.
 * @param environment  The expression environment.
 *
 * @returns The sections.
 */
function sectionsOf(
  attribute: t.JSXAttribute,
  scope: Scope,
  environment: Env,
): Section[] {
  const expression = attrExpr(attribute);
  if (expression.type === 'ArrayExpression') {
    return expression.elements.flatMap(entry => {
      if (!entry || entry.type === 'SpreadElement') {
        throw at(
          aotError(
            'AOT: <SectionList sections> cannot have holes or spreads',
            'write each section out.',
          ),
          entry ?? expression,
        );
      }

      return entry.type === 'ObjectExpression'
        ? [writtenSection(entry, scope, environment)]
        : foldedSection(foldRuntime(entry, environment, scope), entry);
    });
  }

  let folded: unknown;
  try {
    folded = evalStatic(expression, scope);
  } catch {
    throw at(
      aotError(
        'AOT: <SectionList sections> must fold to an array at build time',
        SECTIONS_HINT,
      ),
      attribute,
    );
  }

  if (!Array.isArray(folded)) {
    throw at(
      aotError('AOT: <SectionList sections> must be an array', SECTIONS_HINT),
      attribute,
    );
  }

  return folded.flatMap(value => foldedSection(value, attribute));
}

/**
 * A section written in place. Its data and renderItem are kept as written; every other field folds.
 *
 * @param object  The section.
 * @param scope  Compile-time constants in reach.
 * @param environment  The expression environment.
 *
 * @returns The section.
 */
function writtenSection(
  object: t.ObjectExpression,
  scope: Scope,
  environment: Env,
): Section {
  const section: Section = {
    value: {},
    data: null,
    constantData: null,
    renderItem: null,
    node: object,
  };
  for (const property of object.properties) {
    if (
      property.type !== 'ObjectProperty' ||
      property.computed ||
      (property.key.type !== 'Identifier' &&
        property.key.type !== 'StringLiteral')
    ) {
      throw at(
        aotError(
          'AOT: a <SectionList> section must be plain fields',
          "write each field out: {title: 'A', data: items}.",
        ),
        property,
      );
    }

    const key =
      property.key.type === 'Identifier'
        ? property.key.name
        : property.key.value;
    const value = property.value as t.Expression;
    if (key === 'keyExtractor') continue;

    if (key === 'renderItem') {
      if (!isFn(value)) {
        throw at(
          aotError(
            "AOT: a <SectionList> section's renderItem must be an inline function",
            'renderItem: ({item}) => <Row item={item} />',
          ),
          property,
        );
      }

      section.renderItem = value;
      continue;
    }

    if (key === 'data') {
      section.data = value;
      // A data that folds can be read through `section` too; list state is only mapped over.
      try {
        const rows = foldRuntime(value, environment, scope);
        if (Array.isArray(rows)) {
          section.value.data = rows;
        }
      } catch {
        // mapped over only
      }
      continue;
    }

    section.value[key] = foldRuntime(value, environment, scope);
  }

  return section;
}

/**
 * A section that folded. One that is not an object is skipped, as Flow A skips it.
 *
 * @param value  The folded section.
 * @param node  Where it is written, for errors.
 *
 * @returns The section, or nothing.
 */
function foldedSection(value: unknown, node: t.Node): Section[] {
  if (value === null || typeof value !== 'object') return [];

  const fields = value as Record<string, unknown>;
  if (fields.data != null && !Array.isArray(fields.data)) {
    throw at(
      aotError(
        "AOT: a <SectionList> section's data must be an array",
        SECTIONS_HINT,
      ),
      node,
    );
  }

  return [
    {
      value: fields,
      data: null,
      constantData: (fields.data as unknown[] | undefined) ?? null,
      renderItem: null,
      node,
    },
  ];
}
