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

/*
 * Contexts, worked out at compile time: `const Theme = createContext(…)`, `<Theme.Provider value={…}>` and
 * `useContext(Theme)`. Each component instance is inlined where it is rendered, so the nearest provider above it is
 * known, and a value read from the context binds as a prop given that provider's value would. With no provider above,
 * it reads the context's default.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {traverse} from '@babel/core';
import {aotError} from './diagnostics.mts';
import {evalStatic} from './static-eval.mts';
import {attrExpr} from './style-text.mts';
import {bindProp, propDescriptorOf} from './control-flow.mts';
import type * as t from '@babel/types';
import type {AotError} from './diagnostics.mts';
import type {PropDescriptor, PropTables} from './control-flow.mts';
import type {Env, Scope, StateTable} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** A context the module makes with createContext. */
export interface ContextRecord {
  name: string;
  /** The default it was made with, or null for none. */
  value: t.Expression | null;
}

/** The module's contexts, and the environment their defaults are read in. */
export interface Contexts {
  records: Map<string, ContextRecord>;
  defaults: Env;
}

/** A `<C.Provider value={…}>` around what is being lowered: its value, and where it is written. */
export interface Provider {
  value: t.Expression | null;
  scope: Scope;
  environment: Env;
  state: StateTable;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * An error located at a node.
 *
 * @param message  The message.
 * @param hint  How to fix it.
 * @param node  Where it is.
 *
 * @returns The error.
 */
function refusal(
  message: string,
  hint: string,
  node: t.Node | null | undefined,
): AotError {
  const error = aotError(message, hint);
  if (node?.loc) {
    error.aotLoc = node.loc.start;
  }
  return error;
}

/**
 * The name a call is made to, `React.` stripped: `useContext(…)` and `React.useContext(…)` are both `useContext`.
 *
 * @param node  Any node.
 *
 * @returns The name, or null when it isn't a call to a name.
 */
function calleeName(node: t.Node | null | undefined): string | null {
  if (node?.type !== 'CallExpression') return null;
  const callee = node.callee;
  if (callee.type === 'Identifier') return callee.name;
  if (
    callee.type === 'MemberExpression' &&
    !callee.computed &&
    callee.object.type === 'Identifier' &&
    callee.object.name === 'React' &&
    callee.property.type === 'Identifier'
  ) {
    return callee.property.name;
  }

  return null;
}

/**
 * Whether a node is a `useContext(…)` call, whose value bindContextUses binds.
 *
 * @param node  Any node.
 *
 * @returns Whether it is.
 */
export const isContextRead = (node: t.Node | null | undefined): boolean =>
  calleeName(node) === 'useContext';

/**
 * The module's contexts: each module-level `const C = createContext(default)`.
 *
 * @param program  The module.
 *
 * @returns The contexts, by name.
 */
export function contextsOf(program: t.Program): Map<string, ContextRecord> {
  const contexts = new Map<string, ContextRecord>();
  for (const statement of program.body) {
    const declaration =
      statement.type === 'ExportNamedDeclaration'
        ? statement.declaration
        : statement;
    if (declaration?.type !== 'VariableDeclaration') continue;
    for (const declarator of declaration.declarations) {
      if (calleeName(declarator.init) !== 'createContext') continue;
      if (declarator.id.type !== 'Identifier') {
        throw refusal(
          'AOT: a context must be bound to a name',
          'const Theme = createContext(…);',
          declarator,
        );
      }
      const argument = (declarator.init as t.CallExpression).arguments[0];
      contexts.set(declarator.id.name, {
        name: declarator.id.name,
        value: (argument as t.Expression | undefined) ?? null,
      });
    }
  }

  return contexts;
}

/**
 * Refuses a context's name where a binding of that name hides it, in `useContext(C)` or `<C.Provider>`, since contexts
 * are found by name.
 *
 * @param ast  The parsed module.
 */
export function refuseHiddenContexts(ast: t.File): void {
  const names = new Set(contextsOf(ast.program).keys());
  if (!names.size) return;

  const check = (
    name: string,
    scope: {getBinding(name: string): {scope: {block: t.Node}} | undefined},
    node: t.Node,
  ) => {
    if (!names.has(name)) return;
    if (scope.getBinding(name)?.scope.block.type === 'Program') return;
    throw refusal(
      `AOT: "${name}" here is not the context: a binding of that name hides it`,
      `rename the binding, so ${name} names the context made with createContext.`,
      node,
    );
  };

  traverse.cache.clear();
  traverse(ast, {
    JSXOpeningElement(path) {
      const name = path.node.name;
      if (
        name.type === 'JSXMemberExpression' &&
        name.object.type === 'JSXIdentifier'
      ) {
        check(name.object.name, path.scope, path.node);
      }
    },
    CallExpression(path) {
      const argument = path.node.arguments[0];
      if (isContextRead(path.node) && argument?.type === 'Identifier') {
        check(argument.name, path.scope, argument);
      }
    },
  });
  traverse.cache.clear();
}

/**
 * The context a `<C.Provider>` provides. `<C.Consumer>` is refused.
 *
 * @param element  Any element.
 * @param environment  The expression environment.
 *
 * @returns The context's name, or null when the element isn't a provider.
 */
export function providedContextOf(
  element: t.JSXElement,
  environment: Env,
): string | null {
  const name = element.openingElement.name;
  if (
    name.type !== 'JSXMemberExpression' ||
    name.object.type !== 'JSXIdentifier' ||
    !environment.contexts?.records.has(name.object.name)
  ) {
    return null;
  }
  if (name.property.name === 'Provider') return name.object.name;

  throw refusal(
    `AOT: <${name.object.name}.${name.property.name}> is not supported`,
    `read the context with useContext(${name.object.name}) at the top of the component instead.`,
    element,
  );
}

/**
 * The environment inside a `<C.Provider value={…}>`: what is lowered there reads C from it.
 *
 * @param element  The provider.
 * @param context  The context it provides.
 * @param scope  Compile-time constants in reach.
 * @param environment  The expression environment around it.
 * @param state  The component's state table.
 *
 * @returns The environment.
 */
export function providedEnvironment(
  element: t.JSXElement,
  context: string,
  scope: Scope,
  environment: Env,
  state: StateTable,
): Env {
  let value: t.Expression | null = null;
  for (const attribute of element.openingElement.attributes) {
    if (
      attribute.type === 'JSXAttribute' &&
      attribute.name.type === 'JSXIdentifier' &&
      attribute.name.name === 'key'
    ) {
      continue;
    }
    if (
      attribute.type !== 'JSXAttribute' ||
      attribute.name.type !== 'JSXIdentifier' ||
      attribute.name.name !== 'value'
    ) {
      throw refusal(
        `AOT: <${context}.Provider> takes only a value`,
        `<${context}.Provider value={…}>.`,
        attribute,
      );
    }
    value = attrExpr(attribute) as t.Expression;
  }

  const providers = new Map(environment.providers);
  providers.set(context, {value, scope, environment, state});
  return {...environment, providers};
}

/**
 * The provider of a context in reach, or the context's default when there is none.
 *
 * @param context  The context.
 * @param environment  The expression environment.
 *
 * @returns The provider.
 */
function providerOf(context: ContextRecord, environment: Env): Provider {
  const provider = environment.providers?.get(context.name);
  if (provider) return provider;

  const defaults = environment.contexts!.defaults;
  return {
    value: context.value,
    scope: defaults.constants,
    environment: defaults,
    state: {byName: new Map(), bySetter: new Map()},
  };
}

/**
 * The descriptor of a value a provider gives, worked out where the provider is written.
 *
 * @param name  The name it is bound to.
 * @param node  The value, or null for none.
 * @param provider  The provider.
 *
 * @returns The descriptor.
 */
function describe(
  name: string,
  node: t.Expression | null,
  provider: Provider,
): PropDescriptor {
  if (!node) return {static: true, value: undefined};
  if (node.type === 'Identifier' && provider.state.bySetter.has(node.name)) {
    throw refusal(
      `AOT: a context's value can't hold the setter "${node.name}"`,
      `give it a function that calls the setter instead: {toggle: () => ${node.name}(…)}.`,
      node,
    );
  }

  return propDescriptorOf(name, node, provider.scope, provider.environment);
}

/**
 * The value an object a provider gives has under a key.
 *
 * @param object  The object.
 * @param key  The key.
 * @param context  The context's name, for the error.
 *
 * @returns The value, or null when it has none.
 */
function valueOfKey(
  object: t.ObjectExpression,
  key: string,
  context: string,
): t.Expression | null {
  for (const property of object.properties) {
    if (property.type === 'SpreadElement') {
      throw refusal(
        `AOT: the value of context "${context}" is an object with a spread`,
        'list each value: value={{theme, toggle}}.',
        property,
      );
    }
    const name = property.computed
      ? property.key.type === 'StringLiteral'
        ? property.key.value
        : null
      : property.key.type === 'Identifier'
        ? property.key.name
        : property.key.type === 'StringLiteral'
          ? property.key.value
          : null;
    if (name !== key) continue;

    if (property.type === 'ObjectMethod') {
      return {
        type: 'FunctionExpression',
        params: property.params,
        body: property.body,
        generator: property.generator,
        async: property.async,
        loc: property.loc,
      } as t.FunctionExpression;
    }

    return property.value as t.Expression;
  }

  return null;
}

/**
 * Binds the values a component body reads with `const x = useContext(C)` or `const {a, b} = useContext(C)`, each as
 * a prop of that value from the nearest provider would be. A `useContext` call anywhere else is refused.
 *
 * @param body  The component's body.
 * @param environment  The expression environment the component is rendered in, whose providers are in reach.
 * @param tables  The component's tables, which the values are bound into.
 *
 * @returns The names bound.
 */
export function bindContextUses(
  body: t.BlockStatement,
  environment: Env,
  tables: PropTables,
): Set<string> {
  const names = new Set<string>();
  const allowed = new WeakSet<t.Node>();
  for (const statement of body.body) {
    if (statement.type !== 'VariableDeclaration') continue;
    for (const declarator of statement.declarations) {
      if (calleeName(declarator.init) !== 'useContext') continue;
      const call = declarator.init as t.CallExpression;
      allowed.add(call);

      // The argument is a context the module makes.
      const argument = call.arguments[0];
      const context =
        argument?.type === 'Identifier'
          ? environment.contexts?.records.get(argument.name)
          : undefined;
      if (!context || call.arguments.length !== 1) {
        throw refusal(
          'AOT: useContext takes a context made in this file',
          'make it at module level and pass it: const Theme = createContext(…); … useContext(Theme).',
          call,
        );
      }
      const provider = providerOf(context, environment);
      const bind = (name: string, descriptor: PropDescriptor) => {
        bindProp(
          name,
          descriptor,
          tables,
          provider.environment,
          provider.state,
        );
        names.add(name);
      };

      // `const value = useContext(C)`: the whole value.
      const pattern = declarator.id;
      if (pattern.type === 'Identifier') {
        bind(pattern.name, describe(pattern.name, provider.value, provider));
        continue;
      }
      if (pattern.type !== 'ObjectPattern') {
        throw refusal(
          `AOT: the value of context "${context.name}" can only be destructured as an object`,
          `const {a, b} = useContext(${context.name}), or const value = useContext(${context.name}).`,
          pattern,
        );
      }

      // `const {a, b: c = 1} = useContext(C)`: each key, from an object written out in the provider, or from a
      // constant one.
      const value = provider.value;
      const whole =
        value && value.type !== 'ObjectExpression'
          ? describe('value', value, provider)
          : null;
      for (const property of pattern.properties) {
        if (
          property.type === 'RestElement' ||
          (property.computed && property.key.type !== 'StringLiteral')
        ) {
          throw refusal(
            `AOT: the value of context "${context.name}" can only be destructured by key`,
            `name each key: const {a, b} = useContext(${context.name}).`,
            property,
          );
        }
        const key =
          property.key.type === 'Identifier'
            ? property.key.name
            : (property.key as t.StringLiteral).value;
        const target = property.value;
        const name =
          target.type === 'Identifier'
            ? target.name
            : target.type === 'AssignmentPattern' &&
                target.left.type === 'Identifier'
              ? target.left.name
              : null;
        if (!name) {
          throw refusal(
            `AOT: the value of context "${context.name}" can only be destructured one level deep`,
            'bind each key to a name, then destructure that name on its own.',
            target,
          );
        }

        let descriptor: PropDescriptor;
        if (whole) {
          if (
            whole.static !== true ||
            whole.value === null ||
            typeof whole.value !== 'object'
          ) {
            throw refusal(
              `AOT: the value of context "${context.name}" must be an object to destructure`,
              'give the provider an object written out in place: value={{theme, toggle}}.',
              value,
            );
          }
          descriptor = {
            static: true,
            value: (whole.value as Record<string, unknown>)[key],
          };
        } else {
          descriptor = describe(
            name,
            value
              ? valueOfKey(value as t.ObjectExpression, key, context.name)
              : null,
            provider,
          );
        }

        // A default stands in for a key the value has no value for.
        if (
          descriptor.static === true &&
          descriptor.value === undefined &&
          target.type === 'AssignmentPattern'
        ) {
          descriptor = {
            static: true,
            value: evalStatic(target.right, tables.scope, 'boot'),
          };
        }
        bind(name, descriptor);
      }
    }
  }

  // Anywhere else, useContext would read a provider this pass doesn't know.
  const walk = (node: unknown): void => {
    if (!node || typeof node !== 'object') return;
    if (Array.isArray(node)) {
      node.forEach(walk);
      return;
    }
    const astNode = node as t.Node;
    if (typeof astNode.type !== 'string') return;
    if (calleeName(astNode) === 'useContext' && !allowed.has(astNode)) {
      throw refusal(
        'AOT: useContext must be called at the top of a component',
        'bind its value on a line of its own at the top of the component: const theme = useContext(Theme).',
        astNode,
      );
    }
    for (const key of Object.keys(astNode)) {
      if (
        key === 'loc' ||
        key === 'leadingComments' ||
        key === 'trailingComments'
      ) {
        continue;
      }
      walk((astNode as unknown as Record<string, unknown>)[key]);
    }
  };
  walk(body);

  return names;
}
