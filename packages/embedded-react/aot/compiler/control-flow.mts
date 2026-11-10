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
import {foldBoot, isBoot} from './boot.mts';
import {mergeBranchElements} from './branch-merge.mts';
import {evalStatic, setBinding, withUndefined} from './static-eval.mts';
import {
  bindBuiltValues,
  foldRuntime,
  listItemOf,
  lowerExpr,
  lowerSourcedWide,
} from './expressions.mts';
import {STRUCT} from './ir/types.mts';
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
  bindConst,
  settleBootInitials,
} from './collect.mts';
import {collectAnims, collectRefs} from './animations.mts';
import {attrExpr} from './style-text.mts';
import {inlineFragments} from './elements/props.mts';
import {lowerEffect} from './handlers.mts';
import {
  bindDerivedList,
  derivedListOf,
  renderBindings,
  rowItemOf,
} from './list-order.mts';
import {collectPanResponders, panSpreadTarget} from './pan-responder.mts';
import {emitNode} from './nodes.mts';
import {narrowOn, narrowed, narrowedScope} from './tagged-records.mts';
import type * as t from '@babel/types';
import type {AotError} from './diagnostics.mts';
import type {Out} from './out.mts';
import type {IrBranch, IrChild, IrElement, IrShow} from './ir/nodes.mts';
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
 * A prop as a component instance receives it: a folded constant, a runtime expression (a list row's
 * `item.field`, say), or a callback compiled where the component uses it as an event handler.
 */
type PropDescriptor =
  | {static: true; value: unknown; isCallback?: undefined}
  | {static: 'list'; list: ListState; isCallback?: undefined}
  | (Local & {static: false; isCallback?: undefined})
  | {
      isCallback: true;
      node: FunctionNode;
      environment?: Env;
      state?: StateTable;
      static?: undefined;
    };

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Reads a component's props as descriptors: `{static:true,value}` (folded),
 * `{static:false, slotKind, struct}` (a runtime expression — e.g., a list row's `item.field`), or
 * `{fn:true,node}` (a callback).
 *
 * @param openingElement  The component instance's opening tag.
 * @param scope  Compile-time constants in reach at the call site.
 * @param environment  The caller's expression environment.
 *
 * @returns The props, by name.
 */
function extractProps(
  openingElement: t.JSXOpeningElement,
  scope: Scope,
  environment: Env,
): Record<string, PropDescriptor> {
  const props: Record<string, PropDescriptor> = {};
  for (const attribute of openingElement.attributes) {
    // Static spread: {...obj} where obj folds to a compile-time object → merge its keys as props.
    if (attribute.type === 'JSXSpreadAttribute') {
      // A PanResponder's handlers need a real scene node, which a component instance is not.
      if (panSpreadTarget(attribute.argument, environment)) {
        throw aotError(
          'AOT: a PanResponder can only be spread onto a host element',
          'spread `{...pan.panHandlers}` onto the <View> (or <Pressable>/<ScrollView>) that ' +
            'should own the gesture, not onto a component instance — the AOT wires the responder ' +
            'to a real scene node.',
        );
      }

      let spreadObject;
      try {
        spreadObject = evalStatic(attribute.argument, scope);
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
      attribute.type !== 'JSXAttribute' ||
      (attribute.name as t.JSXIdentifier).name === 'key'
    ) {
      continue;
    }

    // A callback prop (inline arrow, caller's useCallback or forwarded prop) is an ` fn `, resolved at its handler.
    const node = attrExpr(attribute);
    if (isFn(node)) {
      props[(attribute.name as t.JSXIdentifier).name] = {
        isCallback: true,
        node,
      };
      continue;
    }

    if (node.type === 'Identifier' && environment.callbacks?.has(node.name)) {
      props[(attribute.name as t.JSXIdentifier).name] = {
        isCallback: true,
        node: environment.callbacks.get(node.name)!,
      };
      continue;
    }

    if (
      node.type === 'Identifier' &&
      environment.functionProps?.has(node.name)
    ) {
      props[(attribute.name as t.JSXIdentifier).name] = {
        isCallback: true,
        ...environment.functionProps.get(node.name)!,
      }; // forward original {node, env, state}

      continue;
    }

    // A list state passes as it is, and so does a list filtered or sorted in render.
    const propName = (attribute.name as t.JSXIdentifier).name;
    const listState =
      node.type === 'Identifier'
        ? environment.state.get(node.name)
        : derivedListOf(node, environment);
    if (listState?.kind === 'list') {
      props[propName] = {static: 'list', list: listState};
      continue;
    }

    // Any other prop folds to a constant when it can, else becomes a runtime expression. A list's item by index
    // passes as it is, and an array or object built from runtime values folds with those values kept as runtime
    // reads.
    try {
      props[propName] = {
        static: true,
        value: bindConst(propName, evalStatic(node, scope, 'boot')),
      };
    } catch {
      const item = listItemOf(node, environment);
      props[propName] = item
        ? {
            static: false,
            ...item,
            source: {kind: 'local', local: item, location: node.loc?.start},
            expression: {kind: 'local', local: item, type: STRUCT},
          }
        : node.type === 'ArrayExpression' || node.type === 'ObjectExpression'
          ? {static: true, value: foldRuntime(node, environment, scope)}
          : {static: false, ...lowerSourcedWide(node, environment)};
    }
  }

  return props;
}

/**
 * Maps a component's parameter to its prop descriptors (handles destructure rename and defaults).
 *
 * @param functionNode  The component.
 * @param props  The props the instance was given.
 * @param scope  Module constants a default can refer to.
 *
 * @returns The descriptor each name in the component's body is bound to.
 */
function bindParams(
  functionNode: FunctionNode,
  props: Record<string, PropDescriptor>,
  scope: Scope,
): Map<string, PropDescriptor> {
  const bindings = new Map<string, PropDescriptor>();
  const parameter = functionNode.params[0];
  if (!parameter) return bindings;

  // `function C(props)`: the whole props object is one constant, so every prop must fold.
  if (parameter.type === 'Identifier') {
    const propsObject: Record<string, unknown> = {};
    for (const [propName, descriptor] of Object.entries(props)) {
      if (descriptor.static !== true) {
        throw new Error(
          'AOT: dynamic props require a destructured component parameter (e.g. `function C({ x })`)',
        );
      }

      propsObject[propName] = descriptor.value;
    }

    bindings.set(parameter.name, {static: true, value: propsObject});
  } else if (parameter.type === 'ObjectPattern') {
    // `function C({x, y: renamed, z = 1})`: bind each name to its prop, else to its folded default.
    const properties = parameter.properties.map(property => {
      if (property.type === 'RestElement') {
        throw new Error(
          'AOT: rest props (...rest) in a component param not supported',
        );
      }

      // Resolve a destructured prop's source name and local binding name.
      const propName =
        (property.key as t.Identifier).name ??
        (property.key as t.StringLiteral).value;
      const bindName =
        property.value?.type === 'Identifier'
          ? property.value.name
          : property.value?.type === 'AssignmentPattern'
            ? (property.value.left as t.Identifier).name
            : propName;
      return {property, propName, bindName};
    });

    // As in JS, the pattern's names shadow module ones in a default, and each is in reach only once bound.
    const defaultScope: Scope = {...scope};
    for (const {bindName} of properties) delete defaultScope[bindName];

    for (const {property, propName, bindName} of properties) {
      let descriptor = props[propName];
      if (!descriptor && property.value?.type === 'AssignmentPattern') {
        descriptor = {
          static: true,
          value: bindConst(
            bindName,
            evalStatic(property.value.right, defaultScope, 'boot'),
          ),
        };
      }

      descriptor ??= {static: true, value: undefined};
      if (descriptor.static === true)
        setBinding(defaultScope, bindName, descriptor.value);
      bindings.set(bindName, descriptor);
    }
  } else {
    throw new Error('AOT: unsupported component parameter pattern');
  }

  return bindings;
}

/**
 * True if `expression` is how a component body refers to its children (destructured {children} or props.children).
 *
 * @param expression  A child expression.
 * @param environment  The expression environment.
 *
 * @returns Whether it names the children slot.
 */
function isChildrenRef(expression: t.Node, environment: Env): boolean {
  const childrenRef = environment.children?.ref;
  if (!childrenRef) return false;
  if (childrenRef.kind === 'local') {
    return (
      expression.type === 'Identifier' && expression.name === childrenRef.name
    );
  }

  return (
    expression.type === 'MemberExpression' &&
    !expression.computed &&
    expression.object.type === 'Identifier' &&
    expression.object.name === childrenRef.name &&
    (expression.property as t.Identifier).name === 'children'
  );
}

/**
 * Inlines a function component instance: bind props (static → scope, dynamic → locals), emit its JSX.
 * Children passed at the call site are captured and emitted (in the CALLER's scope) where the body uses them.
 *
 * @param element  The component instance.
 * @param scope  Compile-time constants in reach at the call site.
 * @param output  Everything emitted so far.
 * @param environment  The caller's expression environment.
 * @param state  The caller's state table.
 * @param options  How the instance is placed.
 *
 * @returns The instance, wrapping the element the component's JSX lowered to.
 */
export function emitComponent(
  element: t.JSXElement,
  scope: Scope,
  output: Out,
  environment: Env,
  state: StateTable,
  options?: EmitOptions,
): IrElement {
  // Look up the component and keep the call-site children that render something.
  const componentName = (element.openingElement.name as t.JSXIdentifier).name;
  const functionNode = output.components.get(componentName)!;
  const childNodes = inlineFragments(element.children).filter(
    child =>
      !(child.type === 'JSXText' && !child.value.trim()) &&
      !(
        child.type === 'JSXExpressionContainer' &&
        child.expression.type === 'JSXEmptyExpression'
      ),
  );

  // How the body refers to children: destructured `{ children }` (a local) or whole `props` → props.children.
  const parameter = functionNode.params[0];
  let childrenRef: ChildrenRef | null = null;
  if (parameter?.type === 'ObjectPattern') {
    for (const property of parameter.properties as t.ObjectProperty[]) {
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
  } else if (parameter?.type === 'Identifier') {
    childrenRef = {kind: 'props', name: parameter.name};
  }

  // A child sees only module scope: start from module consts with no locals, so caller bindings cannot leak in.
  const moduleScope = environment.moduleConstants ?? scope;
  const childScope: Scope = {...moduleScope};
  const childLocals = new Map<string, Local>();
  // Callback props resolve to the caller's function, env and state; inherit the caller's own so they forward.
  const functionProps = new Map<string, FunctionProp>(
    environment.functionProps,
  );

  // Bind each prop: a callback to fnProps, a constant to the child's scope, a runtime value to its locals, and a
  // list state to its state.
  const listProps = new Map<string, ListState>();
  for (const [name, descriptor] of bindParams(
    functionNode,
    extractProps(element.openingElement, scope, environment),
    moduleScope,
  )) {
    // The children binding comes from the slot, not from a value prop.
    if (childrenRef?.kind === 'local' && name === childrenRef?.name) continue;
    if (descriptor.isCallback) {
      functionProps.set(name, {
        node: descriptor.node,
        environment: descriptor.environment ?? environment,
        state: descriptor.state ?? state,
      });
    } else if (descriptor.static === 'list') {
      listProps.set(name, descriptor.list);
      delete childScope[name];
    } else if (descriptor.static) {
      setBinding(childScope, name, descriptor.value);
    } else {
      childLocals.set(name, {
        name: descriptor.name,
        slotKind: descriptor.slotKind,
        struct: descriptor.struct,
        list: descriptor.list,
        row: descriptor.row,
        index: descriptor.index,
        guarded: descriptor.guarded,
        isBoolean: descriptor.isBoolean, // keep boolean-ness across the prop boundary (extractProps supplies it)
        isLiteral: descriptor.isLiteral,
        wide: descriptor.wide,
        source: descriptor.source,
        expression: descriptor.expression,
      });

      // A dynamic prop is a runtime binding; drop any same-named module const so no fold emits it instead.
      delete childScope[name];
    }
  }

  // Capture the call-site children with the caller's scope and env, for wherever the body renders them.
  const children: ChildrenSlot = {
    nodes: childNodes,
    scope,
    environment,
    ref: childrenRef,
  };

  // Each inlined instance namespaces its own hooks under a unique `c<N>_` prefix so instances stay independent.
  const prefix = `c${output.instanceCount++}_`;

  // Fold the child body's static consts as lowerApp does for App; they shadow same-named module consts.
  if (functionNode.body.type === 'BlockStatement') {
    // As in App: the body's own names, hook bindings included, shadow module ones before anything folds.
    for (const name of declaredNames(functionNode.body.body)) {
      delete childScope[name];
    }

    for (const statement of functionNode.body.body) {
      if (
        statement.type !== 'VariableDeclaration' ||
        statement.kind !== 'const'
      ) {
        continue;
      }
      for (const declarator of statement.declarations) {
        if (declarator.id.type !== 'Identifier' || !declarator.init) continue;
        if (childLocals.has(declarator.id.name)) continue; // a dynamic prop of that name is already bound
        try {
          setBinding(
            childScope,
            declarator.id.name,
            bindConst(
              declarator.id.name,
              evalStatic(declarator.init, withUndefined(childScope), 'boot'),
            ),
          );
        } catch {
          // A dynamic const hides any same-named module const; a memo re-binds it below, else it stays unresolved.
          delete childScope[declarator.id.name];
        }
      }
    }
  }

  // Collect the child's own hooks under its prefix; its state slots are runtime bindings, so they leave the scope.
  const childAnimations = collectAnims(functionNode.body, childScope, prefix);
  const childRefs = collectRefs(
    functionNode.body,
    childScope,
    prefix,
    environment.wide,
  );
  const childPans = collectPanResponders(functionNode.body, prefix);
  const childCallbacks = collectCallbacks(functionNode.body);
  const childMemos = collectMemos(functionNode.body);
  let childState = state;
  if (usesState(functionNode)) {
    childState = collectState(
      functionNode.body as t.BlockStatement,
      childScope,
      environment.caps,
      prefix,
      environment.wide,
      output.program,
    );
    settleBootInitials(childState.byName.values());
    output.childStateRecords.push(...childState.byName.values());
    for (const name of childState.byName.keys()) {
      delete childScope[name];
    }
  }

  // The child's own refs and animated values are runtime bindings too (see lowerApp).
  for (const name of [...childAnimations.keys(), ...childRefs.keys()]) {
    delete childScope[name];
  }
  output.childRefs.push(...childRefs.values());
  output.childAnimations.push(...childAnimations.values());

  // The child's expression environment: the caller's, with every name-keyed table replaced by the child's.
  const childEnvironment: Env = {
    ...environment,
    constants: childScope,
    locals: childLocals,
    children,
    functionProps,
    state: listProps.size
      ? new Map([...childState.byName, ...listProps])
      : childState.byName,
    animations: childAnimations,
    refs: childRefs,
    pans: childPans,
    callbacks: childCallbacks,
    helpers: collectHelpers(functionNode.body, output.program),
    callbackPrefix: prefix,
  };

  // Resolve memos in order (fold, else a local expression), then compile its effects, all in the child's env/state.
  for (const {name, expression, isMemo} of renderBindings(
    functionNode.body,
    childMemos,
  )) {
    if (!isMemo) {
      bindDerivedList(name, expression, childEnvironment);
      continue;
    }
    try {
      setBinding(
        childScope,
        name,
        bindConst(name, evalStatic(expression, childScope, 'boot')),
      );
    } catch {
      if (bindDerivedList(name, expression, childEnvironment)) {
        continue;
      }
      if (
        expression.type === 'ArrayExpression' ||
        expression.type === 'ObjectExpression'
      ) {
        setBinding(childScope, name, foldRuntime(expression, childEnvironment));
        continue;
      }
      const memo = lowerSourcedWide(expression, childEnvironment);
      childLocals.set(name, {
        slotKind: memo.slotKind,
        isBoolean: memo.isBoolean,
        isLiteral: memo.isLiteral,
        wide: memo.wide,
        source: memo.source,
        expression: memo.expression,
      });
      delete childScope[name]; // a runtime binding beats a module const of its name in every fold
    }
  }

  if (functionNode.body.type === 'BlockStatement') {
    bindBuiltValues(functionNode.body.body, childEnvironment);
  }

  for (const effect of collectEffects(functionNode.body)) {
    lowerEffect(effect, childEnvironment, childState, output);
  }

  // Emit the JSX the component returns, in the child's own scope.
  const root = emitNode(
    componentReturnJSX(functionNode, childScope),
    childScope,
    output,
    childEnvironment,
    childState,
    options,
  );
  return {kind: 'component', id: root.id, name: componentName, prefix, root};
}

/**
 * Emits an element / component child. options.show toggles its display.
 *
 * @param node  The child; anything but a JSX element is refused.
 * @param scope  Compile-time constants in reach.
 * @param output  Everything emitted so far.
 * @param environment  The expression environment.
 * @param state  The component's state table.
 * @param options  How the child is placed.
 *
 * @returns The child.
 */
function emitElementInto(
  node: t.Node,
  scope: Scope,
  output: Out,
  environment: Env,
  state: StateTable,
  options?: EmitOptions,
): IrElement {
  if (node.type !== 'JSXElement') {
    throw new Error(`AOT: expected a JSX element here, got ${node.type}`);
  }

  return emitNode(node, scope, output, environment, state, options);
}

/**
 * Unrolls `arr.map((item, i) => <JSX/>)` over a COMPILE-TIME-CONSTANT array.
 *
 * @param call  The `.map(…)` call.
 * @param scope  Compile-time constants in reach.
 * @param output  Everything emitted so far.
 * @param environment  The expression environment.
 * @param state  The component's state table.
 *
 * @returns The rows.
 */
function emitMap(
  call: t.CallExpression,
  scope: Scope,
  output: Out,
  environment: Env,
  state: StateTable,
): IrElement[] {
  // The array must fold at compile time, its values worked out at run time aside, and the callback must be inline.
  const target = (call.callee as t.MemberExpression).object;
  let items;
  if (target.type === 'ArrayExpression') {
    items = foldRuntime(target, environment, scope);
  } else {
    try {
      items = evalStatic(target, scope);
    } catch {
      throw new Error(
        'AOT: .map target must be a list state, a constant array, or an array literal built in render',
      );
    }
  }

  if (!Array.isArray(items)) {
    throw new Error('AOT: .map target did not resolve to an array');
  }

  const mapFunction = call.arguments[0];
  if (!isFn(mapFunction)) {
    throw new Error('AOT: .map argument must be an inline function');
  }

  const itemName = (mapFunction.params[0] as t.Identifier | undefined)?.name;
  const indexName = (mapFunction.params[1] as t.Identifier | undefined)?.name;
  const rowJSX = componentReturnJSX(mapFunction);

  // Emit one row per element, with its item and index bound as compile-time constants.
  return items.map((item, index) => {
    const rowScope: Scope = {...scope};
    if (itemName) {
      setBinding(rowScope, itemName, item);
    }
    if (indexName) {
      setBinding(rowScope, indexName, index);
    }
    return emitElementInto(
      rowJSX,
      rowScope,
      output,
      rowEnv(environment, [itemName, indexName], rowScope),
      state,
    );
  });
}

/**
 * The env for one `.map` row. Its callback params (item, index) shadow every outer binding of the same
 * name — a JS arrow parameter always does — so those names leave every name-keyed map (state, locals,
 * callbacks, helpers, …), and the row's own binding wins: in `constants`, or in `ownLocals` for a pooled
 * row's struct item.
 *
 * @param environment  The expression environment around the `.map`.
 * @param parameters  The callback's parameter names (item, index), where present.
 * @param constants  The row's compile-time scope.
 * @param ownLocals  The row's runtime bindings, when its item is one (a pooled row).
 *
 * @returns The row's environment.
 */
function rowEnv(
  environment: Env,
  parameters: (string | undefined)[],
  constants: Scope,
  ownLocals: Map<string, Local> | null = null,
): Env {
  // Copy a table without the row's names; one that binds none of them is shared as is.
  const names = parameters.filter(Boolean) as string[];
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
    ...environment,
    constants,
    state: drop(environment.state),
    refs: drop(environment.refs),
    animations: drop(environment.animations),
    locals: ownLocals ?? drop(environment.locals),
    callbacks: drop(environment.callbacks),
    functionProps: drop(environment.functionProps),
    pans: drop(environment.pans),
    helpers: drop(environment.helpers),
    svgImports: drop(environment.svgImports),
    children: names.includes(environment.children?.ref?.name as string)
      ? null
      : environment.children,
  };
}

/**
 * `{listState.map((item, i) => <Row/>)}` over a STATE array of variable length. Pre-allocates a fixed
 * pool of `capacity` rows (no runtime malloc); the row at index `row` binds `item` to `s_<name>[row]` (a struct
 * local) and is shown only while `row < count` (display toggle). the update then drives every row's content
 * and show.
 *
 * @param call  The `.map(…)` call.
 * @param listState  The list state it maps over.
 * @param scope  Compile-time constants in reach.
 * @param output  Everything emitted so far.
 * @param environment  The expression environment.
 * @param state  The component's state table.
 *
 * @returns The pool.
 */
function emitDynamicMap(
  call: t.CallExpression,
  listState: ListState,
  scope: Scope,
  output: Out,
  environment: Env,
  state: StateTable,
): IrChild {
  const mapFunction = call.arguments[0];
  if (!isFn(mapFunction)) {
    throw new Error('AOT: .map argument must be an inline function');
  }

  const itemName = (mapFunction.params[0] as t.Identifier | undefined)?.name;
  const indexName = (mapFunction.params[1] as t.Identifier | undefined)?.name;
  const rowJSX = componentReturnJSX(mapFunction);

  // Build every row of the pool; each row's display is tied to the list's current count.
  const rows: IrElement[] = [];
  for (let rowIndex = 0; rowIndex < listState.capacity; rowIndex++) {
    // The item is a runtime struct slot, so drop any same-named const from the row scope, as emitComponent does.
    const rowScope: Scope = {...scope};
    if (itemName) {
      delete rowScope[itemName];
    }

    if (indexName) {
      setBinding(rowScope, indexName, rowIndex); // the index is a compile-time literal per pooled row
    }

    const rowLocals = new Map<string, Local>(environment.locals);
    if (indexName) {
      rowLocals.delete(indexName); // the index is a per-row literal in rowScope
    }

    // A row's item has no slot of its own: only its fields are read, through `struct`.
    if (itemName) {
      rowLocals.set(
        itemName,
        rowItemOf(listState, {name: itemName, row: rowIndex} as Local),
      );
    }

    rows.push(
      emitElementInto(
        rowJSX,
        rowScope,
        output,
        {
          ...rowEnv(environment, [itemName, indexName], rowScope, rowLocals),
          inListRow: true,
        },
        state,
        {show: {kind: 'rowInUse', row: rowIndex, list: listState}},
      ),
    );
  }

  return {kind: 'pool', list: listState, rows};
}

/** The value types whose falsy value, 0, React renders as text. */
const NUMERIC_KINDS = new Set(['int', 'float', 'int64']);

/**
 * Whether a folded child value renders nothing, as React treats it: false, null, undefined and the empty string.
 *
 * @param value  The folded value.
 *
 * @returns Whether it renders nothing.
 */
const rendersNothing = (value: unknown): boolean =>
  value === false || value == null || value === '';

/**
 * The error for a child expression that is not lowered; `what` describes it when it folded to a value.
 *
 * @param expression  The child expression.
 * @param what  Its folded value, described, or null when it does not fold.
 *
 * @returns The error, for the caller to throw.
 */
function unsupportedChild(expression: t.Node, what: string | null): AotError {
  const error =
    what == null
      ? aotError(
          `AOT: unsupported expression child "${expression.type}" in a container`,
          'a child expression must be a JSX element, `cond && <El/>` / a ternary of elements, ' +
            "or `list.map(item => <El/>)`. A bare variable holding JSX isn't inlined — write the " +
            'element directly. (If this is a responsive Flow-A-only branch, compile at the target ' +
            'board size via ER_AOT_SCREEN_W/H so the AOT folds the supported branch.)',
        )
      : aotError(
          `AOT: a non-element expression child (${what}) cannot render here`,
          'only JSX elements render as children; wrap text in a <Text>{…}</Text>.',
        );
  if (expression.loc) {
    error.aotLoc = expression.loc.start;
  }

  return error;
}

/**
 * Lowers one branch of a conditional child: an element, a nested `&&` or ternary, or something that renders
 * nothing. A condition that folds picks its branch now. A state-driven one builds the branch it picks; in a list
 * row it builds every branch with the row and shows each only while it is chosen, `within` the branch it sits in.
 *
 * @param expression  The branch.
 * @param scope  Compile-time constants in reach.
 * @param output  Everything emitted so far.
 * @param environment  The expression environment.
 * @param state  The component's state table.
 * @param within  When the branch it sits in shows, or null at the top.
 *
 * @returns The branch.
 */
function lowerBranch(
  expression: t.Node,
  scope: Scope,
  output: Out,
  environment: Env,
  state: StateTable,
  within: IrShow | null,
): IrBranch {
  if (expression.type === 'JSXElement') {
    return emitElementInto(
      expression,
      scope,
      output,
      environment,
      state,
      within ? {show: within} : {},
    );
  }

  const isAnd =
    expression.type === 'LogicalExpression' && expression.operator === '&&';
  if (isAnd || expression.type === 'ConditionalExpression') {
    const testNode = isAnd
      ? expression.left
      : (expression as t.ConditionalExpression).test;
    const whenTrue = isAnd
      ? expression.right
      : (expression as t.ConditionalExpression).consequent;
    const whenFalse = isAnd
      ? null
      : (expression as t.ConditionalExpression).alternate;

    // A test that folds picks its branch at build time; `&&` with a false test renders nothing.
    let folded: {value: unknown} | null;
    try {
      folded = {value: evalStatic(testNode, scope, 'boot')};
    } catch {
      folded = null;
    }

    // A test that depends on the panel needs no variant when both branches are elements of one shape.
    if (folded && isBoot(folded.value)) {
      const merged =
        whenTrue.type === 'JSXElement' && whenFalse?.type === 'JSXElement'
          ? mergeBranchElements(testNode, whenTrue, whenFalse)
          : null;
      if (merged)
        return lowerBranch(merged, scope, output, environment, state, within);
      folded = {value: foldBoot(folded.value)};
    }

    if (folded) {
      // A false `&&` yields its left value, which must be one React renders as nothing (`0` is text).
      if (isAnd && !folded.value && !rendersNothing(folded.value)) {
        throw unsupportedChild(
          expression,
          JSON.stringify(folded.value) ?? String(folded.value),
        );
      }

      const chosen = folded.value ? whenTrue : whenFalse;
      return chosen
        ? lowerBranch(chosen, scope, output, environment, state, within)
        : null;
    }

    // A state-driven test builds the branch it picks. A list row builds every branch, each shown while picked.
    const test = lowerExpr(testNode, environment);
    if (isAnd && NUMERIC_KINDS.has(test.type.kind)) {
      const error = aotError(
        'AOT: a number on the left of `&&` renders 0 as text when it is 0',
        'React draws the 0 itself, and text needs a <Text>. Test it as a condition: `count > 0 && <El/>`.',
      );
      if (testNode.loc) {
        error.aotLoc = testNode.loc.start;
      }

      throw error;
    }
    const toggle = !!environment.inListRow;
    const shown = (negate: boolean): IrShow | null =>
      toggle ? {kind: 'condition', test, negate, within} : null;
    // A test on a tagged object's tag narrows the fields each branch can read.
    const narrowing = narrowOn(testNode, environment);
    return {
      kind: 'conditional',
      test,
      toggle,
      whenTrue: lowerBranch(
        whenTrue,
        narrowedScope(scope, narrowing.whenTrue),
        output,
        narrowed(environment, narrowing.whenTrue),
        state,
        shown(false),
      ),
      whenFalse: whenFalse
        ? lowerBranch(
            whenFalse,
            narrowedScope(scope, narrowing.whenFalse),
            output,
            narrowed(environment, narrowing.whenFalse),
            state,
            shown(true),
          )
        : null,
    };
  }

  // Anything else must be a constant that renders nothing.
  let value: unknown;
  try {
    value = evalStatic(expression, withUndefined(scope));
  } catch {
    throw unsupportedChild(expression, null);
  }

  if (!rendersNothing(value)) {
    throw unsupportedChild(expression, JSON.stringify(value) ?? String(value));
  }

  return null;
}

/**
 * Emits the children of a container node. Everything unrolls at compile time: fragments are inlined, a
 * state-driven condition lowers each of its branches, and a `.map` over list state builds a fixed pool of rows.
 * Every child is either emitted or refused with an "AOT: …" error: text outside a <Text>, or anything else the
 * engine has no node for.
 *
 * @param children  The container's children.
 * @param scope  Compile-time constants in reach.
 * @param output  Everything emitted so far.
 * @param environment  The expression environment.
 * @param state  The component's state table.
 *
 * @returns The container's children.
 */
export function emitChildren(
  children: t.JSXElement['children'],
  scope: Scope,
  output: Out,
  environment: Env,
  state: StateTable,
): IrChild[] {
  const lowered: IrChild[] = [];
  const element = (built: IrElement) =>
    lowered.push({kind: 'element', element: built});
  for (const child of inlineFragments(children)) {
    // An element child is emitted as it is; an {expression} child is lowered by its shape below.
    if (child.type === 'JSXElement') {
      element(emitElementInto(child, scope, output, environment, state));
      continue;
    }

    // Text needs a <Text>; the whitespace between tags is not a child.
    if (child.type === 'JSXText') {
      if (child.value.trim()) {
        const error = aotError(
          `AOT: text ${JSON.stringify(child.value.trim())} must be inside a <Text>`,
          'only a <Text> draws text: <Text>…</Text>.',
        );
        if (child.loc) {
          error.aotLoc = child.loc.start;
        }

        throw error;
      }

      continue;
    }

    if (child.type === 'JSXSpreadChild') {
      throw unsupportedChild(child, null);
    }

    const expression = child.expression;
    if (expression.type === 'JSXEmptyExpression') continue;

    if (isChildrenRef(expression, environment)) {
      // {children} / {props.children}: emit the captured call-site children, in the caller's scope/env.
      lowered.push(
        ...emitChildren(
          environment.children!.nodes,
          environment.children!.scope,
          output,
          environment.children!.environment,
          state,
        ),
      );
      continue;
    }

    if (
      expression.type === 'CallExpression' &&
      expression.callee.type === 'MemberExpression' &&
      (expression.callee.property as t.Identifier).name === 'map'
    ) {
      // `{items.map(item => <X/>)}`: a pool of rows over list state, else unrolled over a constant array.
      const mapTarget = expression.callee.object;
      const stateRecord =
        mapTarget.type === 'Identifier'
          ? environment.state.get(mapTarget.name)
          : derivedListOf(mapTarget, environment);
      if (stateRecord?.kind === 'list') {
        lowered.push(
          emitDynamicMap(
            expression,
            stateRecord,
            scope,
            output,
            environment,
            state,
          ),
        );
      } else {
        emitMap(expression, scope, output, environment, state).forEach(element);
      }

      continue;
    }

    // An element, `cond && <X/>`, a ternary of them (nested too), or a constant that renders nothing.
    const branch = lowerBranch(
      expression,
      scope,
      output,
      environment,
      state,
      null,
    );
    if (branch?.kind === 'conditional') {
      lowered.push(branch);
    } else if (branch) {
      element(branch);
    }
  }

  return lowered;
}
