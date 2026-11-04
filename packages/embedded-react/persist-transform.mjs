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

// Simulator-only Babel transform: rewrites `useState(init)` in app code to a persisting helper so
// state transparently survives a hot reload (see tools/simulator/README.md). Plain
// `useState` from 'react' just persists in the simulator; on a device (no transform, no __erPersist)
// it's exactly useState. Only the app's own files are transformed — never the library or React (so
// the helper itself isn't rewritten).
//
// Each call is keyed by `module::component#index` (component name + its useState order), which is
// stable across the edits you make most often (JSX/logic). Adding/removing a useState in a component,
// or renaming it, shifts that component's keys and resets its state — press R in the sim for a clean
// reset any time.
import {transformSync} from '@babel/core';
import syntaxJsx from '@babel/plugin-syntax-jsx';

/** Best-effort name of the function/component enclosing a path (for a stable, readable key). */
function enclosingName(path) {
  const functionPath = path.getFunctionParent();
  if (!functionPath) return '_';
  if (functionPath.node.id && functionPath.node.id.name)
    return functionPath.node.id.name; // function Foo() {}
  const parent = functionPath.parentPath && functionPath.parentPath.node;
  if (parent) {
    if (parent.type === 'VariableDeclarator' && parent.id && parent.id.name)
      return parent.id.name; // const Foo = () =>
    if (parent.key && parent.key.name) return parent.key.name; // method / property
    if (
      parent.type === 'AssignmentExpression' &&
      parent.left &&
      parent.left.name
    )
      return parent.left.name;
  }
  return '_';
}

/** Babel plugin: useState(init) → __erPersistState("key", init), importing the helper when used. */
function persistPlugin({types}) {
  return {
    name: 'er-persist-usestate',
    visitor: {
      Program: {
        enter(_path, state) {
          state.erCounters = new Map();
          state.erUsed = false;
        },
        exit(path, state) {
          if (!state.erUsed) return;
          path.unshiftContainer(
            'body',
            types.importDeclaration(
              [
                types.importSpecifier(
                  types.identifier('__erPersistState'),
                  types.identifier('usePersistentState'),
                ),
              ],
              types.stringLiteral('embedded-react'),
            ),
          );
        },
      },
      CallExpression(path, state) {
        if (!types.isIdentifier(path.node.callee, {name: 'useState'})) return;
        // Only rewrite the real useState (an imported binding from react / embedded-react).
        const binding = path.scope.getBinding('useState');
        if (!binding || binding.kind !== 'module') return;
        const declaration =
          binding.path.parentPath && binding.path.parentPath.node;
        const source =
          declaration && declaration.source && declaration.source.value;
        if (source !== 'react' && source !== 'embedded-react') return;

        const functionName = enclosingName(path);
        const index = state.erCounters.get(functionName) || 0;
        state.erCounters.set(functionName, index + 1);
        const key = `${state.opts.moduleId}::${functionName}#${index}`;

        path.replaceWith(
          types.callExpression(types.identifier('__erPersistState'), [
            types.stringLiteral(key),
            ...path.node.arguments,
          ]),
        );
        state.erUsed = true;
        path.skip();
      },
    },
  };
}

/**
 * Whether `absolutePath` is an app source file that should receive the persist transform: under the project
 * root, but NOT a dependency. The library itself lives under the project root in a consumer install
 * (`<project>/node_modules/embedded-react`), and transforming it would rewrite the `useState` *inside*
 * `usePersistentState` into a call to `usePersistentState` — infinite self-recursion → stack overflow.
 * Excluding `node_modules` is what keeps the helper (and react) untransformed in a published install,
 * not just in the monorepo (where the library happens to sit outside the demo's project root).
 *
 * @param {string} absolutePath           Absolute path of the module esbuild is loading.
 * @param {string} projectRootNormalized  Project root, forward-slash normalized.
 * @returns {boolean}
 */
export function shouldPersist(absolutePath, projectRootNormalized) {
  const normalizedPath = absolutePath.replace(/\\/g, '/');
  return (
    normalizedPath.startsWith(projectRootNormalized) &&
    !normalizedPath.includes('/node_modules/')
  );
}

/**
 * Applies the persist transform to a module's source.
 *
 * TypeScript entries (.ts/.tsx) are parsed with the `typescript` plugin enabled; the type syntax is
 * preserved in the output and stripped downstream by esbuild's ts/tsx loader (see sim-server.mjs).
 *
 * @param {string} code      Module source (JSX and/or TypeScript allowed).
 * @param {string} moduleId  Stable module identifier (used in keys; e.g. a path relative to the app).
 * @returns {string} Transformed source (JSX/TS preserved for esbuild to handle).
 */
export function transformPersist(code, moduleId) {
  // Enable the babel parser plugins this module's extension needs. `.tsx` is jsx+typescript; `.ts` is
  // typescript-only (the jsx plugin would misparse `<T>` type assertions); everything else is jsx.
  const isTs = /\.tsx?$/.test(moduleId);
  const isTsx = /\.tsx$/.test(moduleId);
  const parserPlugins = isTsx
    ? ['jsx', 'typescript']
    : isTs
      ? ['typescript']
      : ['jsx'];
  const result = transformSync(code, {
    filename: moduleId,
    babelrc: false,
    configFile: false,
    sourceType: 'module',
    parserOpts: {plugins: parserPlugins},
    // syntaxJsx only enables jsx parsing; for ts/tsx we drive the parser via parserOpts above instead.
    plugins: [...(isTs ? [] : [syntaxJsx]), [persistPlugin, {moduleId}]],
  });
  return result.code;
}
