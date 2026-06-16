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
import { transformSync } from '@babel/core';
import syntaxJsx from '@babel/plugin-syntax-jsx';

/** Best-effort name of the function/component enclosing a path (for a stable, readable key). */
function enclosingName(path) {
  const fn = path.getFunctionParent();
  if (!fn) return '_';
  if (fn.node.id && fn.node.id.name) return fn.node.id.name; // function Foo() {}
  const parent = fn.parentPath && fn.parentPath.node;
  if (parent) {
    if (parent.type === 'VariableDeclarator' && parent.id && parent.id.name) return parent.id.name; // const Foo = () =>
    if (parent.key && parent.key.name) return parent.key.name; // method / property
    if (parent.type === 'AssignmentExpression' && parent.left && parent.left.name) return parent.left.name;
  }
  return '_';
}

/** Babel plugin: useState(init) → __erPersistState("key", init), importing the helper when used. */
function persistPlugin({ types: t }) {
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
            t.importDeclaration(
              [t.importSpecifier(t.identifier('__erPersistState'), t.identifier('usePersistentState'))],
              t.stringLiteral('embedded-react'),
            ),
          );
        },
      },
      CallExpression(path, state) {
        if (!t.isIdentifier(path.node.callee, { name: 'useState' })) return;
        // Only rewrite the real useState (an imported binding from react / embedded-react).
        const binding = path.scope.getBinding('useState');
        if (!binding || binding.kind !== 'module') return;
        const decl = binding.path.parentPath && binding.path.parentPath.node;
        const src = decl && decl.source && decl.source.value;
        if (src !== 'react' && src !== 'embedded-react') return;

        const fnName = enclosingName(path);
        const idx = state.erCounters.get(fnName) || 0;
        state.erCounters.set(fnName, idx + 1);
        const key = `${state.opts.moduleId}::${fnName}#${idx}`;

        path.replaceWith(
          t.callExpression(t.identifier('__erPersistState'), [t.stringLiteral(key), ...path.node.arguments]),
        );
        state.erUsed = true;
        path.skip();
      },
    },
  };
}

/**
 * Applies the persist transform to a module's source.
 *
 * @param {string} code      Module source (JSX allowed).
 * @param {string} moduleId  Stable module identifier (used in keys; e.g. a path relative to the app).
 * @returns {string} Transformed source (JSX preserved for esbuild to handle).
 */
export function transformPersist(code, moduleId) {
  const out = transformSync(code, {
    filename: moduleId,
    babelrc: false,
    configFile: false,
    sourceType: 'module',
    parserOpts: { plugins: ['jsx'] },
    plugins: [syntaxJsx, [persistPlugin, { moduleId }]],
  });
  return out.code;
}
