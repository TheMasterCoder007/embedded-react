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

import {parse} from '@babel/parser';
import {aotError} from './diagnostics.mts';
import type {ParserPlugin} from '@babel/parser';
import type * as t from '@babel/types';
import type {CompileOptions} from './options.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

// Type-only expression wrappers: `x as T`, `x satisfies T`, `x!`, `<T>x`, `f<T>` — unwrap to the inner expr.
const TS_EXPR_WRAPPERS = new Set([
  'TSAsExpression',
  'TSSatisfiesExpression',
  'TSNonNullExpression',
  'TSTypeAssertion',
  'TSInstantiationExpression',
]);

// Type-only declarations (no runtime presence) — dropped from any statement/body list.
const TS_TYPE_DECLS = new Set([
  'TSInterfaceDeclaration',
  'TSTypeAliasDeclaration',
  'TSDeclareFunction',
]);

// Type-only fields hung off otherwise-runtime nodes — deleted so nothing downstream traverses into them.
const TS_TYPE_FIELDS = [
  'typeAnnotation',
  'returnType',
  'typeParameters',
  'typeArguments',
  'accessibility',
  'definite',
  'declare',
  'readonly',
  'override',
  'abstract',
];

// Keys that never hold child AST nodes — skip them so the scrub stays cheap and never mangles metadata.
const TS_SKIP_KEYS = new Set([
  'loc',
  'start',
  'end',
  'range',
  'leadingComments',
  'trailingComments',
  'innerComments',
  'comments',
  'extra',
  'tokens',
]);

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Is this app a TypeScript entry? Driven by the filename extension, or an explicit opts.ts (for tests).
 *
 * @param opts  compileSource's options.
 *
 * @returns Whether to parse the source as TypeScript.
 */
const isTsEntry = (opts: Pick<CompileOptions, 'ts' | 'filename'>): boolean =>
  opts.ts ?? /\.[cm]?tsx$/.test(opts.filename || '');

/**
 * The Babel parser plugins for an entry.
 *
 * @param isTypeScript  Whether the entry is TypeScript.
 *
 * @returns JSX, plus TypeScript when asked.
 */
const parserPlugins = (isTypeScript: boolean): ParserPlugin[] =>
  isTypeScript ? ['jsx', 'typescript'] : ['jsx'];

/**
 * True for nodes that carry no runtime meaning and must be removed from a statement/specifier list.
 *
 * @param node  An element of an AST list.
 *
 * @returns Whether it is type-only.
 */
const isTypeOnly = (node: t.Node | null | undefined): boolean =>
  !!node &&
  (TS_TYPE_DECLS.has(node.type) ||
    // `import type ... ` / `export type ...`, and per-specifier `import { type X }`.
    ((node.type === 'ImportDeclaration' || node.type === 'ImportSpecifier') &&
      node.importKind === 'type') ||
    ((node.type === 'ExportNamedDeclaration' ||
      node.type === 'ExportSpecifier') &&
      node.exportKind === 'type'));

/**
 * Follows a chain of type-only expression wrappers to the runtime expression underneath.
 *
 * @param node  An expression, possibly wrapped in `as` / `satisfies` / `!` / `<T>`.
 *
 * @returns The expression under every wrapper.
 */
const unwrapTs = (node: any): any => {
  while (node && TS_EXPR_WRAPPERS.has(node.type)) {
    node = node.expression;
  }

  return node;
};

/**
 * Strips every TypeScript-only construct from a parsed AST, in place: drop type declarations and type
 * imports, unwrap `as`/`!`/`<T>` expression wrappers, and delete type-annotation fields. Remaining nodes
 * keep their .loc, so the compiler's error code-frames stay accurate.
 *
 * @param root  The parsed file.
 *
 * @returns The same file, scrubbed.
 */
function stripTypeScript(root: t.File): t.File {
  // A walk over every key of every node, so a node here is a plain record of child values.
  const visit = (node: any) => {
    if (!node || typeof node !== 'object') return;

    // Delete the node's own type-only fields, then walk its children.
    for (const field of TS_TYPE_FIELDS) {
      if (field in node) {
        delete node[field];
      }
    }

    for (const key of Object.keys(node)) {
      if (TS_SKIP_KEYS.has(key)) continue;

      let value = node[key];
      if (Array.isArray(value)) {
        // A list (statements, specifiers, arguments …): drop type-only entries and unwrap wrapped ones.
        const kept = [];
        for (let item of value) {
          if (isTypeOnly(item)) continue;
          if (item && TS_EXPR_WRAPPERS.has(item.type)) {
            item = unwrapTs(item);
          }
          kept.push(item);
          visit(item);
        }

        node[key] = kept;
      } else if (value && typeof value.type === 'string') {
        // A single child node: replace a type-only wrapper with the expression under it.
        if (TS_EXPR_WRAPPERS.has(value.type)) {
          value = unwrapTs(value);
          node[key] = value;
        }

        visit(value);
      } else if (value && typeof value === 'object') {
        visit(value);
      }
    }
  };

  visit(root);
  return root;
}

/**
 * One pass over the parsed program that fixes what `undefined` means before anything is compiled.
 *
 * - A JSX attribute whose value is `{undefined}` is DROPPED. Flow A's buildProps omits an undefined prop,
 *   so the node keeps its default — `visible={undefined}` stays visible. Doing it here gives the twenty-odd
 *   prop readers that view at once, instead of each deciding what an undefined value means.
 *   A value that only becomes undefined in some scope — a prop a child was never given — is dropped per
 *   emission by omitUndefinedAttrs. This literal pass still matters for <Svg> shapes, which bypass it.
 * - A BINDING named `undefined` is refused. JS allows shadowing the global, but Flow B gives `undefined`
 *   a meaning of its own (an omitted prop, empty text), and a shadowing local would silently lose to it.
 *
 * @param ast  The parsed file, rewritten in place.
 */
export function normalizeUndefined(ast: t.File): void {
  // Refuse an identifier that binds the name `undefined`, pointing the error at it.
  const rejectUndefinedName = (id: t.Node | null | undefined) => {
    if (id?.type !== 'Identifier' || id.name !== 'undefined') return;

    const error = aotError(
      'AOT: `undefined` cannot be used as a name',
      'Flow B reads `undefined` as the global everywhere — an omitted prop, or empty text — ' +
        'so a binding with that name would never be read. Rename it.',
    );
    if (id.loc) {
      error.aotLoc = id.loc.start;
    }

    throw error;
  };

  // Refuse `undefined` anywhere in a binding pattern: plain, destructured, defaulted or rest.
  const rejectUndefinedBindings = (
    patternNode: t.Node | null | undefined,
  ): void => {
    if (!patternNode) return;

    if (patternNode.type === 'Identifier') {
      rejectUndefinedName(patternNode);
    } else if (patternNode.type === 'ArrayPattern') {
      patternNode.elements.forEach(rejectUndefinedBindings);
    } else if (patternNode.type === 'ObjectPattern') {
      patternNode.properties.forEach(property =>
        rejectUndefinedBindings(
          property.type === 'RestElement' ? property.argument : property.value,
        ),
      );
    } else if (patternNode.type === 'AssignmentPattern') {
      rejectUndefinedBindings(patternNode.left);
    } else if (patternNode.type === 'RestElement') {
      rejectUndefinedBindings(patternNode.argument);
    }
  };

  // A JSX attribute written as exactly `name={undefined}`.
  const isUndefinedAttr = (attr: t.JSXAttribute | t.JSXSpreadAttribute) =>
    attr.type === 'JSXAttribute' &&
    attr.value?.type === 'JSXExpressionContainer' &&
    attr.value.expression.type === 'Identifier' &&
    attr.value.expression.name === 'undefined';

  // Keys that never hold child nodes.
  const SKIP = new Set([
    'loc',
    'start',
    'end',
    'extra',
    'leadingComments',
    'trailingComments',
    'innerComments',
  ]);

  // A walk over every key of every node, so a node here is a plain record of child values.
  const visit = (node: any) => {
    // Check the names each declaring node binds and drop `{undefined}` attributes from a JSX element.
    if (!node || typeof node.type !== 'string') return;

    switch (node.type) {
      case 'VariableDeclarator':
        rejectUndefinedBindings(node.id);
        break;
      case 'FunctionDeclaration':
      case 'FunctionExpression':
      case 'ArrowFunctionExpression':
      case 'ObjectMethod':
      case 'ClassMethod':
        if (node.id) {
          rejectUndefinedName(node.id);
        }
        node.params.forEach(rejectUndefinedBindings);
        break;
      case 'CatchClause':
        rejectUndefinedBindings(node.param);
        break;
      case 'ClassDeclaration':
      case 'ClassExpression':
        if (node.id) {
          rejectUndefinedName(node.id);
        }
        break;
      case 'ImportSpecifier':
      case 'ImportDefaultSpecifier':
      case 'ImportNamespaceSpecifier':
        rejectUndefinedName(node.local);
        break;
      case 'JSXOpeningElement':
        node.attributes = node.attributes.filter(
          (attr: t.JSXAttribute | t.JSXSpreadAttribute) =>
            !isUndefinedAttr(attr),
        );
        break;
    }

    // Then walk every child node.
    for (const key of Object.keys(node)) {
      if (SKIP.has(key)) continue;

      const value = node[key];
      if (Array.isArray(value)) {
        value.forEach(visit);
      } else if (value && typeof value.type === 'string') {
        visit(value);
      }
    }
  };

  visit(ast);
}

/**
 * Parses an app entry to a JS+JSX AST, transparently stripping TypeScript when the entry is .tsx (JSX is not
 * allowed in a .ts file). The strip is faithful: an App.tsx compiles to the same C as its untyped App.jsx twin.
 *
 * @param src  The entry's source.
 * @param opts  compileSource's options: its filename (or `ts`) decides whether it is TypeScript.
 *
 * @returns The parsed file.
 */
export function parseApp(
  src: string,
  opts: Pick<CompileOptions, 'ts' | 'filename'> = {},
): t.File {
  const isTypeScript = isTsEntry(opts);
  const ast = parse(src, {
    sourceType: 'module',
    plugins: parserPlugins(isTypeScript),
  });
  if (isTypeScript) {
    stripTypeScript(ast);
  }

  return ast;
}
