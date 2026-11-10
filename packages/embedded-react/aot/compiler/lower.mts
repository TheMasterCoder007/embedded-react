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

import {checkNumber, lowerWithChecks, runChecks} from './check.mts';
import {
  evalStatic,
  rethrowFoldLimit,
  setBinding,
  withUndefined,
} from './static-eval.mts';
import {abandonBoot, beginBoot, endBoot, setBootLowering} from './boot.mts';
import {
  bindBuiltValues,
  findShadowedClockCalls,
  foldRuntime,
  lowerSourcedWide,
  lowerExpr,
} from './expressions.mts';
import {
  findComponent,
  declaredNames,
  moduleScope,
  collectState,
  settleBootInitials,
  findReturnJSX,
  collectComponents,
  collectHelpers,
  collectImageImports,
  collectSvgImports,
  collectCallbacks,
  collectMemos,
  collectEffects,
  bindConst,
} from './collect.mts';
import {collectAnims, collectRefs} from './animations.mts';
import {lowerEffect} from './handlers.mts';
import {collectPanResponders} from './pan-responder.mts';
import {emitNode} from './nodes.mts';
import {lowerKeyboardConfig} from './keyboard.mts';
import {itemValueAt} from './list-items.mts';
import {normalizeUndefined, parseApp} from './parse.mts';
import {inlineCustomHooks} from './custom-hooks.mts';
import {Out} from './out.mts';
import type {BlockStatement} from '@babel/types';
import type {ResolvedOptions} from './options.mts';
import {
  conditionalsOf,
  isStateDriven,
  nodesOf,
  updatedVectorOf,
} from './ir/nodes.mts';
import {
  expressionReads,
  listStates,
  nodeReads,
  vectorReads,
} from './ir/reads.mts';
import type {IrApp} from './ir/app.mts';
import type {IrSlotStore} from './ir/source.mts';
import type {SlotWidths} from './ir/types.mts';
import type {IrElement} from './ir/nodes.mts';
import type {IrCallback, IrStmt} from './ir/statements.mts';
import {bindDerivedList, renderBindings} from './list-order.mts';
import type {
  Env,
  FunctionProp,
  ListState,
  Local,
  Scope,
  StateRecord,
} from './types.mts';
import {bindContextUses, contextsOf} from './contexts.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Settles what only the whole app decides. With nothing for an update to re-apply there is no update,
 * so no body ends by asking for one. A composition that no start waits on has no waits for a stop to end.
 *
 * @param output  The lowered app's functions.
 * @param hasUpdate  The app has an update.
 */
function settleFunctions(output: Out, hasUpdate: boolean): void {
  const idleOwners = new Set(
    [...output.animationWaits.values()]
      .filter(record => !record.isStarted)
      .map(record => record.owner),
  );
  const settleStmts = (statements: IrStmt[]): void => {
    for (const statement of statements) {
      if (statement.kind === 'stopAnimations') {
        if (
          statement.waitOwner !== null &&
          idleOwners.has(statement.waitOwner)
        ) {
          statement.waitOwner = null;
        }
      } else if (statement.kind === 'if') {
        settleStmts(statement.consequent);
        settleStmts(statement.alternate ?? []);
      } else if (statement.kind === 'block' || statement.kind === 'loop') {
        settleStmts(statement.body);
      }
    }
  };
  const settle = (callback: IrCallback | null): void => {
    if (!callback) return;
    if (!hasUpdate) {
      callback.updatesState = false;
    }
    settleStmts(callback.body);
  };

  for (const handler of output.handlers) {
    if (handler.kind === 'callback') {
      settle(handler.callback);
    } else if (handler.kind === 'panEvent') {
      settle(handler.event.callback);
    }
  }
  for (const timer of output.timerFunctions) {
    settle(timer.callback);
  }
  for (const animationCallback of output.animationCallbacks) {
    if (animationCallback.kind === 'completion') {
      settle(animationCallback.completion);
    }
  }
  for (const effectFunction of output.effectFunctions) {
    if (effectFunction.kind === 'mount') {
      settleStmts(effectFunction.body);
    } else if (effectFunction.kind === 'cleanup') {
      settleStmts(effectFunction.effect.cleanup!);
    } else {
      settleStmts(effectFunction.effect.body);
    }
  }
  for (const mountEffect of output.mountEffects) {
    if (mountEffect.kind === 'inline') {
      settle(mountEffect.callback);
    } else if (mountEffect.kind === 'call' && !hasUpdate) {
      mountEffect.updatesState = false;
    }
  }
}

/**
 * Flags each state a node, drawing, or conditional reads, so a write to it marks it changed and the update sets
 * them again. One that also reads something untracked is set on every update, so its reads need no flag.
 *
 * @param root  The app's tree.
 */
function watchState(root: IrElement, derivedLists: ListState[]): void {
  const reads = [
    ...nodesOf(root).flatMap(node => {
      const vector = updatedVectorOf(node);
      return [nodeReads(node), vector && vectorReads(vector)];
    }),
    ...conditionalsOf(root).map(conditional =>
      expressionReads(conditional.test),
    ),
    // A handler can read a filtered or sorted list that nothing draws, which must still be rebuilt.
    ...derivedLists.map(listStates),
  ];
  for (const read of reads) {
    read?.forEach(record => {
      record.isWatched = true;
    });
  }
}

/**
 * Refuses state a package cannot hold: a number in a list item that isn't finite, and a host value that would take
 * the name of the setter every app exports for the wall clock. A field an item leaves out starts at 0.
 *
 * @param stateRecords  The app's state, then each inlined instance's.
 */
function checkStateRecords(stateRecords: StateRecord[]): void {
  for (const record of stateRecords) {
    if (record.kind !== 'list') continue;
    for (const item of record.items) {
      for (const field of record.struct.fields) {
        const {present, value} = itemValueAt(item, field);
        if (field.kind === 'string' || field.kind === 'bool' || !present) {
          continue;
        }
        const number = Number(value);
        if (field.kind === 'float' || !Number.isFinite(number)) {
          checkNumber(number);
        }
      }
    }
  }
}

/**
 * Lowers a Flow B app's JSX (or TSX) source, then runs the check pass over what lowering recorded. Pure (no
 * I/O); the package backend (package/) writes the result.
 *
 * @param source  The App.jsx/App.tsx source text.
 * @param options  compilePackage's options, resolved (see resolveOptions).
 * @param wide  State/ref slots to declare 64-bit (see compileWidened).
 * @param stores  Collects every value stored into an int slot.
 *
 * @returns The lowered app.
 */
export function lowerApp(
  source: string,
  options: ResolvedOptions,
  wide: SlotWidths,
  stores: IrSlotStore[],
): IrApp {
  beginBoot(source);
  try {
    const {result, checks} = lowerWithChecks(() =>
      lowerProgram(source, options, wide, stores),
    );
    const app = {...result, boot: endBoot(), checks};
    runChecks(app.checks);
    return app;
  } finally {
    abandonBoot();
  }
}

/**
 * Runs the compiler passes over the parsed app, from its module scope and hooks through the element tree to its
 * effects, recording the values the check pass checks.
 *
 * @param source  The App.jsx/App.tsx source text.
 * @param options  compilePackage's options, resolved (see resolveOptions).
 * @param wide  State/ref slots to declare 64-bit (see compileWidened).
 * @param stores  Collects every value stored into an int slot.
 *
 * @returns The lowered app, without its checks.
 */
function lowerProgram(
  source: string,
  options: ResolvedOptions,
  wide: SlotWidths,
  stores: IrSlotStore[],
): Omit<IrApp, 'boot' | 'checks'> {
  // Parse the app and prepare the AST for the passes below.
  const ast = parseApp(source, options);
  normalizeUndefined(ast);
  inlineCustomHooks(ast);
  const shadowedClock = findShadowedClockCalls(ast);

  // Seed the module scope with image imports (each is its baked-name string) so consts that use them can fold.
  const {screen} = options;
  const imageImports = collectImageImports(ast.program);
  const svgImports = collectSvgImports(ast.program);
  const imageSeed = Object.fromEntries(
    [...imageImports].map(([local, imageImport]) => [local, imageImport.name]),
  );
  const scope: Scope = moduleScope(ast.program, screen, imageSeed);
  // Snapshot module bindings before App's consts fold in: child components close over these, not App's shadows.
  const moduleConstants: Scope = {...scope};
  const component = findComponent(ast.program);
  // Hide redeclared module names, then fold the body's static consts so responsive `if`s resolve at compile time.
  for (const name of declaredNames((component.body as BlockStatement).body)) {
    delete scope[name];
  }

  // What the module itself can read, where a context's default is worked out.
  const imageNames = new Map(
    [...imageImports].map(([, imageImport]) => [
      imageImport.name,
      imageImport.importPath,
    ]),
  ); // asset name → path
  const moduleEnvironment: Env = {
    state: new Map(),
    locals: new Map(),
    constants: moduleConstants,
    moduleConstants,
    animations: new Map(),
    refs: new Map(),
    pans: new Map(),
    callbacks: new Map(),
    helpers: collectHelpers(
      {type: 'BlockStatement', body: [], directives: []},
      ast.program,
    ),
    imageNames,
    svgImports,
    svgArtifacts: options.svgArtifacts || {},
    caps: options.caps,
    wide,
    stores,
    shadowedClock,
    constantLists: new Map(),
    derivedLists: new Map(),
    splitLists: new Map(),
    functionProps: new Map(),
    children: null,
  };
  moduleEnvironment.contexts = {
    records: contextsOf(ast.program),
    defaults: moduleEnvironment,
  };

  // The App has no provider above it, so a context it reads is the default.
  const appLocals = new Map<string, Local>();
  const appFunctionProps = new Map<string, FunctionProp>();
  const contextNames = bindContextUses(
    component.body as BlockStatement,
    moduleEnvironment,
    {
      scope,
      locals: appLocals,
      functionProps: appFunctionProps,
      lists: new Map(),
    },
  );

  for (const statement of (component.body as BlockStatement).body) {
    if (
      statement.type !== 'VariableDeclaration' ||
      statement.kind !== 'const'
    ) {
      continue;
    }

    for (const declarator of statement.declarations) {
      if (declarator.id.type !== 'Identifier' || !declarator.init) continue;
      if (contextNames.has(declarator.id.name)) continue;
      try {
        setBinding(
          scope,
          declarator.id.name,
          bindConst(
            declarator.id.name,
            evalStatic(declarator.init, withUndefined(scope), 'boot'),
          ),
        );
      } catch (error) {
        rethrowFoldLimit(error);
        // A dynamic const must hide a same-named module const, or the JSX quietly renders the module value.
        delete scope[declarator.id.name];
      }
    }
  }

  // Collect the component's state; a state name hides any module const of the same name from the folds.
  const state = collectState(
    component.body as BlockStatement,
    scope,
    options.caps,
    '',
    wide,
    ast.program,
  );
  for (const name of state.byName.keys()) {
    delete scope[name];
  }

  // Collect the component's other hooks, plus the file's helpers and child components.
  const rootJSX = findReturnJSX(component.body as BlockStatement, scope);
  const animations = collectAnims(component.body, scope);
  const refs = collectRefs(component.body, scope, '', wide);
  for (const name of [...animations.keys(), ...refs.keys()]) {
    delete scope[name];
  }

  const pans = collectPanResponders(component.body);
  const callbacks = collectCallbacks(component.body);
  const memos = collectMemos(component.body);
  const helpers = collectHelpers(component.body, ast.program);
  const components = collectComponents(ast.program);

  // The expression environment every emitter compiles against.
  const environment: Env = {
    state: state.byName,
    locals: appLocals,
    constants: scope,
    moduleConstants, // what an inlined child component sees (see emitComponent)
    animations: animations,
    refs,
    pans,
    callbacks,
    helpers,
    imageNames,
    svgImports,
    svgArtifacts: options.svgArtifacts || {},
    caps: options.caps,
    wide,
    stores,
    shadowedClock,
    constantLists: moduleEnvironment.constantLists,
    derivedLists: moduleEnvironment.derivedLists,
    splitLists: moduleEnvironment.splitLists,
    functionProps: appFunctionProps,
    contexts: moduleEnvironment.contexts,
  };

  // A value worked out at boot lowers its slot against the app's environment.
  setBootLowering(environment, lowerExpr);
  settleBootInitials(state.byName.values());

  // Before emit, fold each memo or inline its expression at each use (no cache needed: dep tracking re-applies).
  for (const {name, expression, isMemo} of renderBindings(
    component.body,
    memos,
  )) {
    if (!isMemo) {
      bindDerivedList(name, expression, environment);
      continue;
    }
    try {
      setBinding(
        scope,
        name,
        bindConst(name, evalStatic(expression, scope, 'boot')),
      );
    } catch (error) {
      rethrowFoldLimit(error);
      if (bindDerivedList(name, expression, environment)) {
        continue;
      }
      if (
        expression.type === 'ArrayExpression' ||
        expression.type === 'ObjectExpression'
      ) {
        setBinding(scope, name, foldRuntime(expression, environment));
        continue;
      }
      const memo = lowerSourcedWide(expression, environment);
      environment.locals.set(name, {
        slotKind: memo.slotKind,
        isBoolean: memo.isBoolean,
        isLiteral: memo.isLiteral,
        wide: memo.wide,
        source: memo.source,
        expression: memo.expression,
      });
      delete scope[name]; // a runtime binding beats a module const of its name in every fold
    }
  }

  bindBuiltValues((component.body as BlockStatement).body, environment);

  // Emit the scene graph from the root JSX; `output` collects everything else it lowers.
  const output = new Out(components, ast.program);
  const keyboard = lowerKeyboardConfig(ast.program); // a module-level setKeyboardConfig({...})
  const root = emitNode(rootJSX, scope, output, environment, state);

  // Emit registered only the images it reached; a dynamic source can't be enumerated, so bake every import.
  if (output.bakeAllImages) {
    for (const [, imageImport] of imageImports) {
      output.images.set(imageImport.name, imageImport.importPath);
    }
  }

  // Lower effects (mount-once or dep-driven) after emit, so output.timerFunctions includes handler timers too.
  for (const effect of collectEffects(component.body)) {
    lowerEffect(effect, environment, state, output);
  }

  // An update exists only when something has to be re-applied after a state change, or a list filtered or sorted
  // in render rebuilt.
  const derivedLists = [...environment.derivedLists!.values()].flatMap(
    lists => [...lists.values()],
  );
  const hasUpdate =
    nodesOf(root).some(
      node => isStateDriven(node) || updatedVectorOf(node) !== null,
    ) ||
    conditionalsOf(root).some(conditional => !conditional.toggle) ||
    output.dependencyEffects.length > 0 ||
    derivedLists.length > 0;
  settleFunctions(output, hasUpdate);
  if (hasUpdate) {
    watchState(root, derivedLists);
  }

  const stateRecords = [
    ...state.byName.values(),
    ...output.childStateRecords,
    ...environment.constantLists!.values(),
    ...derivedLists,
    ...[...environment.splitLists!.values()].flatMap(lists => [
      ...lists.values(),
    ]),
  ];
  checkStateRecords(stateRecords);

  return {
    state: stateRecords,
    refs: [...refs.values(), ...output.childRefs],
    animations: [...animations.values(), ...output.childAnimations],
    keyboard,
    root,
    images: [...output.images.entries()].map(([name, importPath]) => ({
      name,
      importPath,
    })),
    output,
  };
}
