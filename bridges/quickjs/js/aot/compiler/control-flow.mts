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

import {aotError} from './diagnostics.mts';
import {evalStatic, withUndefined} from './static-eval.mts';
import {asCond, emitExprWide, emitExpr} from './expressions.mts';
import {
  isFn,
  declaredNames,
  collectState,
  componentReturnJSX,
  collectHelpers,
  collectCallbacks,
  collectMemos,
  collectEffects,
  usesState,
} from './collect.mts';
import {collectAnims, collectRefs} from './animations.mts';
import {attrExpr} from './style-text.mts';
import {compileEffect} from './handlers.mts';
import {collectPanResponders, panSpreadTarget} from './pan-responder.mts';
import {emitNode} from './nodes.mts';
import type * as t from '@babel/types';
import type {Out} from './out.mts';
import type {
  ChildrenRef,
  ChildrenSlot,
  EmitOptions,
  Env,
  FunctionNode,
  FunctionProp,
  ListState,
  Local,
  Scope,
  StateTable,
} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * A prop as a component instance receives it: a folded constant, a runtime C expression (a list row's
 * `item.field`, say), or a callback compiled where the component uses it as an event handler.
 */
type PropDescriptor =
  | {static: true; value: unknown; fn?: undefined}
  | (Local & {static: false; fn?: undefined})
  | {
      fn: true;
      node: FunctionNode;
      env?: Env;
      state?: StateTable;
      static?: undefined;
    };

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Reads a component's props as descriptors: `{static:true,value}` (folded),
 * `{static:false,code, cType, struct}` (a runtime C expression — e.g., a list row's `item.field`), or
 * `{fn:true,node}` (a callback).
 *
 * @param openingElement  The component instance's opening tag.
 * @param scope  Compile-time constants in reach at the call site.
 * @param env  The caller's expression environment.
 *
 * @returns The props, by name.
 */
function extractProps(
  openingElement: t.JSXOpeningElement,
  scope: Scope,
  env: Env,
): Record<string, PropDescriptor> {
  const props: Record<string, PropDescriptor> = {};
  for (const attr of openingElement.attributes) {
    // Static spread: {...obj} where obj folds to a compile-time object → merge its keys as props.
    if (attr.type === 'JSXSpreadAttribute') {
      // A PanResponder's handlers need a real scene node, which a component instance is not.
      if (panSpreadTarget(attr.argument, env)) {
        throw aotError(
          'AOT: a PanResponder can only be spread onto a host element',
          'spread `{...pan.panHandlers}` onto the <View> (or <Pressable>/<ScrollView>) that ' +
            'should own the gesture, not onto a component instance — the AOT wires the responder ' +
            'to a real scene node.',
        );
      }

      let spreadObject;
      try {
        spreadObject = evalStatic(attr.argument, scope);
      } catch {
        throw new Error(
          'AOT: only a compile-time-constant object can be spread to a component ({...obj})',
        );
      }

      if (spreadObject == null || typeof spreadObject !== 'object') {
        throw new Error(
          'AOT: a component spread {...x} must resolve to an object',
        );
      }

      for (const [propName, propValue] of Object.entries(spreadObject)) {
        props[propName] = {static: true, value: propValue};
      }

      continue;
    }

    // `key` is React's, not the component's.
    if (
      attr.type !== 'JSXAttribute' ||
      (attr.name as t.JSXIdentifier).name === 'key'
    ) {
      continue;
    }

    // A callback prop (inline arrow, caller's useCallback or forwarded prop) is an ` fn `, resolved at its handler.
    const node = attrExpr(attr);
    if (isFn(node)) {
      props[(attr.name as t.JSXIdentifier).name] = {fn: true, node};
      continue;
    }

    if (node.type === 'Identifier' && env.callbacks?.has(node.name)) {
      props[(attr.name as t.JSXIdentifier).name] = {
        fn: true,
        node: env.callbacks.get(node.name)!,
      };
      continue;
    }

    if (node.type === 'Identifier' && env.fnProps?.has(node.name)) {
      props[(attr.name as t.JSXIdentifier).name] = {
        fn: true,
        ...env.fnProps.get(node.name)!,
      }; // forward original {node, env, state}

      continue;
    }

    // Any other prop folds to a constant when it can, else becomes a runtime C expression.
    try {
      props[(attr.name as t.JSXIdentifier).name] = {
        static: true,
        value: evalStatic(node, scope),
      };
    } catch {
      props[(attr.name as t.JSXIdentifier).name] = {
        static: false,
        ...emitExprWide(node, env),
      };
    }
  }

  return props;
}

/**
 * Maps a component's parameter to its prop descriptors (handles destructure rename and defaults).
 *
 * @param fn  The component.
 * @param props  The props the instance was given.
 *
 * @returns The descriptor each name in the component's body is bound to.
 */
function bindParams(
  fn: FunctionNode,
  props: Record<string, PropDescriptor>,
): Map<string, PropDescriptor> {
  const bindings = new Map<string, PropDescriptor>();
  const param = fn.params[0];
  if (!param) return bindings;

  // `function C(props)`: the whole props object is one constant, so every prop must fold.
  if (param.type === 'Identifier') {
    const propsObject: Record<string, unknown> = {};
    for (const [propName, descriptor] of Object.entries(props)) {
      if (!descriptor.static) {
        throw new Error(
          'AOT: dynamic props require a destructured component parameter (e.g. `function C({ x })`)',
        );
      }

      propsObject[propName] = descriptor.value;
    }

    bindings.set(param.name, {static: true, value: propsObject});
  } else if (param.type === 'ObjectPattern') {
    // `function C({x, y: renamed, z = 1})`: bind each name to its prop, else to its folded default.
    for (const property of param.properties) {
      if (property.type === 'RestElement') {
        throw new Error(
          'AOT: rest props (...rest) in a component param not supported',
        );
      }

      // Resolve a destructured prop's source name, local binding name, and optional default value descriptor.
      const propName =
        (property.key as t.Identifier).name ??
        (property.key as t.StringLiteral).value;
      const bindName =
        property.value?.type === 'Identifier'
          ? property.value.name
          : property.value?.type === 'AssignmentPattern'
            ? (property.value.left as t.Identifier).name
            : propName;
      let descriptor = props[propName];
      if (!descriptor && property.value?.type === 'AssignmentPattern') {
        descriptor = {
          static: true,
          value: evalStatic(property.value.right, {}),
        };
      }

      bindings.set(bindName, descriptor ?? {static: true, value: undefined});
    }
  } else {
    throw new Error('AOT: unsupported component parameter pattern');
  }

  return bindings;
}

/**
 * True if `expr` is how a component body refers to its children (destructured {children} or props.children).
 *
 * @param expr  A child expression.
 * @param env  The expression environment.
 *
 * @returns Whether it names the children slot.
 */
function isChildrenRef(expr: t.Node, env: Env): boolean {
  const childrenRef = env.children?.ref;
  if (!childrenRef) return false;
  if (childrenRef.kind === 'local') {
    return expr.type === 'Identifier' && expr.name === childrenRef.name;
  }

  return (
    expr.type === 'MemberExpression' &&
    !expr.computed &&
    expr.object.type === 'Identifier' &&
    expr.object.name === childrenRef.name &&
    (expr.property as t.Identifier).name === 'children'
  );
}

/**
 * Inlines a function component instance: bind props (static → scope, dynamic → locals), emit its JSX.
 * Children passed at the call site are captured and emitted (in the CALLER's scope) where the body uses them.
 *
 * @param element  The component instance.
 * @param scope  Compile-time constants in reach at the call site.
 * @param out  Everything emitted so far.
 * @param env  The caller's expression environment.
 * @param state  The caller's state table.
 * @param opts  How the instance is placed.
 *
 * @returns The variable of the node the component's JSX lowered to.
 */
export function emitComponent(
  element: t.JSXElement,
  scope: Scope,
  out: Out,
  env: Env,
  state: StateTable,
  opts?: EmitOptions,
): string {
  // Look up the component and keep the call-site children that render something.
  const componentName = (element.openingElement.name as t.JSXIdentifier).name;
  const fn = out.components.get(componentName)!;
  const childNodes = element.children.filter(
    (child): child is t.JSXElement | t.JSXExpressionContainer =>
      child.type === 'JSXElement' ||
      (child.type === 'JSXExpressionContainer' &&
        child.expression.type !== 'JSXEmptyExpression'),
  );

  // How the body refers to children: destructured `{ children }` (a local) or whole `props` → props.children.
  const param = fn.params[0];
  let childrenRef: ChildrenRef | null = null;
  if (param?.type === 'ObjectPattern') {
    for (const property of param.properties as t.ObjectProperty[]) {
      if (
        ((property.key as t.Identifier)?.name ??
          (property.key as t.StringLiteral)?.value) === 'children'
      ) {
        childrenRef = {
          kind: 'local',
          name: (property.value as t.Identifier)?.name ?? 'children',
        };
      }
    }
  } else if (param?.type === 'Identifier') {
    childrenRef = {kind: 'props', name: param.name};
  }

  // A child sees only module scope: start from module consts with no locals, so caller bindings cannot leak in.
  const childScope: Scope = {...(env.moduleConsts ?? scope)};
  const childLocals = new Map<string, Local>();
  // Callback props resolve to the caller's function, env and state; inherit the caller's own so they forward.
  const fnProps = new Map<string, FunctionProp>(env.fnProps);

  // Bind each prop: a callback to fnProps, a constant to the child's scope, a runtime value to its locals.
  for (const [name, descriptor] of bindParams(
    fn,
    extractProps(element.openingElement, scope, env),
  )) {
    // The children binding comes from the slot, not from a value prop.
    if (childrenRef?.kind === 'local' && name === childrenRef?.name) continue;
    if (descriptor.fn) {
      fnProps.set(name, {
        node: descriptor.node,
        env: descriptor.env ?? env,
        state: descriptor.state ?? state,
      });
    } else if (descriptor.static) {
      childScope[name] = descriptor.value;
    } else {
      childLocals.set(name, {
        code: descriptor.code,
        cType: descriptor.cType,
        struct: descriptor.struct,
        isBool: descriptor.isBool, // keep boolean-ness across the prop boundary (extractProps supplies it)
      });

      // A dynamic prop is a runtime binding; drop any same-named module const so no fold emits it instead.
      delete childScope[name];
    }
  }

  // Capture the call-site children with the caller's scope and env, for wherever the body renders them.
  const children: ChildrenSlot | null = childNodes.length
    ? {nodes: childNodes, scope, env, ref: childrenRef}
    : null;

  // Each inlined instance namespaces its own hooks under a unique `c<N>_` prefix so instances stay independent.
  const prefix = `c${out.instN++}_`;

  // Fold the child body's static consts as compileSourceImpl does for App; they shadow same-named module consts.
  if (fn.body.type === 'BlockStatement') {
    // As in App: the body's own names, hook bindings included, shadow module ones before anything folds.
    for (const name of declaredNames(fn.body.body)) {
      delete childScope[name];
    }

    for (const stmt of fn.body.body) {
      if (stmt.type !== 'VariableDeclaration' || stmt.kind !== 'const') {
        continue;
      }
      for (const decl of stmt.declarations) {
        if (decl.id.type !== 'Identifier' || !decl.init) continue;
        if (childLocals.has(decl.id.name)) continue; // a dynamic prop of that name is already bound
        try {
          childScope[decl.id.name] = evalStatic(
            decl.init,
            withUndefined(childScope),
          );
        } catch {
          // A dynamic const hides any same-named module const; a memo re-binds it below, else it stays unresolved.
          delete childScope[decl.id.name];
        }
      }
    }
  }

  // Collect the child's own hooks under its prefix; its state slots are runtime bindings, so they leave the scope.
  const childAnims = collectAnims(fn.body, childScope, prefix);
  const childRefs = collectRefs(fn.body, childScope, prefix, env.wide);
  const childPans = collectPanResponders(fn.body, prefix);
  const childCallbacks = collectCallbacks(fn.body);
  const childMemos = collectMemos(fn.body);
  let childState = state;
  if (usesState(fn)) {
    childState = collectState(
      fn.body as t.BlockStatement,
      childScope,
      env.caps,
      prefix,
      env.wide,
    );
    out.childStateRecords.push(...childState.byName.values());
    for (const name of childState.byName.keys()) {
      delete childScope[name];
    }
  }

  // The child's own refs and animated values are runtime bindings too (see compileSourceImpl).
  for (const name of [...childAnims.keys(), ...childRefs.keys()]) {
    delete childScope[name];
  }
  out.childRefs.push(...childRefs.values());
  out.childAnimations.push(...childAnims.values());

  // The child's expression environment: the caller's, with every name-keyed table replaced by the child's.
  const childEnv: Env = {
    ...env,
    consts: childScope,
    locals: childLocals,
    children,
    fnProps,
    state: childState.byName,
    animations: childAnims,
    refs: childRefs,
    pans: childPans,
    callbacks: childCallbacks,
    helpers: collectHelpers(fn.body, out.program),
    cbPrefix: prefix,
  };

  // Resolve memos in order (fold, else a local C expr), then compile its effects, all in the child's env/state.
  for (const [name, expr] of childMemos) {
    try {
      childScope[name] = evalStatic(expr, childScope);
    } catch {
      const cExpr = emitExprWide(expr, childEnv);
      childLocals.set(name, {
        code: `(${cExpr.code})`,
        cType: cExpr.cType,
        isBool: cExpr.isBool,
      });
      delete childScope[name]; // a runtime binding beats a module const of its name in every fold
    }
  }

  for (const effect of collectEffects(fn.body)) {
    compileEffect(effect, childEnv, childState, out);
  }

  // Emit the JSX the component returns, in the child's own scope.
  return emitNode(
    componentReturnJSX(fn, childScope),
    childScope,
    out,
    childEnv,
    childState,
    opts,
  );
}

/**
 * Emits an element / component child and appends it to the parent. opts.displayCode toggles its show.
 *
 * @param node  The child; anything but a JSX element is refused.
 * @param parentVar  The parent node's variable.
 * @param scope  Compile-time constants in reach.
 * @param out  Everything emitted so far.
 * @param env  The expression environment.
 * @param state  The component's state table.
 * @param opts  How the child is placed.
 */
function emitElementInto(
  node: t.Node,
  parentVar: string,
  scope: Scope,
  out: Out,
  env: Env,
  state: StateTable,
  opts?: EmitOptions,
): void {
  if (node.type !== 'JSXElement') {
    throw new Error(`AOT: expected a JSX element here, got ${node.type}`);
  }

  const childNodeId = emitNode(node, scope, out, env, state, opts);
  out.build.push(`    er_tree_append_child(${parentVar}, ${childNodeId});`);
}

/**
 * Unrolls `arr.map((item, i) => <JSX/>)` over a COMPILE-TIME-CONSTANT array.
 *
 * @param call  The `.map(…)` call.
 * @param parentVar  The parent node's variable.
 * @param scope  Compile-time constants in reach.
 * @param out  Everything emitted so far.
 * @param env  The expression environment.
 * @param state  The component's state table.
 */
function emitMap(
  call: t.CallExpression,
  parentVar: string,
  scope: Scope,
  out: Out,
  env: Env,
  state: StateTable,
): void {
  // The array must fold at compile time and the callback must be inline.
  let items;
  try {
    items = evalStatic((call.callee as t.MemberExpression).object, scope);
  } catch {
    throw new Error(
      'AOT: .map target must be a compile-time-constant array (dynamic lists not yet supported)',
    );
  }

  if (!Array.isArray(items)) {
    throw new Error('AOT: .map target did not resolve to an array');
  }

  const mapFn = call.arguments[0];
  if (!isFn(mapFn)) {
    throw new Error('AOT: .map argument must be an inline function');
  }

  const itemName = (mapFn.params[0] as t.Identifier | undefined)?.name;
  const indexName = (mapFn.params[1] as t.Identifier | undefined)?.name;
  const rowJSX = componentReturnJSX(mapFn);

  // Emit one row per element, with its item and index bound as compile-time constants.
  items.forEach((item, index) => {
    const rowScope: Scope = {...scope};
    if (itemName) {
      rowScope[itemName] = item;
    }
    if (indexName) {
      rowScope[indexName] = index;
    }
    emitElementInto(
      rowJSX,
      parentVar,
      rowScope,
      out,
      rowEnv(env, [itemName, indexName], rowScope),
      state,
    );
  });
}

/**
 * The env for one `.map` row. Its callback params (item, index) shadow every outer binding of the same
 * name — a JS arrow parameter always does — so those names leave every name-keyed map (state, locals,
 * callbacks, helpers, …), and the row's own binding wins: in `consts`, or in `ownLocals` for a pooled
 * row's struct item.
 *
 * @param env  The expression environment around the `.map`.
 * @param params  The callback's parameter names (item, index), where present.
 * @param consts  The row's compile-time scope.
 * @param ownLocals  The row's runtime bindings, when its item is one (a pooled row).
 *
 * @returns The row's environment.
 */
function rowEnv(
  env: Env,
  params: (string | undefined)[],
  consts: Scope,
  ownLocals: Map<string, Local> | null = null,
): Env {
  // Copy a table without the row's names; one that binds none of them is shared as is.
  const names = params.filter(Boolean) as string[];
  const drop = <Table extends Map<string, unknown> | undefined>(
    bindings: Table,
  ): Table => {
    if (!bindings || !names.some(name => bindings.has(name))) return bindings;

    const copy = new Map(bindings);
    for (const name of names) {
      copy.delete(name);
    }

    return copy as Table;
  };

  // A row param named like the children reference (`children` or `props`) shadows it, leaving no children slot.
  return {
    ...env,
    consts,
    state: drop(env.state),
    refs: drop(env.refs),
    animations: drop(env.animations),
    locals: ownLocals ?? drop(env.locals),
    callbacks: drop(env.callbacks),
    fnProps: drop(env.fnProps),
    pans: drop(env.pans),
    helpers: drop(env.helpers),
    svgImports: drop(env.svgImports),
    children: names.includes(env.children?.ref?.name as string)
      ? null
      : env.children,
  };
}

/**
 * `{listState.map((item, i) => <Row/>)}` over a STATE array of variable length. Pre-allocates a fixed
 * pool of `cap` rows (no runtime malloc); the row at index `row` binds `item` to `s_<name>[row]` (a struct
 * local) and is shown only while `row < count` (display toggle). app_update then drives every row's content
 * and show.
 *
 * @param call  The `.map(…)` call.
 * @param listState  The list state it maps over.
 * @param parentVar  The parent node's variable.
 * @param scope  Compile-time constants in reach.
 * @param out  Everything emitted so far.
 * @param env  The expression environment.
 * @param state  The component's state table.
 */
function emitDynamicMap(
  call: t.CallExpression,
  listState: ListState,
  parentVar: string,
  scope: Scope,
  out: Out,
  env: Env,
  state: StateTable,
): void {
  const mapFn = call.arguments[0];
  if (!isFn(mapFn)) {
    throw new Error('AOT: .map argument must be an inline function');
  }

  const itemName = (mapFn.params[0] as t.Identifier | undefined)?.name;
  const indexName = (mapFn.params[1] as t.Identifier | undefined)?.name;
  const rowJSX = componentReturnJSX(mapFn);

  // Build every row of the pool; each row's display is tied to the list's current count.
  for (let rowIndex = 0; rowIndex < listState.cap; rowIndex++) {
    // The item is a runtime struct slot, so drop any same-named const from the row scope, as emitComponent does.
    const rowScope: Scope = {...scope};
    if (itemName) {
      delete rowScope[itemName];
    }

    if (indexName) {
      rowScope[indexName] = rowIndex; // the index is a compile-time literal per pooled row
    }

    const rowLocals = new Map<string, Local>(env.locals);
    if (indexName) {
      rowLocals.delete(indexName); // the index is a per-row literal in rowScope
    }

    // A row's item has no C type of its own: only its fields are read, through `struct`.
    if (itemName) {
      rowLocals.set(itemName, {
        code: `${listState.arrayName}[${rowIndex}]`,
        struct: listState.struct,
      } as Local);
    }

    emitElementInto(
      rowJSX,
      parentVar,
      rowScope,
      out,
      rowEnv(env, [itemName, indexName], rowScope, rowLocals),
      state,
      {
        displayCode: `(${rowIndex} < ${listState.countMember})`,
      },
    );
  }
}

/**
 * Emits the children of a container node, handling element + {expression} children. Everything unrolls at
 * compile time into a fixed set of nodes: a state-driven condition builds its elements and toggles their
 * display, and a `.map` over list state builds a fixed pool of rows. Anything that would change the node
 * count at runtime is refused with an "AOT: …" error.
 *
 * @param children  The container's children.
 * @param parentVar  The container node's variable.
 * @param scope  Compile-time constants in reach.
 * @param out  Everything emitted so far.
 * @param env  The expression environment.
 * @param state  The component's state table.
 */
export function emitChildren(
  children: t.JSXElement['children'],
  parentVar: string,
  scope: Scope,
  out: Out,
  env: Env,
  state: StateTable,
): void {
  for (const child of children) {
    // An element child is emitted as it is; an {expression} child is lowered by its shape below.
    if (child.type === 'JSXElement') {
      emitElementInto(child, parentVar, scope, out, env, state);
    } else if (child.type === 'JSXExpressionContainer') {
      const expr = child.expression;
      if (expr.type === 'JSXEmptyExpression') continue;

      if (isChildrenRef(expr, env)) {
        // {children} / {props.children}: emit the captured call-site children, in the caller's scope/env.
        emitChildren(
          env.children!.nodes,
          parentVar,
          env.children!.scope,
          out,
          env.children!.env,
          state,
        );
        continue;
      }

      if (expr.type === 'LogicalExpression' && expr.operator === '&&') {
        // `{cond && <X/>}`: a static cond includes or omits X; a dynamic one builds X and toggles its display.
        let condValue;
        try {
          condValue = evalStatic(expr.left, scope);
          if (condValue) {
            emitElementInto(expr.right, parentVar, scope, out, env, state);
          }
        } catch {
          const condCode = asCond(emitExpr(expr.left, env));
          emitElementInto(expr.right, parentVar, scope, out, env, state, {
            displayCode: condCode,
          });
        }
      } else if (
        expr.type === 'ConditionalExpression' &&
        (expr.consequent.type === 'JSXElement' ||
          expr.alternate.type === 'JSXElement')
      ) {
        // `{cond ? <A/> : <B/>}`. Static cond picks a branch; dynamic cond builds both and toggles each.
        let testValue;
        try {
          testValue = evalStatic(expr.test, scope);
          emitElementInto(
            testValue ? expr.consequent : expr.alternate,
            parentVar,
            scope,
            out,
            env,
            state,
          );
        } catch {
          const condCode = asCond(emitExpr(expr.test, env));

          if (expr.consequent.type === 'JSXElement') {
            emitElementInto(
              expr.consequent,
              parentVar,
              scope,
              out,
              env,
              state,
              {displayCode: condCode},
            );
          }

          if (expr.alternate.type === 'JSXElement') {
            emitElementInto(expr.alternate, parentVar, scope, out, env, state, {
              displayCode: `!(${condCode})`,
            });
          }
        }
      } else if (
        expr.type === 'CallExpression' &&
        expr.callee.type === 'MemberExpression' &&
        (expr.callee.property as t.Identifier).name === 'map'
      ) {
        // `{items.map(item => <X/>)}`: a pool of rows over list state, else unrolled over a constant array.
        const mapTarget = expr.callee.object;
        const stateRecord =
          mapTarget.type === 'Identifier'
            ? env.state.get(mapTarget.name)
            : null;
        if (stateRecord?.kind === 'list') {
          emitDynamicMap(expr, stateRecord, parentVar, scope, out, env, state);
        } else {
          emitMap(expr, parentVar, scope, out, env, state);
        }
      } else {
        // A constant that renders nothing (false/null/'') is fine; anything else is unsupported.
        let value;
        try {
          value = evalStatic(expr, scope);
        } catch {
          const error = aotError(
            `AOT: unsupported expression child "${expr.type}" in a container`,
            'a child expression must be a JSX element, `cond && <El/>` / a ternary of elements, ' +
              "or `list.map(item => <El/>)`. A bare variable holding JSX isn't inlined — write the " +
              'element directly. (If this is a responsive Flow-A-only branch, compile at the target ' +
              'board size via ER_AOT_SCREEN_W/H so the AOT folds the supported branch.)',
          );
          if (expr.loc) {
            error.aotLoc = expr.loc.start;
          }
          throw error;
        }

        if (value !== false && value != null && value !== '') {
          const error = aotError(
            `AOT: a non-element expression child (${JSON.stringify(value)}) cannot render here`,
            'only JSX elements render as children; wrap text in a <Text>{…}</Text>.',
          );
          if (expr.loc) {
            error.aotLoc = expr.loc.start;
          }
          throw error;
        }
      }
    }
  }
}
