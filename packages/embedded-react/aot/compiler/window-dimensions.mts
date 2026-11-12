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

/**
 * @file compiler/window-dimensions.mts
 * @brief Turns `useWindowDimensions()` and `Dimensions.get('window')` into reads of the panel's size.
 */

import {traverse} from '@babel/core';
import {PANEL_KEY} from './boot.mts';
import {aotError} from './diagnostics.mts';
import type {NodePath} from '@babel/core';
import type * as t from '@babel/types';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** The package the two are imported from. */
const PACKAGE = 'embedded-react';

/** The sizes `Dimensions.get` names, which are the same on a panel. */
const SIZES = new Set(['window', 'screen']);

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * The package export a name stands for where it is read, or null when it is anything else: a local, a prop or a
 * state of that name hides the import.
 *
 * @param path  Where the name is read.
 * @param node  The name.
 *
 * @returns The export's name, or null.
 */
function packageExportOf(path: NodePath, node: t.Node): string | null {
  if (node.type !== 'Identifier') return null;
  const binding = path.scope.getBinding(node.name);
  const specifier = binding?.path.node;
  const declaration = binding?.path.parent;
  if (
    specifier?.type !== 'ImportSpecifier' ||
    declaration?.type !== 'ImportDeclaration' ||
    declaration.source.value !== PACKAGE
  ) {
    return null;
  }

  const {imported} = specifier;
  return imported.type === 'Identifier' ? imported.name : imported.value;
}

/**
 * Rewrites each `useWindowDimensions()` and `Dimensions.get('window' | 'screen')` to a read of the panel's size,
 * which folds as `screen` does. Only the package's own exports are rewritten, by whatever name they are imported.
 *
 * @param ast  The parsed module, rewritten in place.
 *
 * @throws An AotError for any other use of `Dimensions`.
 */
export function rewriteWindowDimensions(ast: t.File): void {
  const panel = (node: t.Node): t.Identifier =>
    ({type: 'Identifier', name: PANEL_KEY, loc: node.loc}) as t.Identifier;

  traverse.cache.clear();
  traverse(ast, {
    CallExpression(path) {
      const {callee, arguments: callArguments} = path.node;
      if (packageExportOf(path, callee) === 'useWindowDimensions') {
        path.replaceWith(panel(path.node));
      } else if (
        callee.type === 'MemberExpression' &&
        packageExportOf(path, callee.object) === 'Dimensions'
      ) {
        const [size] = callArguments;
        const isGet =
          !callee.computed &&
          callee.property.type === 'Identifier' &&
          callee.property.name === 'get';
        if (
          !isGet ||
          callArguments.length !== 1 ||
          size.type !== 'StringLiteral' ||
          !SIZES.has(size.value)
        ) {
          const error = aotError(
            "AOT: Dimensions has only get('window') and get('screen')",
            'the panel size is fixed while the app runs, so there is nothing else to read or listen to.',
          );
          if (path.node.loc) {
            error.aotLoc = path.node.loc.start;
          }
          throw error;
        }
        path.replaceWith(panel(path.node));
      }
    },
  });
  traverse.cache.clear();
}
