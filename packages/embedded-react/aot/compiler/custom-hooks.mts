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
 * Custom hooks: `function useThing(…) {…}` in the app's file. Each call is replaced by a copy of the hook's body,
 * its parameters replaced by the call's arguments and its locals renamed for that call, so every call site gets
 * state of its own, as a child component's instance does. A name the call binds takes the local the hook returns
 * under it, so `const {count, inc} = useCounter(0)` reads the hook's own state as `count`.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {traverse, types} from '@babel/core';
import {aotError} from './diagnostics.mts';
import {isFn} from './collect.mts';
import type {NodePath} from '@babel/core';
import type * as t from '@babel/types';
import type {AotError} from './diagnostics.mts';
import type {FunctionNode} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** A custom hook defined in the module. */
interface Hook {
  functionNode: FunctionNode;
  /** The module-level statement that defines it, removed once every call is inlined. */
  statement: t.Statement;
}

/** A hook call at the top of a function body. */
interface CallSite {
  hook: string;
  call: t.CallExpression;
  /** What the result is bound to, or null for a call made as a statement. */
  target: t.LVal | null;
  kind: t.VariableDeclaration['kind'];
}

/** A name the call binds and the value of the hook's result it takes. */
interface Binding {
  name: string;
  /** The value, or null when the result has none under that name. */
  value: t.Expression | null;
  /** The default the pattern gives it, written in the caller. */
  fallback?: t.Expression;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Whether a name is a hook's, as React names them: `use` and then a capital or a digit.
 *
 * @param name  The name.
 *
 * @returns Whether it is.
 */
const isHookName = (name: string): boolean => /^use[A-Z0-9]/.test(name);

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
 * The module's custom hooks, by name: function declarations and functions held by a `const`, exported or not.
 *
 * @param program  The module.
 *
 * @returns The hooks.
 */
function hooksOf(program: t.Program): Map<string, Hook> {
  const hooks = new Map<string, Hook>();
  for (const statement of program.body) {
    const declaration =
      statement.type === 'ExportNamedDeclaration' ||
      statement.type === 'ExportDefaultDeclaration'
        ? statement.declaration
        : statement;
    if (declaration?.type === 'FunctionDeclaration') {
      if (declaration.id && isHookName(declaration.id.name)) {
        hooks.set(declaration.id.name, {
          functionNode: declaration,
          statement,
        });
      }
    } else if (declaration?.type === 'VariableDeclaration') {
      for (const declarator of declaration.declarations) {
        if (
          declarator.id.type === 'Identifier' &&
          isHookName(declarator.id.name) &&
          isFn(declarator.init)
        ) {
          if (declaration.declarations.length > 1) {
            throw refusal(
              `AOT: hook "${declarator.id.name}" must be declared on its own`,
              `give it a declaration of its own: const ${declarator.id.name} = (…) => {…};`,
              declarator,
            );
          }
          hooks.set(declarator.id.name, {
            functionNode: declarator.init,
            statement,
          });
        }
      }
    }
  }

  // An arrow with an expression body returns it.
  for (const {functionNode} of hooks.values()) {
    if (functionNode.body.type !== 'BlockStatement') {
      functionNode.body = types.blockStatement([
        types.returnStatement(functionNode.body),
      ]);
    }
  }

  return hooks;
}

/**
 * The module's functions that are not hooks, which may call hooks: its components among them.
 *
 * @param program  The module.
 * @param hooks  The module's hooks.
 *
 * @returns The functions, with their names.
 */
function callersOf(
  program: t.Program,
  hooks: Map<string, Hook>,
): [string, FunctionNode][] {
  const callers: [string, FunctionNode][] = [];
  for (const statement of program.body) {
    const declaration =
      statement.type === 'ExportNamedDeclaration' ||
      statement.type === 'ExportDefaultDeclaration'
        ? statement.declaration
        : statement;
    if (declaration?.type === 'FunctionDeclaration') {
      const name = declaration.id?.name ?? 'default';
      if (!hooks.has(name)) callers.push([name, declaration]);
    } else if (declaration?.type === 'VariableDeclaration') {
      for (const declarator of declaration.declarations) {
        const name = (declarator.id as t.Identifier).name;
        if (isFn(declarator.init) && !hooks.has(name)) {
          callers.push([name, declarator.init]);
        }
      }
    }
  }

  return callers;
}

/**
 * The path of a function, in a file of its own so its scopes are worked out afresh. Names from outside it have no
 * binding there.
 *
 * @param functionNode  The function.
 *
 * @returns Its path.
 */
function functionPath(functionNode: FunctionNode): NodePath<FunctionNode> {
  traverse.cache.clear();
  const file = types.file(
    types.program([
      functionNode.type === 'FunctionDeclaration'
        ? functionNode
        : types.expressionStatement(functionNode),
    ]),
  );
  let found: NodePath<FunctionNode> | null = null;
  traverse(file, {
    Function(path) {
      if (path.node === functionNode) {
        found = path as NodePath<FunctionNode>;
        path.stop();
      }
    },
  });

  return found!;
}

/**
 * The names a node reads that it doesn't bind.
 *
 * @param path  The node's path, in a file of its own.
 *
 * @returns The names.
 */
function freeNames(path: NodePath): Set<string> {
  const names = new Set<string>();
  const add = (reference: NodePath<t.Identifier | t.JSXIdentifier>) => {
    if (!reference.scope.hasBinding(reference.node.name, true)) {
      names.add(reference.node.name);
    }
  };
  if (path.isReferencedIdentifier()) {
    add(path as NodePath<t.Identifier>);
  }
  path.traverse({ReferencedIdentifier: add});

  return names;
}

/**
 * The names an expression reads that it doesn't bind.
 *
 * @param expression  The expression.
 *
 * @returns The names.
 */
function freeNamesOfExpression(expression: t.Expression): Set<string> {
  traverse.cache.clear();
  const file = types.file(
    types.program([
      types.expressionStatement(types.cloneNode(expression, true)),
    ]),
  );
  let names = new Set<string>();
  traverse(file, {
    Program(path) {
      names = freeNames(path);
      path.stop();
    },
  });

  return names;
}

/**
 * Whether a function binds a name anywhere: itself, or a block or function inside it.
 *
 * @param path  The function.
 * @param name  The name.
 * @param gone  Its own bindings that are gone: parameters replaced by arguments.
 *
 * @returns Whether it does.
 */
function bindsAnywhere(
  path: NodePath,
  name: string,
  gone: Set<string>,
): boolean {
  if (path.scope.hasOwnBinding(name) && !gone.has(name)) return true;
  let found = false;
  path.traverse({
    Scopable(inner) {
      if (inner.scope !== path.scope && inner.scope.hasOwnBinding(name)) {
        found = true;
        inner.stop();
      }
    },
  });

  return found;
}

/**
 * The hook call a statement at the top of a function body makes: `const x = useThing(…)` or `useThing(…);`.
 *
 * @param statement  The statement.
 * @param hooks  The module's hooks.
 *
 * @returns The call, or null when it makes none.
 */
function callSiteOf(
  statement: t.Statement,
  hooks: Map<string, Hook>,
): CallSite | null {
  const hookOf = (node: t.Node | null | undefined): string | null =>
    node?.type === 'CallExpression' &&
    node.callee.type === 'Identifier' &&
    hooks.has(node.callee.name)
      ? node.callee.name
      : null;

  if (statement.type === 'ExpressionStatement') {
    const hook = hookOf(statement.expression);
    return hook
      ? {
          hook,
          call: statement.expression as t.CallExpression,
          target: null,
          kind: 'const',
        }
      : null;
  }

  if (
    statement.type === 'VariableDeclaration' &&
    statement.declarations.length === 1
  ) {
    const [declarator] = statement.declarations;
    const hook = hookOf(declarator.init);
    return hook
      ? {
          hook,
          call: declarator.init as t.CallExpression,
          target: declarator.id as t.LVal,
          kind: statement.kind,
        }
      : null;
  }

  return null;
}

/**
 * The name a pattern element binds, and its default.
 *
 * @param element  The element.
 * @param hook  The hook's name, for the error.
 *
 * @returns The name and default.
 */
function patternName(
  element: t.Node,
  hook: string,
): {name: string; fallback?: t.Expression} {
  if (element.type === 'Identifier') return {name: element.name};
  if (
    element.type === 'AssignmentPattern' &&
    element.left.type === 'Identifier'
  ) {
    return {name: element.left.name, fallback: element.right};
  }

  throw refusal(
    `AOT: the result of hook "${hook}" can only be destructured one level deep`,
    `bind each part to a name: const {a, b} = ${hook}(…), then destructure a and b on their own.`,
    element,
  );
}

/**
 * The value an object the hook returns has under a key.
 *
 * @param object  The object.
 * @param key  The key.
 * @param hook  The hook's name, for the error.
 *
 * @returns The value, or null when it has none.
 */
function valueOfKey(
  object: t.ObjectExpression,
  key: string,
  hook: string,
): t.Expression | null {
  for (const property of object.properties) {
    if (property.type === 'SpreadElement') {
      throw refusal(
        `AOT: hook "${hook}" returns an object with a spread`,
        'list each value the hook returns: return {a, b}.',
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
      return types.functionExpression(
        null,
        property.params,
        property.body,
        property.generator,
        property.async,
      );
    }

    return property.value as t.Expression;
  }

  return null;
}

/**
 * The key of a pattern's property.
 *
 * @param property  The property.
 * @param hook  The hook's name, for the error.
 *
 * @returns The key.
 */
function patternKey(property: t.ObjectProperty, hook: string): string {
  if (!property.computed && property.key.type === 'Identifier') {
    return property.key.name;
  }
  if (property.key.type === 'StringLiteral') return property.key.value;

  throw refusal(
    `AOT: the result of hook "${hook}" can only be destructured by name`,
    `name each key: const {a, b} = ${hook}(…).`,
    property,
  );
}

/**
 * Replaces a read of a parameter with the argument.
 *
 * @param reference  The read.
 * @param value  The argument.
 */
function replaceReference(reference: NodePath, value: t.Expression): void {
  const parent = reference.parent;
  if (
    parent.type === 'ObjectProperty' &&
    parent.shorthand &&
    parent.value === reference.node
  ) {
    parent.shorthand = false;
  }
  reference.replaceWith(value);
}

/**
 * Inlines custom hooks into the module's functions. Each call at the top of a component or hook body becomes the
 * hook's statements, renamed for the call, and the names the call binds read what the hook returns. A hook called
 * anywhere else is refused, and the hooks' declarations are dropped.
 *
 * @param ast  The parsed module, rewritten in place.
 */
export function inlineCustomHooks(ast: t.File): void {
  const program = ast.program;
  const hooks = hooksOf(program);
  if (!hooks.size) return;

  // How many times each hook has been inlined, which names its locals at the next call.
  const counts = new Map<string, number>();
  const expanded = new Set<string>();

  /**
   * Inlines one call: the statements it is replaced by.
   *
   * @param caller  The function making the call.
   * @param callerName  Its name, for errors.
   * @param site  The call.
   *
   * @returns The statements.
   */
  const inlineCall = (
    caller: FunctionNode,
    callerName: string,
    site: CallSite,
  ): t.Statement[] => {
    const {hook: name, call, target} = site;
    const count = (counts.get(name) ?? 0) + 1;
    counts.set(name, count);
    const suffix = `@${name}#${count}`;

    const callerPath = functionPath(caller);
    const callerScope = callerPath.scope;

    // A name the hook reads from the module must mean the same where it is inlined.
    const copy = types.cloneNode(hooks.get(name)!.functionNode, true);
    const hookPath = functionPath(copy);
    for (const freeName of freeNames(hookPath)) {
      if (callerScope.hasOwnBinding(freeName)) {
        throw refusal(
          `AOT: hook "${name}" reads "${freeName}", which ${callerName} declares too`,
          `the hook is inlined into ${callerName}, where its own "${freeName}" would hide the one ` +
            `${name} reads. Rename one of them.`,
          call,
        );
      }
    }

    // The hook returns once, at its end, since its statements run on into the caller's.
    const statements = (copy.body as t.BlockStatement).body;
    const last = statements.at(-1);
    hookPath.traverse({
      ReturnStatement(path) {
        if (path.getFunctionParent()?.node === copy && path.node !== last) {
          throw refusal(
            `AOT: hook "${name}" returns before its end`,
            'a custom hook is inlined where it is called, so it can only return as its last ' +
              'statement. Work the result out in a variable and return that.',
            path.node,
          );
        }
      },
    });
    const returned =
      last?.type === 'ReturnStatement' ? (last.argument ?? null) : null;
    const body =
      last?.type === 'ReturnStatement' ? statements.slice(0, -1) : statements;

    // Every local of the hook gets a name of its own for this call.
    const locals = new Set<string>();
    for (const local of Object.keys(hookPath.scope.bindings)) {
      if (hookPath.scope.bindings[local].kind === 'param') continue;
      hookPath.scope.rename(local, local + suffix);
      locals.add(local + suffix);
    }

    // Each read of a parameter reads the argument instead, as the caller has it.
    copy.params.forEach((parameter, index) => {
      const {name: parameterName, fallback} = (() => {
        if (
          parameter.type === 'Identifier' ||
          (parameter.type === 'AssignmentPattern' &&
            parameter.left.type === 'Identifier')
        ) {
          return patternName(parameter, name);
        }
        throw refusal(
          `AOT: hook "${name}" can only take named parameters`,
          `name each one, a default allowed: function ${name}(a, b = 1). Destructure inside the hook.`,
          parameter,
        );
      })();
      const argument = call.arguments[index];
      if (
        argument &&
        argument.type !== 'SpreadElement' &&
        !types.isExpression(argument)
      ) {
        throw refusal(
          `AOT: unsupported argument to hook "${name}"`,
          'pass each argument as a value.',
          argument,
        );
      }
      if (argument?.type === 'SpreadElement') {
        throw refusal(
          `AOT: a spread argument to hook "${name}" is not supported`,
          `pass each argument on its own: ${name}(a, b).`,
          argument,
        );
      }

      const binding = hookPath.scope.getOwnBinding(parameterName)!;
      if (!binding.constant) {
        throw refusal(
          `AOT: hook "${name}" sets its parameter "${parameterName}"`,
          'the parameter is replaced by the argument, so copy it into a local of its own first: ' +
            `let value = ${parameterName};`,
          binding.constantViolations[0].node,
        );
      }

      // A local of the hook's must not take the place of a name the argument reads.
      const value: t.Expression = (argument as t.Expression | undefined) ??
        fallback ?? {type: 'Identifier', name: 'undefined'};
      const argumentNames = argument
        ? freeNamesOfExpression(argument as t.Expression)
        : new Set<string>();
      for (const reference of binding.referencePaths) {
        for (const argumentName of argumentNames) {
          const inner = reference.scope.getBinding(argumentName);
          if (
            inner &&
            !(inner.kind === 'param' && inner.scope === hookPath.scope)
          ) {
            throw refusal(
              `AOT: an argument to hook "${name}" reads "${argumentName}", which ${name} declares too`,
              `the argument is read inside ${name}, where its own "${argumentName}" would hide ` +
                "the caller's. Rename one of them.",
              argument,
            );
          }
        }
        replaceReference(reference, types.cloneNode(value, true));
      }
    });
    const parameterNames = new Set(
      copy.params.map(parameter =>
        parameter.type === 'AssignmentPattern'
          ? (parameter.left as t.Identifier).name
          : (parameter as t.Identifier).name,
      ),
    );
    copy.params = [];

    // The names the call binds, and the values of the result each takes.
    const bindings: Binding[] = [];
    let direct = false;
    // `result.key` reads, for a result bound to one name, replaced by the name each key is bound to.
    const memberReads = new Map<string, NodePath<t.MemberExpression>[]>();
    if (target && !returned) {
      throw refusal(
        `AOT: hook "${name}" returns nothing`,
        `call it as a statement: ${name}(…);`,
        target,
      );
    } else if (
      !target ||
      !returned ||
      returned.type === 'CallExpression' ||
      isFn(returned)
    ) {
      direct = Boolean(target);
    } else if (target.type === 'Identifier') {
      if (returned.type === 'ObjectExpression') {
        // Each read must name a key, as `result.key`.
        const binding = callerScope.getOwnBinding(target.name)!;
        for (const reference of binding.referencePaths) {
          const member = reference.parentPath;
          const isRead =
            member?.isMemberExpression() &&
            member.node.object === reference.node &&
            (!member.node.computed ||
              member.node.property.type === 'StringLiteral') &&
            !(
              member.parentPath.isAssignmentExpression() &&
              member.parentPath.node.left === member.node
            ) &&
            !member.parentPath.isUpdateExpression();
          if (!isRead) {
            throw refusal(
              `AOT: the result of hook "${name}" can only be read by key`,
              `read it as ${target.name}.key, or destructure it: const {a, b} = ${name}(…).`,
              reference.node,
            );
          }
          const property = (member as NodePath<t.MemberExpression>).node
            .property;
          const key =
            property.type === 'StringLiteral'
              ? property.value
              : (property as t.Identifier).name;
          memberReads.set(key, [
            ...(memberReads.get(key) ?? []),
            member as NodePath<t.MemberExpression>,
          ]);
        }
        for (const key of memberReads.keys()) {
          bindings.push({
            name: `${target.name}:${key}`,
            value: valueOfKey(returned, key, name),
          });
        }
      } else {
        bindings.push({name: target.name, value: returned});
      }
    } else if (target.type === 'ArrayPattern') {
      if (returned.type !== 'ArrayExpression') {
        direct = true;
      } else {
        target.elements.forEach((element, index) => {
          if (!element) return;
          if (element.type === 'RestElement') {
            throw refusal(
              `AOT: a rest element can't take the result of hook "${name}"`,
              'name each element: const [a, b] = …',
              element,
            );
          }
          const value = returned.elements[index] ?? null;
          if (value?.type === 'SpreadElement') {
            throw refusal(
              `AOT: hook "${name}" returns an array with a spread`,
              'list each value the hook returns: return [a, b].',
              value,
            );
          }
          bindings.push({...patternName(element, name), value});
        });
      }
    } else if (target.type === 'ObjectPattern') {
      if (returned.type !== 'ObjectExpression') {
        direct = true;
      } else {
        for (const property of target.properties) {
          if (property.type === 'RestElement') {
            throw refusal(
              `AOT: a rest element can't take the result of hook "${name}"`,
              'name each key: const {a, b} = …',
              property,
            );
          }
          bindings.push({
            ...patternName(property.value, name),
            value: valueOfKey(returned, patternKey(property, name), name),
          });
        }
      }
    } else {
      direct = true;
    }

    // A name the call binds takes the hook's local it is bound to, when no name in the hook would clash.
    const hookFreeNames = freeNames(hookPath);
    const claimed = new Set<string>();
    const remaining = bindings.filter(binding => {
      const local = binding.value;
      if (
        local?.type !== 'Identifier' ||
        !locals.has(local.name) ||
        claimed.has(local.name) ||
        hookFreeNames.has(binding.name) ||
        bindsAnywhere(hookPath, binding.name, parameterNames)
      ) {
        return true;
      }
      claimed.add(local.name);
      hookPath.scope.rename(local.name, binding.name);
      return false;
    });

    // The others are declared after the hook's statements, or read as the name they stand for.
    const declarations: t.Statement[] = [];
    const declare = (pattern: t.LVal, init: t.Expression) => {
      declarations.push({
        type: 'VariableDeclaration',
        kind: site.kind,
        declarations: [{type: 'VariableDeclarator', id: pattern, init}],
        loc: call.loc,
      } as t.VariableDeclaration);
    };
    const aliases = new Map<string, string>();
    for (const {name: bound, value, fallback} of remaining) {
      const init = value ?? fallback;
      if (!init) {
        throw refusal(
          `AOT: hook "${name}" returns no "${bound.split(':').at(-1)}"`,
          'read only the values the hook returns, or give the name a default.',
          target,
        );
      }
      if (init.type === 'Identifier') {
        aliases.set(bound, init.name);
      } else if (isFn(init) || init.type === 'CallExpression') {
        declare({type: 'Identifier', name: bound}, init);
      } else {
        // Any other value is worked out where it is read, as a memo is.
        declare(
          {type: 'Identifier', name: bound},
          types.callExpression(types.identifier('useMemo'), [
            types.arrowFunctionExpression([], init),
            types.arrayExpression([]),
          ]),
        );
      }
    }
    if (direct) {
      declare(target!, returned!);
    } else if (!target && returned?.type === 'CallExpression') {
      declarations.push(types.expressionStatement(returned));
    }

    // In the caller, `result.key` reads the name its key is bound to, and an alias reads what it stands for.
    for (const [key, reads] of memberReads) {
      const bound = `${(target as t.Identifier).name}:${key}`;
      for (const read of reads) {
        read.replaceWith(types.identifier(aliases.get(bound) ?? bound));
      }
    }
    for (const [alias, original] of aliases) {
      if (alias.includes(':')) continue;
      const binding = callerScope.getOwnBinding(alias)!;
      for (const reference of binding.referencePaths) {
        if (
          reference.scope.getBinding(original) !==
          callerScope.getBinding(original)
        ) {
          throw refusal(
            `AOT: "${alias}" stands for "${original}", which a function in ${callerName} declares too`,
            'rename the inner one.',
            reference.node,
          );
        }
      }
      callerScope.rename(alias, original);
    }

    return [...body, ...declarations];
  };

  /**
   * Inlines the hook calls at the top of a function's body, the hooks' own calls first.
   *
   * @param functionNode  The function.
   * @param functionName  Its name, for errors.
   * @param stack  The hooks being expanded, so one that calls itself is refused.
   */
  const expandBody = (
    functionNode: FunctionNode,
    functionName: string,
    stack: string[],
  ): void => {
    if (functionNode.body.type !== 'BlockStatement') return;
    const body = functionNode.body.body;
    for (let index = 0; index < body.length; index++) {
      // A declaration of several names is split, so each hook call has a declaration of its own.
      const statement = body[index];
      if (
        statement.type === 'VariableDeclaration' &&
        statement.declarations.length > 1 &&
        statement.declarations.some(declarator =>
          callSiteOf({...statement, declarations: [declarator]}, hooks),
        )
      ) {
        body.splice(
          index,
          1,
          ...statement.declarations.map(declarator => ({
            ...statement,
            declarations: [declarator],
          })),
        );
      }

      const site = callSiteOf(body[index], hooks);
      if (!site) continue;
      expandHook(site.hook, stack, site.call);
      const statements = inlineCall(functionNode, functionName, site);
      body.splice(index, 1, ...statements);
      index += statements.length - 1;
    }
  };

  /**
   * Inlines the hook calls a hook makes, once.
   *
   * @param name  The hook.
   * @param stack  The hooks being expanded.
   * @param call  The call that reached it, for the error.
   */
  const expandHook = (name: string, stack: string[], call?: t.Node): void => {
    if (expanded.has(name)) return;
    if (stack.includes(name)) {
      throw refusal(
        `AOT: hook "${name}" calls itself`,
        "a custom hook is inlined where it is called, so it can't call itself, directly or through " +
          'another hook.',
        call,
      );
    }
    expandBody(hooks.get(name)!.functionNode, name, [...stack, name]);
    expanded.add(name);
  };

  for (const name of hooks.keys()) {
    expandHook(name, []);
  }
  for (const [name, functionNode] of callersOf(program, hooks)) {
    expandBody(functionNode, name, []);
  }

  // Every call has been inlined, so the declarations go; a hook read anywhere else is refused.
  const definitions = new Set([...hooks.values()].map(hook => hook.statement));
  program.body = program.body.filter(statement => !definitions.has(statement));
  traverse.cache.clear();
  traverse(ast, {
    ReferencedIdentifier(path) {
      const hookName = path.node.name;
      if (hooks.has(hookName) && !path.scope.hasBinding(hookName, true)) {
        throw refusal(
          `AOT: hook "${hookName}" must be called at the top of a component or another hook`,
          'as React requires, call it on its own line at the top of the function body: ' +
            `const value = ${hookName}(…). Flow B inlines it there.`,
          path.node,
        );
      }
    },
  });
  traverse.cache.clear();
}
