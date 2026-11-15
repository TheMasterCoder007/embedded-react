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
 * @file compiler/keyboard-dismiss.mts
 * @brief Finds each `Keyboard.dismiss()`, by the package's own export, and refuses any other use of Keyboard.
 */

import {traverse} from '@babel/core';
import {aotError} from './diagnostics.mts';
import type {NodePath} from '@babel/core';
import type * as t from '@babel/types';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** The package Keyboard is imported from. */
const PACKAGE = 'embedded-react';

/** What a `Keyboard.dismiss()` is rewritten to call. Not a name JS allows, so no binding of the app's is one. */
export const KEYBOARD_DISMISS_CALL = '#Keyboard.dismiss';

const HINT =
  'Keyboard.dismiss() is the one Keyboard call a package has; to follow the keyboard, set state in the ' +
  "<TextInput>'s onFocus and onBlur.";

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Whether a name stands for the package's Keyboard where it is read. A local, a prop or a state of that name hides
 * the import.
 *
 * @param path  Where the name is read.
 *
 * @returns Whether it does.
 */
function isKeyboard(path: NodePath<t.Identifier>): boolean {
  const binding = path.scope.getBinding(path.node.name);
  const specifier = binding?.path.node;
  const declaration = binding?.path.parent;
  if (
    specifier?.type !== 'ImportSpecifier' ||
    declaration?.type !== 'ImportDeclaration' ||
    declaration.source.value !== PACKAGE
  ) {
    return false;
  }
  const {imported} = specifier;
  return (
    (imported.type === 'Identifier' ? imported.name : imported.value) ===
    'Keyboard'
  );
}

/**
 * Rewrites every `Keyboard.dismiss()` to a call of KEYBOARD_DISMISS_CALL, by whatever name Keyboard is imported.
 *
 * @param ast  The parsed file, rewritten in place.
 *
 * @throws An AotError for any other use of Keyboard.
 */
export function rewriteKeyboardDismiss(ast: t.File): void {
  traverse.cache.clear();
  traverse(ast, {
    Identifier(path) {
      if (!path.isReferencedIdentifier() || !isKeyboard(path)) return;
      const member = path.parent;
      const call = path.parentPath?.parent;
      const method =
        member.type === 'MemberExpression' &&
        member.object === path.node &&
        !member.computed &&
        member.property.type === 'Identifier' &&
        call?.type === 'CallExpression' &&
        call.callee === member
          ? member.property.name
          : null;
      if (
        method === 'dismiss' &&
        (call as t.CallExpression).arguments.length === 0
      ) {
        (call as t.CallExpression).callee = {
          type: 'Identifier',
          name: KEYBOARD_DISMISS_CALL,
          loc: member.loc,
        } as t.Identifier;
        path.skip();
        return;
      }
      const error = aotError(
        method === 'dismiss'
          ? 'AOT: Keyboard.dismiss() takes no arguments'
          : method
            ? `AOT: Keyboard.${method}() is not in the subset`
            : 'AOT: Keyboard has only Keyboard.dismiss()',
        HINT,
      );
      if (path.node.loc) error.aotLoc = path.node.loc.start;
      throw error;
    },
  });
  traverse.cache.clear();
}
