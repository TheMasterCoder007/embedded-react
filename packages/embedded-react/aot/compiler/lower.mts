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
import {aotError} from './diagnostics.mts';
import {evalStatic, setBinding, withUndefined} from './static-eval.mts';
import {abandonBoot, beginBoot, endBoot, setBootLowering} from './boot.mts';
import {
  findShadowedClockCalls,
  emitSourcedWide,
  lowerExpr,
} from './expressions.mts';
import {
  findComponent,
  declaredNames,
  moduleScope,
  collectState,
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
import {normalizeUndefined, parseApp} from './parse.mts';
import {Out} from './out.mts';
import type {BlockStatement} from '@babel/types';
import type {ResolvedOptions} from './options.mts';
import {isStateDriven, nodesOf, updatedVectorOf} from './ir/nodes.mts';
import {nodeReads, vectorReads} from './ir/reads.mts';
import type {IrApp} from './ir/app.mts';
import type {IrSlotStore} from './ir/source.mts';
import type {SlotWidths} from './ir/types.mts';
import type {IrNode} from './ir/nodes.mts';
import type {IrCallback, IrStmt} from './ir/statements.mts';
import type {Env, Scope, StateRecord} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Settles what only the whole app decides. With nothing for app_update() to re-apply there is no app_update(),
 * so no body ends by calling it. A composition that no start waits on has no waits for a stop to end.
 *
 * @param out  The lowered app's functions.
 * @param hasUpdate  The app has an app_update().
 */
function settleFunctions(out: Out, hasUpdate: boolean): void {
  const idleOwners = new Set(
    [...out.animWaits.values()]
      .filter(record => !record.isStarted)
      .map(record => record.owner),
  );
  const settleStmts = (stmts: IrStmt[]): void => {
    for (const stmt of stmts) {
      if (stmt.kind === 'stopAnimations') {
        if (stmt.waitOwner !== null && idleOwners.has(stmt.waitOwner)) {
          stmt.waitOwner = null;
        }
      } else if (stmt.kind === 'if') {
        settleStmts(stmt.consequent);
        settleStmts(stmt.alternate ?? []);
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

  for (const handler of out.handlers) {
    if (handler.kind === 'callback') {
      settle(handler.callback);
    } else if (handler.kind === 'panEvent') {
      settle(handler.event.callback);
    }
  }
  for (const timer of out.timerFns) {
    settle(timer.callback);
  }
  for (const animationCallback of out.animCbs) {
    if (animationCallback.kind === 'completion') {
      settle(animationCallback.completion);
    }
  }
  for (const effectFunction of out.effectFns) {
    if (effectFunction.kind === 'mount') {
      settleStmts(effectFunction.body);
    } else if (effectFunction.kind === 'cleanup') {
      settleStmts(effectFunction.effect.cleanup!);
    } else {
      settleStmts(effectFunction.effect.body);
    }
  }
  for (const mountEffect of out.mountEffects) {
    if (mountEffect.kind === 'inline') {
      settle(mountEffect.callback);
    } else if (mountEffect.kind === 'call' && !hasUpdate) {
      mountEffect.updatesState = false;
    }
  }
}

/**
 * Flags each state a node or drawing reads, so a write to it marks it changed and app_update() sets them again.
 * A node that also reads something untracked is set on every update, so its reads need no flag.
 *
 * @param nodes  The app's nodes.
 */
function watchState(nodes: IrNode[]): void {
  for (const node of nodes) {
    const vector = updatedVectorOf(node);
    for (const reads of [nodeReads(node), vector && vectorReads(vector)]) {
      reads?.forEach(record => {
        record.isWatched = true;
      });
    }
  }
}

/**
 * Refuses state C cannot hold: a number in a list item that has no C form, and a host value that would take
 * the name of the setter every app exports for the wall clock. A field an item leaves out starts at 0.
 *
 * @param stateRecords  The app's state, then each inlined instance's.
 */
function checkStateRecords(stateRecords: StateRecord[]): void {
  for (const record of stateRecords) {
    if (record.kind !== 'list') continue;
    for (const item of record.items) {
      for (const field of record.struct.fields) {
        const value = item[field.key];
        if (field.kind === 'string' || value == null) continue;
        const number = Number(value);
        if (field.kind === 'float' || !Number.isFinite(number)) {
          checkNumber(number);
        }
      }
    }
  }

  if (
    stateRecords.some(
      record =>
        record.kind === 'scalar' && record.host && record.name === 'wall_clock',
    )
  ) {
    throw aotError(
      'AOT: useHostValue("wall_clock") would collide with er_app_set_wall_clock()',
      'rename the host value — every app exports er_app_set_wall_clock() for Date.now().',
    );
  }
}

/**
 * Lowers a Flow B app's JSX (or TSX) source, then runs the check pass over what lowering recorded. Pure (no
 * I/O); writeApp (assemble.mts) writes the result as C.
 *
 * @param src  The App.jsx/App.tsx source text.
 * @param opts  compileSource's options, resolved (see resolveOptions).
 * @param wide  State/ref slots to declare 64-bit (see compileWidened).
 * @param stores  Collects every value stored into an int slot.
 *
 * @returns The lowered app.
 */
export function lowerApp(
  src: string,
  opts: ResolvedOptions,
  wide: SlotWidths,
  stores: IrSlotStore[],
): IrApp {
  beginBoot();
  try {
    const {result, checks} = lowerWithChecks(() =>
      lowerProgram(src, opts, wide, stores),
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
 * @param src  The App.jsx/App.tsx source text.
 * @param opts  compileSource's options, resolved (see resolveOptions).
 * @param wide  State/ref slots to declare 64-bit (see compileWidened).
 * @param stores  Collects every value stored into an int slot.
 *
 * @returns The lowered app, without its checks.
 */
function lowerProgram(
  src: string,
  opts: ResolvedOptions,
  wide: SlotWidths,
  stores: IrSlotStore[],
): Omit<IrApp, 'boot' | 'checks'> {
  // Parse the app and prepare the AST for the passes below.
  const ast = parseApp(src, opts);
  normalizeUndefined(ast);
  const shadowedClock = findShadowedClockCalls(ast);

  // Seed the module scope with image imports (each is its baked-name string) so consts that use them can fold.
  const {screen} = opts;
  const imageImports = collectImageImports(ast.program);
  const svgImports = collectSvgImports(ast.program);
  const imageSeed = Object.fromEntries(
    [...imageImports].map(([local, imageImport]) => [local, imageImport.name]),
  );
  const scope: Scope = moduleScope(ast.program, screen, imageSeed);
  // Snapshot module bindings before App's consts fold in: child components close over these, not App's shadows.
  const moduleConsts: Scope = {...scope};
  const component = findComponent(ast.program);
  // Hide redeclared module names, then fold the body's static consts so responsive `if`s resolve at compile time.
  for (const name of declaredNames((component.body as BlockStatement).body)) {
    delete scope[name];
  }

  for (const statement of (component.body as BlockStatement).body) {
    if (
      statement.type !== 'VariableDeclaration' ||
      statement.kind !== 'const'
    ) {
      continue;
    }

    for (const decl of statement.declarations) {
      if (decl.id.type !== 'Identifier' || !decl.init) continue;
      try {
        setBinding(
          scope,
          decl.id.name,
          bindConst(
            decl.id.name,
            evalStatic(decl.init, withUndefined(scope), 'boot'),
          ),
        );
      } catch {
        // A dynamic const must hide a same-named module const, or the JSX quietly renders the module value.
        delete scope[decl.id.name];
      }
    }
  }

  // Collect the component's state; a state name hides any module const of the same name from the folds.
  const state = collectState(
    component.body as BlockStatement,
    scope,
    opts.caps,
    '',
    wide,
  );
  for (const name of state.byName.keys()) {
    delete scope[name];
  }

  // Collect the component's other hooks, plus the file's helpers and child components.
  const rootJSX = findReturnJSX(component.body as BlockStatement, scope);
  const anims = collectAnims(component.body, scope);
  const refs = collectRefs(component.body, scope, '', wide);
  for (const name of [...anims.keys(), ...refs.keys()]) {
    delete scope[name];
  }

  const pans = collectPanResponders(component.body);
  const callbacks = collectCallbacks(component.body);
  const memos = collectMemos(component.body);
  const helpers = collectHelpers(component.body, ast.program);
  const components = collectComponents(ast.program);
  const imageNames = new Map(
    [...imageImports].map(([, imageImport]) => [
      imageImport.name,
      imageImport.importPath,
    ]),
  ); // asset name → path

  // The expression environment every emitter compiles against.
  const env: Env = {
    state: state.byName,
    locals: new Map(),
    consts: scope,
    moduleConsts, // what an inlined child component sees (see emitComponent)
    animations: anims,
    refs,
    pans,
    callbacks,
    helpers,
    imageNames,
    svgImports,
    svgArtifacts: opts.svgArtifacts || {},
    caps: opts.caps,
    wide,
    stores,
    shadowedClock,
  };

  // A value worked out at boot lowers its slot against the app's environment.
  setBootLowering(env, lowerExpr);

  // Before emit, fold each memo or inline its C expression at each use (no cache needed: dep tracking re-applies).
  for (const [name, expr] of memos) {
    try {
      setBinding(scope, name, bindConst(name, evalStatic(expr, scope, 'boot')));
    } catch {
      const cExpr = emitSourcedWide(expr, env);
      env.locals.set(name, {
        code: `(${cExpr.code})`,
        cType: cExpr.cType,
        isBool: cExpr.isBool,
        lit: cExpr.lit,
        wide: cExpr.wide,
        source: cExpr.source,
      });
      delete scope[name]; // a runtime binding beats a module const of its name in every fold
    }
  }

  // Emit the scene graph from the root JSX; `out` collects every generated fragment.
  const out = new Out(components, ast.program);
  const keyboard = lowerKeyboardConfig(ast.program); // a module-level setKeyboardConfig({...})
  const root = emitNode(rootJSX, scope, out, env, state);

  // Emit registered only the images it reached; a dynamic source can't be enumerated, so bake every import.
  if (out.bakeAllImages) {
    for (const [, imageImport] of imageImports) {
      out.images.set(imageImport.name, imageImport.importPath);
    }
  }

  // Lower effects (mount-once or dep-driven) after emit, so out.timerFns/usesTimers include handler timers too.
  for (const effect of collectEffects(component.body)) {
    lowerEffect(effect, env, state, out);
  }

  // app_update() exists only when something has to be re-applied after a state change.
  const hasUpdate =
    nodesOf(root).some(
      node => isStateDriven(node) || updatedVectorOf(node) !== null,
    ) || out.depEffects.length > 0;
  settleFunctions(out, hasUpdate);
  if (hasUpdate) {
    watchState(nodesOf(root));
  }

  const stateRecords = [...state.byName.values(), ...out.childStateRecords];
  checkStateRecords(stateRecords);

  return {
    state: stateRecords,
    refs: [...refs.values(), ...out.childRefs],
    animations: [...anims.values(), ...out.childAnimations],
    keyboard,
    root,
    hasUpdate,
    images: [...out.images.entries()].map(([name, importPath]) => ({
      name,
      importPath,
    })),
    out,
  };
}
