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

import {readFileSync} from 'node:fs';
import {aotError} from './diagnostics.mts';
import {evalStatic, withUndefined} from './static-eval.mts';
import {cScalarType, stripCLiterals, floatLit, cstr} from './c-syntax.mts';
import {
  CHECKED_OP,
  findShadowedClockCalls,
  emitExprWide,
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
} from './collect.mts';
import {collectAnims, collectRefs} from './animations.mts';
import {APP_UPDATE_CALL, compileEffect} from './handlers.mts';
import {collectPanResponders} from './pan-responder.mts';
import {emitNode} from './nodes.mts';
import {compileKeyboardConfig} from './keyboard.mts';
import {normalizeUndefined, parseApp} from './parse.mts';
import {Out} from './out.mts';
import type {BlockStatement} from '@babel/types';
import type {ResolvedOptions} from './options.mts';
import type {
  AssetImport,
  Env,
  ItemField,
  ItemStruct,
  ScalarState,
  Scope,
} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** What one compile produces. */
export interface CompileResult {
  /** The contents of app.gen.c, the generated C implementation. */
  c: string;
  /** The contents of app.gen.h, the generated C header. */
  h: string;
  /** The number of engine nodes the app creates. */
  nodes: number;
  /** The number of state records. */
  state: number;
  /** The number of event handlers. */
  handlers: number;
  /** The number of nodes app_update() re-applies. */
  updates: number;
  /** The baked image imports the generated app references. */
  images: AssetImport[];
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

// The compiler's version, stamped into app.gen.c and asserted against er_version.h, so a mismatch fails to compile.
const PKG_VERSION = JSON.parse(
  readFileSync(new URL('../../package.json', import.meta.url), 'utf8'),
).version;
const [PKG_MAJOR, PKG_MINOR] = PKG_VERSION.split('.');

// libm symbols, and the helpers that call them; any of them in the generated code means it includes <math.h>.
const LIBM_USE = new RegExp(
  '\\b(sinf|cosf|tanf|sqrtf|fabsf|roundf|floorf|ceilf|fminf|fmaxf|atan2f|powf|fmodf|isfinite|' +
    'app_roundf|app_ftoa|app_vector_dirty|M_PI)\\b',
);

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * The `#ifndef` marker a board example guards on, from a demo name. Every character outside [A-Za-z0-9]
 * becomes `_<hex code point>_`, so the result is a C identifier AND the mapping is one-to-one: `foo-bar`
 * is ER_AOT_DEMO_foo_2d_bar and `foo_bar` is ER_AOT_DEMO_foo_5f_bar. Flattening both to `foo_bar` would
 * let a board guard accept the wrong app from either entry point that generates one.
 */
export const demoMarker = (demo: string): string =>
  `ER_AOT_DEMO_${demo.replace(/[^A-Za-z0-9]/gu, character => `_${character.codePointAt(0)!.toString(16)}_`)}`;

/**
 * Compiles a Flow B app's JSX (or TSX) source to C: runs the compiler passes over the parsed app and stitches
 * their output into app.gen.c and app.gen.h. Pure (no I/O), so it can be unit-tested directly; compileSource
 * (compile.mts) is the public entry around it.
 *
 * @param src  The App.jsx/App.tsx source text.
 * @param demo  Demo name (only used in the generated-by header comment).
 * @param opts  compileSource's options, resolved (see resolveOptions).
 * @param wide  State/ref slots to declare 64-bit (see compileWidened).
 * @param found  Collects the int slots a 64-bit timestamp was stored in.
 *
 * @returns The generated app.gen.c and app.gen.h, and what they contain.
 */
export function compileSourceImpl(
  src: string,
  demo: string,
  opts: ResolvedOptions,
  wide: Set<string>,
  found: Set<string>,
): CompileResult {
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
        scope[decl.id.name] = evalStatic(decl.init, withUndefined(scope));
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
    found,
    shadowedClock,
  };

  // Before emit, fold each memo or inline its C expression at each use (no cache needed: dep tracking re-applies).
  for (const [name, expr] of memos) {
    try {
      scope[name] = evalStatic(expr, scope);
    } catch {
      const cExpr = emitExprWide(expr, env);
      env.locals.set(name, {
        code: `(${cExpr.code})`,
        cType: cExpr.cType,
        isBool: cExpr.isBool,
      });
      delete scope[name]; // a runtime binding beats a module const of its name in every fold
    }
  }

  // Emit the scene graph from the root JSX; `out` collects every generated fragment.
  const out = new Out(components, ast.program);
  compileKeyboardConfig(ast.program, out); // module-level setKeyboardConfig({...}) → static ERKeyboardConfig
  const rootNodeId = emitNode(rootJSX, scope, out, env, state);

  // Emit registered only the images it reached; a dynamic source can't be enumerated, so bake every import.
  if (out.bakeAllImages) {
    for (const [, imageImport] of imageImports) {
      out.images.set(imageImport.name, imageImport.importPath);
    }
  }

  // Compile effects (mount-once or dep-driven) after emit, so out.timerFns/usesTimers include handler timers too.
  for (const effect of collectEffects(component.body)) {
    compileEffect(effect, env, state, out);
  }

  // One ERNode* local in er_app_build() per node the tree allocated.
  const nodeDecls = Array.from(
    {length: out.nodeCount},
    (_, nodeIndex) => `n${nodeIndex}`,
  );

  // App state + every inlined child instance's per-instance state (each already namespaced via cField).
  const stateRecords = [...state.byName.values(), ...out.childStateRecords];
  const scalarRecords = stateRecords.filter(
    stateRecord => stateRecord.kind === 'scalar',
  );
  const listRecords = stateRecords.filter(
    stateRecord => stateRecord.kind === 'list',
  );

  // Scalar state → one ErAppState struct. List state → a fixed-capacity struct array + a count each.
  const fieldCDecl = (field: ItemField) =>
    field.kind === 'string'
      ? `    char ${field.key}[${opts.caps.listStrCap}];`
      : `    ${field.kind} ${field.key};`;
  const itemInit = (item: Record<string, unknown>, struct: ItemStruct) => {
    const values = struct.fields.map(field =>
      field.kind === 'string'
        ? cstr(String(item[field.key] ?? ''))
        : field.kind === 'float'
          ? floatLit(Number(item[field.key]) || 0)
          : String(Math.round(Number(item[field.key]) || 0)),
    );
    return `{ ${values.join(', ')} }`;
  };
  const listBlocks = listRecords
    .map(
      listRecord =>
        `typedef struct\n{\n${listRecord.struct.fields.map(fieldCDecl).join('\n')}\n} ${listRecord.cTypeName};\n\n` +
        `static ${listRecord.cTypeName} ${listRecord.arrayName}[${listRecord.cap}] = ` +
        `{${listRecord.items.map(item => '\n    ' + itemInit(item, listRecord.struct)).join(',')}\n};\n` +
        `static int ${listRecord.countMember} = ${listRecord.items.length};\n`,
    )
    .join('\n');
  const scalarFieldDecl = (scalarRecord: ScalarState) =>
    scalarRecord.cType === 'string'
      ? `    char ${scalarRecord.cField}[${opts.caps.listStrCap}];`
      : `    ${cScalarType(scalarRecord.cType)} ${scalarRecord.cField};`;
  const scalarBlock = scalarRecords.length
    ? `typedef struct\n{\n${scalarRecords.map(scalarFieldDecl).join('\n')}\n} ` +
      `ErAppState;\n\nstatic ErAppState s_state = ` +
      `{${scalarRecords.map(scalarRecord => ` .${scalarRecord.cField} = ${scalarRecord.initCode}`).join(',')} ` +
      `};\n`
    : '';
  const stateBlock = [scalarBlock, listBlocks].filter(Boolean).join('\n');

  // A file-scope handle for each node app_update() re-applies.
  const handleDecls = out.handles
    .map(nodeId => `static ERNode* s_${nodeId};`)
    .join('\n');

  // Static per-value ref, skipping refs the C never touches, so consumer builds don't warn about unused statics.
  const refDecls = [...refs.values(), ...out.childRefs]
    .filter(refRecord => refRecord.used)
    .map(
      refRecord =>
        `static ` +
        `${refRecord.kind === 'value' ? cScalarType(refRecord.cType) : refRecord.cType} ` +
        `${refRecord.cVar} = ${refRecord.initCode};`,
    )
    .join('\n');

  // Baked vector op-tapes + paint tables (static <Svg> geometry), emitted at file scope.
  const vectorBlock = out.vectorData.join('\n\n');
  // build_svgN() recompute functions (state-driven Svgs) — declared before app_update, which calls them.
  const vectorBuilderBlock = out.vectorBuilders.join('\n\n');

  // Animated values — one engine-side handle each, created at the top of er_app_build (binds reference them).
  const animList = [...anims.values(), ...out.childAnimations];
  const animDecls = animList
    .map(animValue => `static ERAnimValueHandle ${animValue.cVar};`)
    .join('\n');
  const animCreate = animList
    .map(
      animValue =>
        `    ${animValue.cVar} = er_anim_value_create(${animValue.initCode});`,
    )
    .join('\n');

  // app_update() exists only when something has to be re-applied after a state change.
  const hasUpdate =
    out.updates.length > 0 ||
    out.svgUpdates.length > 0 ||
    out.depEffects.length > 0;
  // With nothing to re-apply, there is no app_update, so the calls queued by setters go too.
  if (!hasUpdate) {
    const dropUpdate = (lines: string[]) =>
      lines.filter(line => line !== APP_UPDATE_CALL);
    for (const cFunction of [
      ...out.handlers,
      ...out.timerFns,
      ...out.animCbs,
      ...out.effectFns,
    ]) {
      cFunction.body = dropUpdate(cFunction.body!);
    }
    out.mountEffects = dropUpdate(out.mountEffects);
  }

  // app_update(): re-apply state-driven node props, rebuild state-driven Svgs, then run dep-driven effects.
  const updateBlock = (() => {
    if (!hasUpdate) return '';
    const lines = ['static void app_update(void)', '{'];
    if (out.updates.length) {
      lines.push('    ERProps p;');
    }
    for (const update of out.updates) {
      lines.push(`    er_props_default(&p);`);
      for (const assign of update.styleAssigns) {
        lines.push(`    p.${assign.field} = ${assign.expr};`);
      }
      for (const assign of update.dynAssigns) {
        lines.push(`    p.${assign.field} = ${assign.code};`);
      }
      if (update.placeholder != null) {
        lines.push(
          `    snprintf(p.placeholder, sizeof(p.placeholder), "%s", ${cstr(update.placeholder)});`,
        );
      }
      if (update.imageName != null) {
        lines.push(
          `    snprintf(p.image_name, sizeof(p.image_name), "%s", ${update.imageName});`,
        );
      }
      if (update.text) {
        if (update.text.args.length) {
          lines.push(
            `    snprintf(p.text, sizeof(p.text), ${cstr(update.text.format)}, ${update.text.args.join(', ')});`,
          );
        } else {
          lines.push(
            `    snprintf(p.text, sizeof(p.text), "%s", ${cstr(update.text.format.replace(/%%/g, '%'))});`,
          );
        }
      }
      lines.push(`    er_node_set_props(s_${update.nodeId}, &p);`);
    }
    // State-driven Svgs: recompute the op-tape from state and re-upload.
    for (const svgUpdate of out.svgUpdates) {
      lines.push(`    build_svg${svgUpdate.id}();`);
      lines.push(
        `    er_node_set_vector_ops(${svgUpdate.nodeVar}, s_svg${svgUpdate.id}_ops, ` +
          `${svgUpdate.len}, s_svg${svgUpdate.id}_paints, ${svgUpdate.nPaints}, ` +
          `${svgUpdate.nGrads ? `s_svg${svgUpdate.id}_grads` : 'NULL'}, ` +
          `${svgUpdate.nGrads || 0});`,
      );
    }
    // Dep-driven useEffect: run each effect whose dependency value changed since the last app_update.
    for (const block of out.depEffects) {
      lines.push(block);
    }
    lines.push('}');
    return lines.join('\n');
  })();

  // PanResponder gesture state, plus should-set predicates: bool callbacks the engine calls during negotiation.
  const panDeclBlock = out.panDecls.join('\n');
  const queryDefs = out.queries
    .map(
      query =>
        `static bool ${query.name}(ERNode* node, const EREventData* data, void* ` +
        `user_data)\n{\n    (void)node;\n    (void)data;\n    (void)user_data;\n    return ` +
        `${query.expr};\n}`,
    )
    .join('\n\n');

  // Event handlers, each with the engine's event callback signature.
  const handlerDefs = out.handlers
    .map(
      handler =>
        `static void ${handler.name}(ERNode* node, const EREventData* data, void* ` +
        `user_data)\n{\n    (void)node;\n    (void)data;\n    ` +
        `(void)user_data;\n${handler.body.join('\n')}\n}`,
    )
    .join('\n\n');

  // Dep-driven effects: a previous-value static per dep, the bodies, and forward decls (app_update comes first).
  const effectDeclsBlock = out.effectDecls.join('\n');
  const effectFwdDecls = out.effectFns
    .map(effectFn => `static void ${effectFn.name}(void);`)
    .join('\n');
  const effectFnDefs = out.effectFns
    .map(
      effectFn =>
        `static void ${effectFn.name}(void)\n{\n${effectFn.body.join('\n')}\n}`,
    )
    .join('\n\n');

  // setInterval/setTimeout: a fixed timer table er_app_tick advances, emitted only when the app uses timers.
  const timerTableBlock = out.usesTimers
    ? `#include <stdbool.h>

#ifndef ER_AOT_MAX_TIMERS
#define ER_AOT_MAX_TIMERS 8
#endif

typedef struct
{
    int interval_ms;
    int remaining_ms;
    int gen;
    bool repeat;
    bool active;
    void (*fn)(void);
} ErTimer;
static ErTimer s_timers[ER_AOT_MAX_TIMERS];

/* An id carries the slot AND the generation that owns it, so clearing an id whose timer has already
   finished (a one-shot, or an earlier clear) cannot kill whatever took the slot next. */
static int er_timer_add(int ms, bool repeat, void (*fn)(void))
{
    for (int i = 0; i < ER_AOT_MAX_TIMERS; i++)
    {
        if (!s_timers[i].active)
        {
            s_timers[i].interval_ms = ms < 1 ? 1 : ms;
            s_timers[i].remaining_ms = s_timers[i].interval_ms;
            s_timers[i].gen = (s_timers[i].gen + 1) & 0xFFFF;
            s_timers[i].repeat = repeat;
            s_timers[i].active = true;
            s_timers[i].fn = fn;
            return (s_timers[i].gen * ER_AOT_MAX_TIMERS) + i;
        }
    }
    return -1; /* table full (raise ER_AOT_MAX_TIMERS) */
}`
    : '';

  const timerFnDefs = out.timerFns
    .map(
      timerFn =>
        `static void ${timerFn.name}(void)\n{\n${timerFn.body!.join('\n')}\n}`,
    )
    .join('\n\n');

  // Forward-declare every timer fn: the handlers and effect fns that register one are defined before it.
  const timerFnFwdDecls = out.timerFns
    .map(timerFn => `static void ${timerFn.name}(void);`)
    .join('\n');

  // Animated.sequence on_complete callbacks each start the next step, so they are forward-declared.
  const animCbDecls = out.animCbs
    .map(
      animCb => `static void ${animCb.name}(bool finished, void* user_data);`,
    )
    .join('\n');
  const animCbDefs = out.animCbs
    .map(
      animCb =>
        `static void ${animCb.name}(bool finished, void* user_data)\n{\n    (void)finished;\n ` +
        `   (void)user_data;\n${animCb.body.join('\n')}\n}`,
    )
    .join('\n\n');

  // Apply a wall-clock change on the app loop, not in the setter: the host may set it before er_app_build().
  const wallClockRefresh = hasUpdate
    ? '    if (s_wall_clock_changed)\n    {\n        s_wall_clock_changed = 0;\n        app_update();\n    }\n'
    : '';

  // er_app_tick(): fire due timers; without timers it only applies a clock change, so a host can always call it.
  const appTickFn = out.usesTimers
    ? `void er_app_tick(int dt_ms)
{
${wallClockRefresh}    for (int i = 0; i < ER_AOT_MAX_TIMERS; i++)
    {
        if (!s_timers[i].active)
        {
            continue;
        }
        s_timers[i].remaining_ms -= dt_ms;
        if (s_timers[i].remaining_ms <= 0)
        {
            void (*fn)(void) = s_timers[i].fn;
            if (s_timers[i].repeat)
            {
                s_timers[i].remaining_ms += s_timers[i].interval_ms;
                if (s_timers[i].remaining_ms <= 0)
                {
                    s_timers[i].remaining_ms = s_timers[i].interval_ms; /* dt ran long; don't spiral */
                }
            }
            else
            {
                s_timers[i].active = false;
            }
            if (fn)
            {
                fn();
            }
        }
    }
}`
    : `void er_app_tick(int dt_ms)\n{\n    (void)dt_ms;\n${wallClockRefresh}}`;

  // useEffect(fn, []) bodies, run once at the end of er_app_build().
  const mountEffectsBlock = out.mountEffects.length
    ? '\n    /* useEffect(fn, []) — run once on mount. */\n' +
      out.mountEffects.join('\n') +
      '\n'
    : '';

  // app_round_dim() when a state-driven style value has to be snapped to a whole pixel at runtime.
  const usesDimRound = /\bapp_round_dim\(/.test(
    [
      updateBlock,
      handlerDefs,
      queryDefs,
      effectFnDefs,
      animCbDefs,
      timerFnDefs,
      out.mountEffects.join('\n'),
      out.build.join('\n'),
    ].join('\n'),
  );
  const dimRoundBlock = usesDimRound
    ? `
/* Snaps a state-driven style value to a whole pixel the way JavaScript's Math.round does — floor(x + 0.5),
   halves UP rather than away from zero. ERProps dimensions are int16 and a plain cast would truncate toward
   zero, which is how the two flows once laid the same app out a pixel apart. Values known at compile time
   are folded with Math.round; this is that rule's runtime twin, and Flow A's bridge applies the same one. */
static int16_t app_round_dim(double v)
{
    /* NaN compares false against everything, so it would slip past both clamps into an undefined cast. */
    if (v != v)
    {
        return 0;
    }
    if (v < -32768.0)
    {
        return -32768;
    }
    if (v > 32767.0)
    {
        return 32767;
    }
    const double r = v + 0.5;
    const int32_t t = (int32_t)r; /* truncates toward zero */
    return (int16_t)(((double)t > r) ? t - 1 : t);
}
`
    : '';

  // Include <math.h> when the generated code, or a helper it calls, uses a libm symbol.
  const usesMath =
    out.needsMath ||
    LIBM_USE.test(
      [
        stateBlock,
        refDecls,
        vectorBuilderBlock,
        updateBlock,
        handlerDefs,
        queryDefs,
        animCbDefs,
        timerFnDefs,
        out.mountEffects.join('\n'),
        out.build.join('\n'),
      ].join('\n'),
    );

  // Each useHostValue gets a public setter that writes its field and refreshes via app_update(), like a JS setter.
  const hostRecords = scalarRecords.filter(scalarRecord => scalarRecord.host);
  if (hostRecords.some(hostRecord => hostRecord.name === 'wall_clock')) {
    throw aotError(
      'AOT: useHostValue("wall_clock") would collide with er_app_set_wall_clock()',
      'rename the host value — every app exports er_app_set_wall_clock() for Date.now().',
    );
  }
  const hostSettersBlock = hostRecords
    .map(
      hostRecord =>
        `void er_app_set_${hostRecord.name}(${cScalarType(hostRecord.cType)} v)\n{\n    ` +
        `${hostRecord.cMember} = v;\n${hasUpdate ? '    app_update();\n' : ''}}`,
    )
    .join('\n\n');
  const hostSetterProtos = hostRecords
    .map(
      hostRecord =>
        `/** @brief Host-fed input '${hostRecord.name}' (useHostValue): set its value and ` +
        `refresh the display. */\nvoid ` +
        `er_app_set_${hostRecord.name}(${cScalarType(hostRecord.cType)} v);`,
    )
    .join('\n\n');

  // File-local helpers (clock readers, JS-exact math), each emitted only when generated code calls it.
  const APP_HELPERS = [
    [
      'app_perf_now',
      'static int64_t app_perf_now(void)\n{\n    return (int64_t)er_now_ms64();\n}',
    ],
    [
      'app_date_now',
      'static int64_t app_date_now(void)\n{\n    return s_wall_offset_ms + (int64_t)er_now_ms64();\n}',
    ],
    ...Object.entries(CHECKED_OP).map(([operator, name]) => [
      name,
      `/* a ${operator} b, saturated to the int range: C leaves a signed overflow undefined. */\n` +
        `static int ${name}(int a, int b)\n{\n    const int64_t v = (int64_t)a ${operator} b;\n` +
        '    return v > INT_MAX ? INT_MAX : v < INT_MIN ? INT_MIN : (int)v;\n}',
    ]),
    [
      'app_neg',
      '/* -a, saturated: -INT_MIN does not fit an int. */\n' +
        'static int app_neg(int a)\n{\n    return a == INT_MIN ? INT_MAX : -a;\n}',
    ],
    [
      'app_mod',
      "/* a % b as JS computes it, kept whole: the remainder has the dividend's sign, as in C. JS gives NaN\n" +
        '   for b == 0, which is 0 here, and INT_MIN % -1 is 0, where C would overflow. */\n' +
        'static int app_mod(int a, int b)\n{\n    return (b == 0 || b == -1) ? 0 : a % b;\n}',
    ],
    [
      'app_abs',
      '/* Math.abs of an int, saturated: -INT_MIN does not fit an int. */\n' +
        'static int app_abs(int v)\n{\n    return v >= 0 ? v : v == INT_MIN ? INT_MAX : -v;\n}',
    ],
    [
      'app_min',
      'static int app_min(int a, int b)\n{\n    return a < b ? a : b;\n}',
    ],
    [
      'app_max',
      'static int app_max(int a, int b)\n{\n    return a > b ? a : b;\n}',
    ],
    [
      'app_div',
      '/* a / b as JS computes it, kept whole: truncated toward zero, and saturated where JS gives +-Infinity\n' +
        '   (b == 0) or a result past the int range (INT_MIN / -1). 0 / 0 is NaN, which is 0. */\n' +
        'static int app_div(int a, int b)\n{\n' +
        '    if (b == 0)\n    {\n        return a > 0 ? INT_MAX : a < 0 ? INT_MIN : 0;\n    }\n' +
        '    if (b == -1)\n    {\n        return a == INT_MIN ? INT_MAX : -a;\n    }\n' +
        '    return a / b;\n}',
    ],
    [
      'app_floordiv',
      "/* Math.floor(a / b) for two ints, exactly: C's `/` rounds toward zero where floor rounds down, and a float\n" +
        '   quotient would round an operand past 2^24. JS gives +-Infinity for b == 0, which saturates by the sign\n' +
        '   of a (0 / 0 is NaN, which is 0); INT_MIN / -1 saturates too, so the quotient is taken in 64 bits. */\n' +
        'static int app_floordiv(int a, int b)\n{\n' +
        '    if (b == 0)\n    {\n        return a > 0 ? INT_MAX : a < 0 ? INT_MIN : 0;\n    }\n' +
        '    const int64_t q = (int64_t)a / b;\n' +
        '    const int64_t f = ((int64_t)a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;\n' +
        '    return f > INT_MAX ? INT_MAX : (int)f;\n}',
    ],
    [
      'app_ceildiv',
      '/* Math.ceil(a / b) for two ints, exactly, with the zero divisor and INT_MIN / -1 as ' +
        'app_floordiv has them. */\n' +
        'static int app_ceildiv(int a, int b)\n{\n' +
        '    if (b == 0)\n    {\n        return a > 0 ? INT_MAX : a < 0 ? INT_MIN : 0;\n    }\n' +
        '    const int64_t q = (int64_t)a / b;\n' +
        '    const int64_t c = ((int64_t)a % b != 0 && (a < 0) == (b < 0)) ? q + 1 : q;\n' +
        '    return c > INT_MAX ? INT_MAX : (int)c;\n}',
    ],
    [
      'app_rounddiv',
      '/* Math.round(a / b) for two ints, exactly: floor(a / b + 1/2), halves up as JS rounds, which is\n' +
        '   floor((2a + b) / 2b) once b is made positive. The zero divisor and the range go as in app_floordiv. */\n' +
        'static int app_rounddiv(int a, int b)\n{\n' +
        '    if (b == 0)\n    {\n        return a > 0 ? INT_MAX : a < 0 ? INT_MIN : 0;\n    }\n' +
        '    const int64_t n = b < 0 ? -(int64_t)a : a;\n' +
        '    const int64_t d = b < 0 ? -(int64_t)b : b;\n' +
        '    const int64_t num = 2 * n + d, den = 2 * d;\n' +
        '    const int64_t q = num / den;\n' +
        '    const int64_t r = (num % den != 0 && num < 0) ? q - 1 : q;\n' +
        '    return r > INT_MAX ? INT_MAX : r < INT_MIN ? INT_MIN : (int)r;\n}',
    ],
    [
      'app_f2i',
      '/* A float kept as an int: truncated toward zero like a C cast, but saturated at the int range and NaN as\n' +
        '   0, where the cast would be undefined behavior. */\n' +
        'static int app_f2i(float v)\n{\n' +
        '    if (v != v)\n    {\n        return 0;\n    }\n' +
        '    if (v >= 2147483648.0f)\n    {\n        return INT_MAX;\n    }\n' +
        '    if (v <= -2147483648.0f)\n    {\n        return INT_MIN;\n    }\n' +
        '    return (int)v;\n}',
    ],
    [
      'app_roundf',
      "/* Math.round as JS rounds: to the nearest whole number, halves up (toward +Infinity). C's roundf takes\n" +
        '   halves away from zero, which gives -3 for -2.5 where JS gives -2. */\n' +
        'static float app_roundf(float v)\n{\n' +
        '    const float f = floorf(v);\n    return v - f >= 0.5f ? f + 1.0f : f;\n}',
    ],
    [
      'app_ftoa',
      "/* A float as JS's String() spells it where %g does not: Infinity, -Infinity and NaN for what %g prints as\n" +
        '   inf, -inf and nan, and 0 for negative zero, which %g prints as -0. Anything else is %g, into `buf`,\n' +
        '   which the caller lends (16 chars, room for any float). All ASCII, which every font bakes. */\n' +
        'static const char* app_ftoa(char* buf, float v)\n{\n' +
        '    if (v != v)\n    {\n        return "NaN";\n    }\n' +
        '    if (isinf(v))\n    {\n        return v > 0.0f ? "Infinity" : "-Infinity";\n    }\n' +
        '    if (v == 0.0f)\n    {\n        return "0";\n    }\n' +
        '    snprintf(buf, 16, "%g", (double)v);\n    return buf;\n}',
    ],
    [
      'app_opacity',
      "/* A state-driven opacity as the engine's 0-255 byte: clamped to 0..1 and rounded, as Flow A's bridge and\n" +
        '   the compile-time fold do, with NaN as 0. */\n' +
        'static uint8_t app_opacity(float v)\n{\n' +
        '    if (!(v > 0.0f))\n    {\n        return 0;\n    }\n' +
        '    if (v >= 1.0f)\n    {\n        return 255;\n    }\n' +
        '    return (uint8_t)(v * 255.0f + 0.5f);\n}',
    ],
    [
      'app_add64',
      '/* a + b, saturated to the int64 range: C leaves a signed overflow undefined. */\n' +
        'static int64_t app_add64(int64_t a, int64_t b)\n{\n' +
        '    if (b > 0 ? a > INT64_MAX - b : a < INT64_MIN - b)\n    {\n' +
        '        return b > 0 ? INT64_MAX : INT64_MIN;\n    }\n    return a + b;\n}',
    ],
    [
      'app_sub64',
      '/* a - b, saturated to the int64 range: C leaves a signed overflow undefined. */\n' +
        'static int64_t app_sub64(int64_t a, int64_t b)\n{\n' +
        '    if (b < 0 ? a > INT64_MAX + b : a < INT64_MIN + b)\n    {\n' +
        '        return b < 0 ? INT64_MAX : INT64_MIN;\n    }\n    return a - b;\n}',
    ],
    [
      'app_mul64',
      '/* a * b, saturated to the int64 range: C leaves a signed overflow undefined. GCC and Clang test for\n' +
        '   the overflow with a builtin; anywhere else (MSVC) the limits are checked by division first. */\n' +
        'static int64_t app_mul64(int64_t a, int64_t b)\n{\n' +
        '#if defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 5)\n' +
        '    int64_t v;\n    if (!__builtin_mul_overflow(a, b, &v))\n    {\n        return v;\n    }\n' +
        '#else\n' +
        '    if (a == 0 || b == 0 ||\n' +
        '        (a > 0 ? (b > 0 ? a <= INT64_MAX / b : b >= INT64_MIN / a)\n' +
        '               : (b > 0 ? a >= INT64_MIN / b : a >= INT64_MAX / b)))\n    {\n' +
        '        return a * b;\n    }\n' +
        '#endif\n' +
        '    return (a < 0) == (b < 0) ? INT64_MAX : INT64_MIN;\n}',
    ],
    [
      'app_neg64',
      '/* -a, saturated: -INT64_MIN does not fit an int64_t. */\n' +
        'static int64_t app_neg64(int64_t a)\n{\n    return a == INT64_MIN ? INT64_MAX : -a;\n}',
    ],
    [
      'app_floordiv64',
      "/* Math.floor(a / b) in whole numbers: C's `/` rounds toward zero where floor rounds down. JS gives\n" +
        '   +-Infinity for b == 0, which saturates by the sign of a (0 / 0 is NaN, which is 0), and INT64_MIN / -1\n' +
        '   is past the range, so it saturates too. */\n' +
        'static int64_t app_floordiv64(int64_t a, int64_t b)\n{\n' +
        '    if (b == 0)\n    {\n        return a > 0 ? INT64_MAX : a < 0 ? INT64_MIN : 0;\n    }\n' +
        '    if (b == -1)\n    {\n        return a == INT64_MIN ? INT64_MAX : -a;\n    }\n' +
        '    const int64_t q = a / b;\n    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;\n}',
    ],
    [
      'app_ceildiv64',
      '/* Math.ceil(a / b) in whole numbers, with the zero divisor and INT64_MIN / -1 as ' +
        'app_floordiv64 has them. */\n' +
        'static int64_t app_ceildiv64(int64_t a, int64_t b)\n{\n' +
        '    if (b == 0)\n    {\n        return a > 0 ? INT64_MAX : a < 0 ? INT64_MIN : 0;\n    }\n' +
        '    if (b == -1)\n    {\n        return a == INT64_MIN ? INT64_MAX : -a;\n    }\n' +
        '    const int64_t q = a / b;\n    return (a % b != 0 && (a < 0) == (b < 0)) ? q + 1 : q;\n}',
    ],
    [
      'app_rounddiv64',
      '/* Math.round(a / b) in whole numbers, halves up as JS rounds: floor(a / b), plus ' +
        'one when what is left is at\n' +
        '   least half of b. Nothing is doubled, which could overflow; the zero divisor and INT64_MIN / -1 go as in\n' +
        '   app_floordiv64. */\n' +
        'static int64_t app_rounddiv64(int64_t a, int64_t b)\n{\n' +
        '    if (b == 0)\n    {\n        return a > 0 ? INT64_MAX : a < 0 ? INT64_MIN : 0;\n    }\n' +
        '    if (b == -1)\n    {\n        return a == INT64_MIN ? INT64_MAX : -a;\n    }\n' +
        '    int64_t q = a / b;\n    int64_t r = a % b;\n' +
        '    if (r != 0 && (r < 0) != (b < 0))\n    {\n        q -= 1;\n        r += b;\n    }\n' +
        '    return (b > 0 ? r >= b - r : r <= b - r) ? q + 1 : q;\n}',
    ],
    [
      'app_div64',
      '/* Math.trunc(a / b) in whole numbers, with the zero divisor and INT64_MIN / -1 as ' +
        'app_floordiv64 has them. */\n' +
        'static int64_t app_div64(int64_t a, int64_t b)\n{\n' +
        '    if (b == 0)\n    {\n        return a > 0 ? INT64_MAX : a < 0 ? INT64_MIN : 0;\n    }\n' +
        '    if (b == -1)\n    {\n        return a == INT64_MIN ? INT64_MAX : -a;\n    }\n' +
        '    return a / b;\n}',
    ],
    [
      'app_mod64',
      "/* a % b in whole numbers, as JS computes it: the remainder has the dividend's sign, as in C. JS gives NaN\n" +
        '   for b == 0, which is 0 here, and INT64_MIN % -1 is 0, where C would overflow. */\n' +
        'static int64_t app_mod64(int64_t a, int64_t b)\n{\n    return (b == 0 || b == -1) ? 0 : a % b;\n}',
    ],
    [
      'app_abs64',
      '/* |v|, saturated: -INT64_MIN does not fit an int64_t. */\n' +
        'static int64_t app_abs64(int64_t v)\n{\n    return v >= 0 ? v : v == INT64_MIN ? INT64_MAX : -v;\n}',
    ],
    [
      'app_min64',
      'static int64_t app_min64(int64_t a, int64_t b)\n{\n    return a < b ? a : b;\n}',
    ],
    [
      'app_max64',
      'static int64_t app_max64(int64_t a, int64_t b)\n{\n    return a > b ? a : b;\n}',
    ],
    [
      'app_delay_ms64',
      '/* A timer delay computed from a timestamp: ToInt32 (wrap to 32 bits), then a negative delay is 0 — the\n' +
        "   conversion Flow A's setTimeout applies. */\n" +
        'static int app_delay_ms64(int64_t v)\n{\n    const uint32_t u = (uint32_t)v;\n    ' +
        'return u > 0x7FFFFFFFu ? 0 : (int)u;\n}',
    ],
    [
      'app_delay_msf',
      '/* A timer delay computed as a float, converted as app_delay_ms64 converts one: ToInt32 (NaN and +-Infinity\n' +
        '   are 0), then a negative delay is 0. Past +-2^63 a float is a multiple of 2^32, ' +
        'which ToInt32 takes to 0. */\n' +
        'static int app_delay_msf(float v)\n{\n' +
        '    if (!(v > -9223372036854775808.0f && v < 9223372036854775808.0f))\n    {\n        return 0;\n    }\n' +
        '    const uint32_t u = (uint32_t)(int64_t)v;\n    return u > 0x7FFFFFFFu ? 0 : (int)u;\n}',
    ],
    [
      'app_slice_len',
      '/* The length items.slice(0, end) leaves, as JS counts it: a negative end counts back from the length and\n' +
        '   stops at 0, and an end past the length keeps every item. */\n' +
        'static int app_slice_len(int len, int end)\n{\n' +
        '    if (end < 0)\n    {\n        return end <= -len ? 0 : len + end;\n    }\n' +
        '    return end < len ? end : len;\n}',
    ],
    [
      'app_vector_dirty',
      "/* updateVector's damage hint from float math. A NaN or infinite edge (a 0/0 in app math) gives no hint,\n" +
        "   so the engine's own damage applies (what changed in the tape, or the whole node), where a zero-width\n" +
        '   one would leave the new drawing unpainted. The rest is rounded out to whole pixels and its edges, not\n' +
        '   its lengths, are bounded well inside the int range, so a rect reaching past the bound is not cut\n' +
        "   short; the engine clips the rect's corners from there. */\n" +
        'static void app_vector_dirty(ERNode* node, float x, float y, float w, float h)\n{\n' +
        '    if (!(isfinite(x) && isfinite(y) && isfinite(w) && isfinite(h)))\n    {\n        return;\n    }\n' +
        '    const float e[4] = {floorf(x), floorf(y), ceilf(x + w), ceilf(y + h)};\n    int c[4];\n' +
        '    for (int k = 0; k < 4; k++)\n    {\n' +
        '        c[k] = (int)(e[k] < -1e9f ? -1e9f : (e[k] > 1e9f ? 1e9f : e[k]));\n    }\n' +
        '    er_node_set_vector_dirty_rect(node, c[0], c[1], c[2] - c[0], c[3] - c[1]);\n}',
    ],
  ];

  // Generated code with literals and comments blanked, so only a real call emits a helper (unused statics warn).
  const helperCallers = stripCLiterals(
    [
      stateBlock,
      refDecls,
      vectorBuilderBlock,
      updateBlock,
      handlerDefs,
      queryDefs,
      effectFnDefs,
      animCbDefs,
      timerFnDefs,
      out.mountEffects.join('\n'),
      out.build.join('\n'),
    ].join('\n'),
  );

  // The clock state, then each helper the generated code calls.
  const clockBlock = [
    '/* Date.now() is the engine clock plus this offset, so it reads as uptime until the host calls\n' +
      '   er_app_set_wall_clock(). performance.now() is the engine clock alone. */\n' +
      'static int64_t s_wall_offset_ms;' +
      (hasUpdate
        ? '\n/* Set by er_app_set_wall_clock(); the next er_app_tick() re-applies what reads the clock. */\n' +
          'static int s_wall_clock_changed;'
        : ''),
    ...APP_HELPERS.filter(([name]) => helperCallers.includes(`${name}(`)).map(
      ([, definition]) => definition,
    ),
  ].join('\n\n');
  // <limits.h> when a helper saturates at INT_MAX / INT_MIN.
  const usesLimits = /\bINT_M(?:AX|IN)\b/.test(clockBlock);
  // Emit er_timer_clear only when called; a clearInterval in a dropped mount-effect cleanup leaves no caller.
  const timerClearBlock =
    out.usesTimers && helperCallers.includes('er_timer_clear(')
      ? `static void er_timer_clear(int id)
{
    if (id < 0)
    {
        return;
    }
    int i = id % ER_AOT_MAX_TIMERS;
    if (s_timers[i].active && s_timers[i].gen == id / ER_AOT_MAX_TIMERS)
    {
        s_timers[i].active = false;
    }
}`
      : '';

  // The longer pieces of app.gen.c, named here so the template below stays readable.
  const versionMismatch =
    `embedded-react version mismatch: app.gen.c was generated by ${PKG_VERSION} but the engine header ` +
    `(er_version.h) is a different major.minor. Regenerate the app with 'npm run aot', or align the engine ` +
    `and npm versions.`;
  const mathInclude = usesMath
    ? '#include <math.h>\n' +
      '/* M_PI is not in ISO C99 <math.h> (only POSIX/GNU); define a fallback so the generated app compiles ' +
      'under -std=c99 / MSVC. */\n' +
      '#ifndef M_PI\n#define M_PI 3.14159265358979323846\n#endif\n'
    : '';
  const limitsInclude = usesLimits ? '#include <limits.h>\n' : '';
  // A blank line sets off each non-empty block before and after.
  const spaced = (block: string) => (block ? '\n' + block + '\n' : '');
  const declarations = [
    refDecls,
    panDeclBlock,
    effectDeclsBlock,
    vectorBlock,
    vectorBuilderBlock,
    animDecls,
    handleDecls,
    timerTableBlock,
    timerClearBlock,
    effectFwdDecls,
    updateBlock,
    timerFnFwdDecls,
    animCbDecls,
    handlerDefs,
    queryDefs,
    effectFnDefs,
    animCbDefs,
    timerFnDefs,
    out.kbdData,
  ]
    .map(spaced)
    .join('');
  const keyboardSetup = out.kbdSetup
    ? out.kbdSetup +
      ' /* app-supplied on-screen keyboard layout/appearance */\n'
    : '';
  const initialUpdate = hasUpdate
    ? '\n    app_update(); /* apply initial state-dependent props */\n'
    : '';

  // app.gen.c: includes and version pin, then file-scope state, declarations and functions, then er_app_build().
  const body = `/*
 * Generated by the embedded-react Flow B AOT compiler (npm run aot -- ${demo}). DO NOT EDIT.
 * Builds the app's scene graph + state machine directly against er_scene.h — no QuickJS, no JS runtime.
 */
#include "app.gen.h"

#include "er_scene.h"
#include "er_version.h"

#include <stdio.h>
#include <string.h>

/* Every string this file writes goes into a FIXED-SIZE slot (a state buffer, ERProps.text), so an
   over-long value is truncated by design — the app cannot grow the buffer the way JS grows a string.
   GCC's -Wformat-truncation reports exactly that intent for any format combining %s with anything else,
   and ESP-IDF compiles with -Werror, so leaving it on would fail the build for ordinary text like
   {'n=' + name}. Clang does not implement the warning; the guard keeps its "unknown warning group"
   diagnostic from firing there. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

/* Version-pin: this file was generated by embedded-react ${PKG_VERSION}. The engine ships LOCKSTEP, so its
   headers must be the same major.minor — otherwise these generated er_scene.h calls may not match the ABI.
   A mismatch fails HERE at compile time (not on-device). Regenerate the app (npm run aot) or align versions. */
_Static_assert(ER_VERSION_MAJOR == ${PKG_MAJOR} && ER_VERSION_MINOR == ${PKG_MINOR},
               "${versionMismatch}");
${mathInclude}${limitsInclude}${dimRoundBlock}\n${clockBlock}\n${stateBlock ? '\n' + stateBlock : ''}${declarations}
${appTickFn}

void er_app_set_wall_clock(int64_t epoch_ms)
{
    s_wall_offset_ms = epoch_ms - (int64_t)er_now_ms64();${hasUpdate ? '\n    s_wall_clock_changed = 1;' : ''}
}
${hostSettersBlock ? '\n' + hostSettersBlock + '\n' : ''}
void er_app_build(int screen_w, int screen_h)
{
    ERProps p;
    ERNode* ${nodeDecls.join(';\n    ERNode* ')};

    /* A screen-sized root the app tree fills (mirrors AppRegistry mounting into a screen-sized host). */
    ERNode* root = er_node_create(ER_NODE_VIEW);
    er_props_default(&p);
    p.width = (int16_t)screen_w;
    p.height = (int16_t)screen_h;
    er_node_set_props(root, &p);
${animCreate ? '\n' + animCreate + '\n' : ''}
${out.build.join('\n')}
    er_tree_append_child(root, ${rootNodeId});
    er_tree_set_root(root);
${keyboardSetup}${initialUpdate}${mountEffectsBlock}}
`;

  // app.gen.h: what the file was generated for, and the app's public entry points.
  const header = `/* Generated by the embedded-react Flow B AOT compiler. DO NOT EDIT. */
#ifndef ER_APP_GEN_H
#define ER_APP_GEN_H

#include <stdint.h>

/*
 * What this file was generated FOR. er_app_build() takes the screen size at runtime, but a responsive app
 * folds its \`screen.width\`/\`screen.height\` branching at GENERATE time — so the layout in here is already
 * committed to these dimensions and no runtime argument can change it.
 *
 * Every board example consumes the same dist/app.gen.c, so generating for one board and then building
 * another produces firmware that compiles, links, boots, and lays out wrong. Boards \`_Static_assert\` these
 * against their own panel size to turn that into a compile error; see each board example's main.c.
 *
 * WHICH demo is recorded the same way, as a marker macro named ER_AOT_DEMO_<demo> with every character
 * outside [A-Za-z0-9] encoded as _<hex>_ (so \`watch-face\` defines ER_AOT_DEMO_watch_2d_face). The encoding
 * is one-to-one, so no two app names can ever share a marker. A board that
 * needs a particular demo's useHostValue setters guards on \`#ifndef\` of its own marker, which names the
 * mismatch instead of leaving a pile of implicit-declaration errors for those setters.
 */
#define ER_AOT_SCREEN_W ${screen.width}
#define ER_AOT_SCREEN_H ${screen.height}
#define ER_AOT_DEMO ${cstr(demo)}
#define ${demoMarker(demo)} 1

/** @brief Builds the AOT-compiled app's scene graph + state machine (call once after backend init). */
void er_app_build(int screen_w, int screen_h);

/** @brief Advances app timers (setInterval/setTimeout). Call once per frame with the elapsed ms; a no-op
 *         for apps that use no timers, so it is always safe to call. */
void er_app_tick(int dt_ms);

/** @brief Tells the app the current time, which Date.now() counts on from. Until it is called Date.now()
 *         reads as uptime; a later call re-anchors it, the next er_app_tick() re-applies what reads the
 *         clock, and performance.now() never moves with it. Call it from the loop that calls
 *         er_app_tick(), before or after er_app_build(); safe whether or not the app reads the clock.
 *  @param[in] epoch_ms  Current time, in milliseconds since the Unix epoch. */
void er_app_set_wall_clock(int64_t epoch_ms);
${hostSetterProtos ? '\n' + hostSetterProtos + '\n' : ''}
#endif
`;

  // The image imports the app references; the CLI resolves each path against the demo dir and bakes them.
  const images = [...out.images.entries()].map(([name, importPath]) => ({
    name,
    importPath,
  }));
  return {
    c: body,
    h: header,
    nodes: out.nodeCount,
    state: stateRecords.length,
    handlers: out.handlers.length,
    updates: out.updates.length,
    images,
  };
}
