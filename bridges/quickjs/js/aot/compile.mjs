// `npm run aot [demo]` — the Flow B ahead-of-time compiler (vertical slice).
//
// Compiles a demo's JSX straight to C against er_scene.h: no QuickJS, no JS at runtime. The generated
// app.gen.c builds the engine node tree directly and wires state + events, so it fits an MCU with only
// internal RAM.
//
// Supported subset (grows demo by demo; unsupported syntax throws "AOT: ..."):
//   - View / Text / Pressable / Image / ScrollView elements
//   - StyleSheet + inline styles → ERProps (static values)
//   - text with literal + {interpolation} segments (interpolations may reference state)
//   - useState(initial) → C state; on* handlers (onPress/onPressIn/onPressOut/onLongPress) → C functions
//   - setState(value) and setState(prev => expr); a small C expression subset (literals, identifiers,
//     +-*/% , comparisons, ?:)
//
// The compiler tracks which nodes depend on which state, so a state change re-sets ONLY the dependent
// nodes (er_node_set_props) — no diffing, no reconciler. See /PLAN.md Flow B.
//
//   npm run aot                      # default demo (thermostat) — but use a minimal demo for the slice
//   npm run aot -- music-player      # a specific demo by folder name
import { parse } from '@babel/parser';
import { codeFrameColumns } from '@babel/code-frame';
import { readFileSync, writeFileSync, mkdirSync, existsSync, readdirSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { lowerStyle, NODE_TYPES, DYN_FIELDS, colorLiteral } from './style-map.mjs';
import { flattenSvg, parseColor, parsePath } from '../src/embedded-react/svg-ops.js';
import { bakeAssets } from '../assets/index.mjs';

const here = dirname(fileURLToPath(import.meta.url)); // bridges/quickjs/js/aot
const repoRoot = resolve(here, '../../../..');
const demosDir = resolve(repoRoot, 'demos');
const distDir = resolve(here, '..', 'dist');

// The core compiler is exported as compileSource(src) so it can be unit-tested on inline JSX. The CLI
// (read a demo's App.jsx, write dist/app.gen.{c,h}) lives in the entry guard at the bottom of this file.
//
// ---------------------------------------------------------------------------------------------------
// SECTION MAP (top → bottom). The pipeline: parse the App.jsx → collect the module's component, state,
// hooks & refs → emit each piece to C (expressions, styles/text, handlers, nodes) → assemble app.gen.c.
//
//   1.  Diagnostics              aotError / withLoc / formatAotError — locate + hint unsupported syntax
//   2.  Static evaluation        evalStatic — fold the compile-time-constant subset (styles, initials)
//   3.  C expression emission    emitExpr — lower a JS expression (state/props/refs) to a C expression
//   4.  Collection passes        moduleScope + collect{State,Components,Callbacks,Memos,Effects}
//   5.  Animations & refs        collect{Anims,Refs}, useAnimatedValue, Easing, interpolate (native driver)
//   6.  JSX → style/text/events  attrExpr, collectStyleAssigns, buildText / text spans
//   7.  Handler compilation      on* arrow → C statements (setters, refs, updateVector, Animated.start)
//   8.  Emit: control flow        components / conditionals / .map — all UNROLL at compile time
//   9.  Vector / Svg             <Svg> subtree → flattenSvg ops/paints → er_node_set_vector_ops
//   10. Node emitters            typed components (Switch/TextInput/Modal/…) + the generic host node
//   11. Keyboard config          setKeyboardConfig({...}) → static ERKeyboardConfig tables
//   12. Compile orchestration    compileSourceImpl — stitch the above into app.gen.{c,h}
//   13. CLI entry                node aot/compile.mjs [demo] → read App.jsx, write dist/app.gen.*
// ---------------------------------------------------------------------------------------------------

// ---------------------------------------------------------------------------------------------------
// Diagnostics — turn "AOT: <reason>" into "<reason> at file:line:col" + a source code-frame (+ a rewrite
// hint when one is attached). emitExpr / emitNode / compileHandlerExpr are wrapped (withLoc) so the
// DEEPEST node that failed pins the location; compileSource formats it at the top.
// ---------------------------------------------------------------------------------------------------

/** Throws an AOT error carrying an optional `hint` (a "rewrite it like this" suggestion shown to the user). */
function aotError(message, hint) {
  const e = new Error(message.startsWith('AOT:') ? message : `AOT: ${message}`);
  if (hint) e.aotHint = hint;
  return e;
}

/** Wraps an emit fn so a thrown AOT error (without a location yet) is tagged with the current node's loc. */
function withLoc(fn) {
  return function (node, ...rest) {
    try {
      return fn(node, ...rest);
    } catch (e) {
      if (e && typeof e.message === 'string' && e.message.startsWith('AOT:') && !e.aotLoc && node && node.loc) {
        e.aotLoc = node.loc.start; // babel loc: { line (1-based), column (0-based) }
      }
      throw e;
    }
  };
}

/** Re-throws an AOT error with `file:line:col`, a code-frame, and any hint folded into the message. */
function formatAotError(e, src, filename) {
  if (!e || !e.aotLoc) return e; // nothing to locate — leave the bare message
  const { line, column } = e.aotLoc;
  const loc = { start: { line, column: column + 1 } }; // code-frame columns are 1-based
  let frame = '';
  try {
    frame = codeFrameColumns(src, loc, { highlightCode: false });
  } catch {
    /* code-frame is best-effort */
  }
  const hint = e.aotHint ? `\n\nhint: ${e.aotHint}` : '';
  const out = new Error(`${e.message}\n  at ${filename}:${line}:${column + 1}\n\n${frame}${hint}`);
  out.aotLoc = e.aotLoc;
  if (e.aotHint) out.aotHint = e.aotHint;
  return out;
}

/**
 * evalStatic at a boundary that REQUIRES a compile-time constant. On a fold failure it rethrows as a
 * LOCATED aotError with a clear message (+ optional hint), instead of letting evalStatic's bare
 * control-flow error ("cannot statically resolve identifier …") leak to the user without a location.
 */
function evalStaticOrThrow(node, scope, message, hint) {
  try {
    return evalStatic(node, scope);
  } catch {
    const e = aotError(message, hint);
    if (node && node.loc) e.aotLoc = node.loc.start;
    throw e;
  }
}

// ---------------------------------------------------------------------------------------------------
// Static expression evaluation — folds the constant subset (used for styles + state initials). Throws
// on anything dynamic (e.g., a state reference), which the caller catches to fall back to C emission.
// ---------------------------------------------------------------------------------------------------
function evalStatic(node, scope) {
  switch (node.type) {
    case 'NumericLiteral':
    case 'StringLiteral':
    case 'BooleanLiteral':
      return node.value;
    case 'NullLiteral':
      return null;
    case 'UnaryExpression': {
      const a = evalStatic(node.argument, scope);
      if (node.operator === '-') return -a;
      if (node.operator === '+') return +a;
      if (node.operator === '!') return !a;
      break;
    }
    case 'BinaryExpression': {
      const l = evalStatic(node.left, scope);
      const r = evalStatic(node.right, scope);
      switch (node.operator) {
        case '+': return l + r;
        case '-': return l - r;
        case '*': return l * r;
        case '/': return l / r;
        case '%': return l % r;
        case '<': return l < r;
        case '>': return l > r;
        case '<=': return l <= r;
        case '>=': return l >= r;
        case '==':
        case '===': return l === r;
        case '!=':
        case '!==': return l !== r;
      }
      break;
    }
    case 'LogicalExpression': {
      const l = evalStatic(node.left, scope);
      if (node.operator === '&&') return l ? evalStatic(node.right, scope) : l;
      if (node.operator === '||') return l ? l : evalStatic(node.right, scope);
      break;
    }
    case 'ConditionalExpression':
      return evalStatic(node.test, scope) ? evalStatic(node.consequent, scope) : evalStatic(node.alternate, scope);
    case 'Identifier':
      if (node.name in scope) return scope[node.name];
      throw new Error(`AOT: cannot statically resolve identifier "${node.name}"`);
    case 'MemberExpression': {
      const obj = evalStatic(node.object, scope);
      const key = node.computed ? evalStatic(node.property, scope) : node.property.name;
      if (obj == null) throw new Error(`AOT: member access on null/undefined ("${key}")`);
      return obj[key];
    }
    case 'ObjectExpression': {
      const o = {};
      for (const prop of node.properties) {
        if (prop.type !== 'ObjectProperty') throw new Error('AOT: object spreads/methods not supported in static objects');
        const k = prop.computed ? evalStatic(prop.key, scope) : prop.key.name ?? prop.key.value;
        o[k] = evalStatic(prop.value, scope);
      }
      return o;
    }
    case 'ArrayExpression':
      return node.elements.map((e) => (e ? evalStatic(e, scope) : null));
    case 'CallExpression': {
      const c = node.callee;
      if (c.type === 'MemberExpression' && c.object.name === 'StyleSheet' && c.property.name === 'create') {
        return evalStatic(node.arguments[0], scope);
      }
      throw new Error(`AOT: cannot statically evaluate call expression`);
    }
  }
  throw new Error(`AOT: unsupported expression "${node.type}" in static context`);
}

// ---------------------------------------------------------------------------------------------------
// C expression emission — lowers a JS expression to C, given the current state + local bindings. Each
// result carries a C type so callers pick the right printf specifier / assignment.
//   env = { state: Map(name→record), locals: Map(name→{code,cType}), consts: scope object }
// ---------------------------------------------------------------------------------------------------
const ARITH = new Set(['+', '-', '*', '/', '%']);
const COMPARE = new Set(['<', '>', '<=', '>=', '==', '!=', '===', '!==']);

function emitExprImpl(node, env) {
  switch (node.type) {
    case 'NumericLiteral':
      return Number.isInteger(node.value) ? { code: String(node.value), cType: 'int' } : { code: `${node.value}f`, cType: 'float' };
    case 'StringLiteral':
      return { code: cstr(node.value), cType: 'string' };
    case 'BooleanLiteral':
      return { code: node.value ? '1' : '0', cType: 'int' };
    case 'Identifier': {
      if (env.locals.has(node.name)) return env.locals.get(node.name);
      if (env.state.has(node.name)) {
        const s = env.state.get(node.name);
        if (s.kind === 'list') throw new Error(`AOT: a list state ("${node.name}") can only be used via .length or .map`);
        return { code: s.cMember, cType: s.cType };
      }
      if (node.name in env.consts) {
        const v = env.consts[node.name];
        if (typeof v === 'number') return Number.isInteger(v) ? { code: String(v), cType: 'int' } : { code: `${v}f`, cType: 'float' };
        if (typeof v === 'string') return { code: cstr(v), cType: 'string' };
      }
      throw new Error(`AOT: cannot resolve identifier "${node.name}" in a dynamic expression`);
    }
    case 'UnaryExpression': {
      const a = emitExpr(node.argument, env);
      // Parenthesize the operand so `-` on a negative literal emits `(-(-135))`, not `(--135)` (a decrement).
      if (node.operator === '-' || node.operator === '+' || node.operator === '!') return { code: `(${node.operator}(${a.code}))`, cType: node.operator === '!' ? 'int' : a.cType };
      throw new Error(`AOT: unsupported unary operator "${node.operator}"`);
    }
    case 'BinaryExpression': {
      const l = emitExpr(node.left, env);
      const r = emitExpr(node.right, env);
      if (ARITH.has(node.operator)) {
        const cType = l.cType === 'float' || r.cType === 'float' ? 'float' : 'int';
        return { code: `(${l.code} ${node.operator} ${r.code})`, cType };
      }
      if (COMPARE.has(node.operator)) {
        const eqOp = node.operator === '===' || node.operator === '==' ? '==' : node.operator === '!==' || node.operator === '!=' ? '!=' : null;
        // String (in)equality → strcmp; the generated C already includes <string.h>.
        if (eqOp && (l.cType === 'string' || r.cType === 'string')) return { code: `(strcmp(${l.code}, ${r.code}) ${eqOp} 0)`, cType: 'int' };
        const op = node.operator === '===' ? '==' : node.operator === '!==' ? '!=' : node.operator;
        return { code: `(${l.code} ${op} ${r.code})`, cType: 'int' };
      }
      throw new Error(`AOT: unsupported binary operator "${node.operator}"`);
    }
    case 'LogicalExpression': {
      const op = node.operator === '&&' || node.operator === '||' ? node.operator : null;
      if (!op) throw new Error(`AOT: unsupported logical operator "${node.operator}"`);
      const l = emitExpr(node.left, env);
      const r = emitExpr(node.right, env);
      return { code: `(${l.code} ${op} ${r.code})`, cType: 'int' };
    }
    case 'ConditionalExpression': {
      const t = emitExpr(node.test, env);
      const c = emitExpr(node.consequent, env);
      const a = emitExpr(node.alternate, env);
      const cType = c.cType === 'float' || a.cType === 'float' ? 'float' : c.cType === a.cType ? c.cType : 'int';
      return { code: `(${t.code} ? ${c.code} : ${a.code})`, cType };
    }
    case 'MemberExpression': {
      // Static fold: member access that resolves to a compile-time constant (e.g. a .map item's `.key`).
      try {
        const v = evalStatic(node, env.consts ?? {});
        if (typeof v === 'number') return Number.isInteger(v) ? { code: String(v), cType: 'int' } : { code: `${v}f`, cType: 'float' };
        if (typeof v === 'string') return { code: cstr(v), cType: 'string' };
        if (typeof v === 'boolean') return { code: v ? '1' : '0', cType: 'int' };
      } catch {
        /* not static — fall through to the dynamic member forms below */
      }
      const obj = node.object;
      const prop = node.computed ? null : node.property.name;
      // `<list>.length` → the runtime count.
      if (obj.type === 'Identifier' && env.state.get(obj.name)?.kind === 'list' && prop === 'length') {
        return { code: env.state.get(obj.name).countMember, cType: 'int' };
      }
      // `<item>.field` where item is a struct local (a list row's bound element).
      if (obj.type === 'Identifier' && env.locals.get(obj.name)?.struct && prop) {
        const f = env.locals.get(obj.name).struct.fields.find((x) => x.key === prop);
        if (!f) throw new Error(`AOT: unknown field "${prop}" on a list item`);
        return { code: `${env.locals.get(obj.name).code}.${f.key}`, cType: f.kind === 'string' ? 'string' : f.kind };
      }
      // `<ref>.current` — a value ref's mutable C slot.
      if (obj.type === 'Identifier' && env.refs?.has(obj.name) && prop === 'current') {
        const r = env.refs.get(obj.name);
        return { code: r.cVar, cType: r.cType };
      }
      // `<event>.x / .y / .dx / .dy` — touch fields of the handler's EREventData.
      if (obj.type === 'Identifier' && env.event === obj.name && (prop === 'x' || prop === 'y' || prop === 'dx' || prop === 'dy')) {
        return { code: `data->${prop}`, cType: 'int' };
      }
      // `<event>.layout.x / .y / .width / .height` — the onLayout rect (EREventData.layout_rect; ERRect uses w/h).
      if (obj.type === 'MemberExpression' && !obj.computed && obj.object.type === 'Identifier' && env.event === obj.object.name && obj.property.name === 'layout') {
        const RECT = { x: 'x', y: 'y', width: 'w', height: 'h' };
        const f = RECT[prop];
        if (!f) throw new Error(`AOT: unknown onLayout rect field "${prop}" (use x / y / width / height)`);
        return { code: `data->layout_rect.${f}`, cType: 'int' };
      }
      if (obj.type === 'Identifier' && obj.name === 'Math' && prop === 'PI') return { code: '(float)M_PI', cType: 'float' };
      throw aotError('AOT: unsupported member expression in a dynamic context', 'in a handler or dynamic expression you can read state, `ref.current`, a `.map` item field, event fields (e.x / e.y / e.dx / e.dy / e.layout.*), and Math.PI — other member access must be a compile-time constant.');
    }
    case 'CallExpression': {
      // Math.* helpers → libm (the generated C includes <math.h> when these appear).
      const c = node.callee;
      if (c.type === 'MemberExpression' && c.object.name === 'Math') {
        const fn = c.property.name;
        const a = node.arguments.map((x) => emitExpr(x, env));
        const UNARY = { sin: 'sinf', cos: 'cosf', tan: 'tanf', sqrt: 'sqrtf', abs: 'fabsf', round: 'roundf', floor: 'floorf', ceil: 'ceilf' };
        if (UNARY[fn] && a.length === 1) {
          const inner = `${UNARY[fn]}((float)(${a[0].code}))`;
          // round/floor/ceil yield a whole number — cast to int so %d / int assignments are correct.
          return fn === 'round' || fn === 'floor' || fn === 'ceil' ? { code: `((int)${inner})`, cType: 'int' } : { code: inner, cType: 'float' };
        }
        const BINARY = { min: 'fminf', max: 'fmaxf', atan2: 'atan2f', pow: 'powf' };
        if (BINARY[fn] && a.length === 2) return { code: `${BINARY[fn]}((float)(${a[0].code}), (float)(${a[1].code}))`, cType: 'float' };
        throw new Error(`AOT: unsupported Math.${fn}(...) (arity ${a.length})`);
      }
      throw new Error('AOT: unsupported call expression in a dynamic expression');
    }
  }
  throw new Error(`AOT: unsupported expression "${node.type}" in a dynamic context`);
}
const emitExpr = withLoc(emitExprImpl);

const printfSpec = (cType) => (cType === 'string' ? '%s' : cType === 'float' ? '%g' : '%d');
const cTypeOfValue = (v) => (typeof v === 'string' ? 'string' : typeof v === 'number' && !Number.isInteger(v) ? 'float' : 'int');

// ---------------------------------------------------------------------------------------------------
// AST helpers + collection passes — small predicates (isFn, fnReturnsJSX, …) and the up-front scans
// that walk the component body ONCE to gather what later emission needs: the module scope, useState,
// child components, and the useCallback / useMemo / useEffect / useRef hooks.
// ---------------------------------------------------------------------------------------------------
const isFn = (n) => n && (n.type === 'FunctionDeclaration' || n.type === 'FunctionExpression' || n.type === 'ArrowFunctionExpression');

function findComponent(program) {
  for (const stmt of program.body) {
    const d = stmt.type === 'ExportNamedDeclaration' ? stmt.declaration : stmt;
    if (!d) continue;
    if (d.type === 'FunctionDeclaration' && d.id?.name === 'App') return d;
    if (d.type === 'VariableDeclaration') for (const decl of d.declarations) if (decl.id?.name === 'App' && isFn(decl.init)) return decl.init;
  }
  throw new Error('AOT: no `App` component found (expected `export function App() { ... }`)');
}

// Target screen size, baked at compile time so the demo's responsive `screen.width`/`screen.height`
// branching folds to the layout for THIS build (each board compiles its own binary). Override per target,
// e.g. ER_AOT_SCREEN_W=240 ER_AOT_SCREEN_H=320 for the CYD; defaults to a wide 800×480.
const SCREEN_W = Number(process.env.ER_AOT_SCREEN_W) || 800;
const SCREEN_H = Number(process.env.ER_AOT_SCREEN_H) || 480;

function moduleScope(program, screen) {
  const scope = { screen };
  for (const stmt of program.body) {
    const d = stmt.type === 'ExportNamedDeclaration' ? stmt.declaration : stmt;
    if (d?.type !== 'VariableDeclaration') continue;
    for (const decl of d.declarations) {
      if (!decl.id || decl.id.type !== 'Identifier' || !decl.init || isFn(decl.init)) continue;
      try {
        scope[decl.id.name] = evalStatic(decl.init, scope);
      } catch {
        /* not a static const — skip */
      }
    }
  }
  return scope;
}

/** Collects useState declarations → state descriptors keyed by both state name and setter name. */
/** Max characters stored per string field of a list-state item (fixed buffer — embedded-friendly). */
const LIST_STR_CAP = Number(process.env.ER_AOT_LIST_STR_CAP) || 48;
/** Max rows a list-state can hold (pre-allocated pool; rows beyond the count are display:none).
 *  Override with ER_AOT_LIST_CAP — lower it on a tight-RAM MCU (each pooled row costs engine nodes). */
const LIST_CAP = Number(process.env.ER_AOT_LIST_CAP) || 16;
/** Max inline segments in a nested-<Text>. Must match the engine's ER_TEXT_MAX_SPANS (default 4); if a
 *  project raises that #define, set ER_AOT_MAX_TEXT_SPANS to the same value when generating. */
const AOT_MAX_TEXT_SPANS = Number(process.env.ER_AOT_MAX_TEXT_SPANS) || 4;

/** Infers a C struct shape from a list-state's initial elements (objects of strings/numbers). */
function inferItemStruct(items, name) {
  const shapeHint =
    'a list state is a fixed-shape struct array: each element must be an OBJECT with the same string/number fields, ' +
    'e.g. useState([{ title: "A", n: 1 }, { title: "B", n: 2 }]). The first element defines the columns.';
  if (!Array.isArray(items) || !items.length)
    throw aotError(`AOT: list state "${name}" needs ≥1 initial element to infer its item shape`, shapeHint);
  const first = items[0];
  if (typeof first !== 'object' || first === null || Array.isArray(first))
    throw aotError(`AOT: list state "${name}" elements must be objects`, shapeHint);
  const fields = Object.keys(first).map((key) => {
    const v = first[key];
    if (typeof v === 'string') return { key, kind: 'string' };
    if (typeof v === 'number') return { key, kind: Number.isInteger(v) ? 'int' : 'float' };
    throw aotError(`AOT: list state "${name}" field "${key}" must be a string or number`, shapeHint);
  });
  return { fields };
}

function collectState(fnBody, scope) {
  const byName = new Map();
  const bySetter = new Map();
  for (const stmt of fnBody.body) {
    if (stmt.type !== 'VariableDeclaration') continue;
    for (const decl of stmt.declarations) {
      const init = decl.init;
      if (init?.type !== 'CallExpression' || init.callee.name !== 'useState' || decl.id.type !== 'ArrayPattern') continue;
      const name = decl.id.elements[0]?.name;
      const setter = decl.id.elements[1]?.name;
      if (!name) continue;
      const initArg = init.arguments[0];

      let rec;
      if (initArg?.type === 'ArrayExpression') {
        // List state → a fixed-capacity C struct array + a count (s_<name>[CAP], s_<name>_count).
        const items = evalStaticOrThrow(
          initArg,
          scope,
          `AOT: the initial value of list state "${name}" must be a compile-time constant array`,
          'useState([...]) initial must be a literal array of objects/numbers/strings — no runtime values or function calls in the initial.',
        );
        let struct;
        try {
          struct = inferItemStruct(items, name);
        } catch (e) {
          if (initArg.loc && !e.aotLoc) e.aotLoc = initArg.loc.start; // collectState isn't withLoc-wrapped
          throw e;
        }
        rec = { name, setter, kind: 'list', struct, items, cap: LIST_CAP, cTypeName: `ErItem_${name}`, arrayName: `s_${name}`, countMember: `s_${name}_count` };
      } else {
        const initVal = initArg
          ? evalStaticOrThrow(
              initArg,
              scope,
              `AOT: the initial value of state "${name}" must be a compile-time constant`,
              'useState(x) initial must be a literal or a constant expression (number, string, bool, or arithmetic over consts) — not a runtime value or function call.',
            )
          : 0;
        let cType = cTypeOfValue(initVal);
        // A numeric literal written with a decimal point or exponent (e.g. useState(70.0)) forces a FLOAT
        // slot even though the value is integral — lets the state hold sub-integer values (a smooth drag)
        // while the UI shows Math.round(value). (70.0 === 70 in JS, so we read the raw source to tell them apart.)
        if (cType === 'int' && initArg?.type === 'NumericLiteral' && typeof initArg.extra?.raw === 'string' && /[.eE]/.test(initArg.extra.raw)) {
          cType = 'float';
        }
        // String scalar → a fixed char buffer in ErAppState; setters snprintf into it (see scalarAssign).
        const initCode = cType === 'string' ? cstr(String(initVal)) : cType === 'float' ? floatLit(initVal) : String(Number(initVal));
        rec = { name, setter, kind: 'scalar', cType, cMember: `s_state.${name}`, initCode };
      }
      byName.set(name, rec);
      if (setter) bySetter.set(setter, rec);
    }
  }
  return { byName, bySetter };
}

function findReturnJSX(fnBody, scope = {}) {
  // Fold top-level `if (staticCond) return …` at compile time — responsive layouts switch on `screen`.
  const scan = (stmts) => {
    for (const stmt of stmts) {
      if (stmt.type === 'IfStatement') {
        let test;
        try {
          test = evalStatic(stmt.test, scope);
        } catch {
          throw new Error('AOT: a top-level `if` in the component must have a compile-time-constant test (e.g. on the `screen` global) — runtime layout branching is not supported');
        }
        const branch = test ? stmt.consequent : stmt.alternate;
        if (branch) {
          const r = scan(branch.type === 'BlockStatement' ? branch.body : [branch]);
          if (r) return r;
        }
        continue;
      }
      if (stmt.type === 'ReturnStatement' && stmt.argument) {
        if (stmt.argument.type === 'JSXElement') return stmt.argument;
        throw new Error(`AOT: the component must return a single JSX element (got ${stmt.argument.type})`);
      }
    }
    return null;
  };
  const r = scan(fnBody.body);
  if (!r) throw new Error('AOT: component has no return statement');
  return r;
}

/** Returns the JSX a function component returns (arrow expression body or a block's return). */
function componentReturnJSX(fn, scope = {}) {
  if (fn.body.type === 'JSXElement') return fn.body;
  if (fn.body.type === 'BlockStatement') return findReturnJSX(fn.body, scope);
  throw new Error('AOT: component body must return a JSX element');
}

const fnReturnsJSX = (fn) => fn.body.type === 'JSXElement' || (fn.body.type === 'BlockStatement' && fn.body.body.some((s) => s.type === 'ReturnStatement' && s.argument?.type === 'JSXElement'));

/** Collects top-level function components (name → fn node), excluding the `App` entry component. */
/** Resolves a component definition expression to its function node, unwrapping memo(fn) / React.memo(fn). */
function asComponentFn(node) {
  if (isFn(node)) return node;
  if (node?.type === 'CallExpression' && isFn(node.arguments[0]) && (node.callee.name === 'memo' || (node.callee.type === 'MemberExpression' && node.callee.property?.name === 'memo'))) return node.arguments[0];
  return null;
}

function collectComponents(program) {
  const comps = new Map();
  for (const stmt of program.body) {
    const d = stmt.type === 'ExportNamedDeclaration' ? stmt.declaration : stmt;
    if (!d) continue;
    if (d.type === 'FunctionDeclaration' && d.id && d.id.name !== 'App' && fnReturnsJSX(d)) comps.set(d.id.name, d);
    if (d.type === 'VariableDeclaration')
      for (const decl of d.declarations) {
        if (decl.id?.type !== 'Identifier' || decl.id.name === 'App') continue;
        const fn = asComponentFn(decl.init); // unwrap memo(...)
        if (fn && fnReturnsJSX(fn)) comps.set(decl.id.name, fn);
      }
  }
  return comps;
}

const IMAGE_EXT_RE = /\.(png|jpe?g|webp|gif|bmp)$/i;

/**
 * Collects image imports — `import wxSun from './assets/wx_sun.png'` → Map(local → { name, importPath }).
 * `name` is the file's basename without extension (the asset key `<Image source>` resolves to and that the
 * Flow A bundler also uses); `importPath` is the source-relative path the CLI bakes from. Mirrors the Flow A
 * esbuild asset plugin so the same `import → basename` convention holds in both flows.
 */
function collectImageImports(program) {
  const byLocal = new Map();
  for (const stmt of program.body) {
    if (stmt.type !== 'ImportDeclaration' || typeof stmt.source.value !== 'string') continue;
    const importPath = stmt.source.value;
    if (!IMAGE_EXT_RE.test(importPath)) continue;
    const name = importPath.split(/[\\/]/).pop().replace(IMAGE_EXT_RE, '');
    for (const spec of stmt.specifiers) {
      if (spec.type === 'ImportDefaultSpecifier') byLocal.set(spec.local.name, { name, importPath });
    }
  }
  return byLocal;
}

/** Collects `const fn = useCallback((...) => {...}, deps)` → Map(name → arrow fn node). Deps are ignored:
 *  the AOT re-renders via its own dependency tracking, so useCallback only names a shared C handler. */
function collectCallbacks(fnBody) {
  const cbs = new Map();
  if (fnBody.type !== 'BlockStatement') return cbs;
  for (const stmt of fnBody.body) {
    if (stmt.type !== 'VariableDeclaration') continue;
    for (const decl of stmt.declarations) {
      const init = decl.init;
      if (init?.type === 'CallExpression' && init.callee.name === 'useCallback' && decl.id.type === 'Identifier' && isFn(init.arguments[0])) {
        cbs.set(decl.id.name, init.arguments[0]);
      }
    }
  }
  return cbs;
}

/** Collects `const m = useMemo(() => expr, deps)` → Map(name → the memo's expression node). */
function collectMemos(fnBody) {
  const memos = new Map();
  if (fnBody.type !== 'BlockStatement') return memos;
  for (const stmt of fnBody.body) {
    if (stmt.type !== 'VariableDeclaration') continue;
    for (const decl of stmt.declarations) {
      const init = decl.init;
      if (init?.type === 'CallExpression' && init.callee.name === 'useMemo' && decl.id.type === 'Identifier' && isFn(init.arguments[0])) {
        const body = init.arguments[0].body;
        if (body.type === 'BlockStatement') throw new Error(`AOT: useMemo for "${decl.id.name}" must be a single expression (for now)`);
        memos.set(decl.id.name, body);
      }
    }
  }
  return memos;
}

/** Collects `useEffect(() => {…}, deps)` calls → { fn, deps, node }. Deps validity is checked at compile. */
function collectEffects(fnBody) {
  const effects = [];
  if (fnBody.type !== 'BlockStatement') return effects;
  for (const stmt of fnBody.body) {
    if (stmt.type !== 'ExpressionStatement') continue;
    const call = stmt.expression;
    if (call?.type === 'CallExpression' && call.callee.type === 'Identifier' && call.callee.name === 'useEffect') {
      if (!isFn(call.arguments[0])) throw aotError('AOT: useEffect must take an inline function', 'write useEffect(() => { … }, []).');
      effects.push({ fn: call.arguments[0], deps: call.arguments[1], node: call });
    }
  }
  return effects;
}

/** True if a function component declares any useState (per-instance child state — not yet supported). */
function usesState(fn) {
  if (fn.body.type !== 'BlockStatement') return false;
  return fn.body.body.some((s) => s.type === 'VariableDeclaration' && s.declarations.some((d) => d.init?.type === 'CallExpression' && d.init.callee.name === 'useState'));
}

// ---------------------------------------------------------------------------------------------------
// Animations — useAnimatedValue → an engine-side ERAnimValueHandle (native driver). The value binds to
// a node property (opacity / transform / color) via er_anim_value_bind, and Animated.timing/spring(...)
// .start() → er_anim_value_animate. The host's per-frame embedded_renderer_tick advances it in C — no
// per-frame JS, no app_update needed for the motion itself.
// ---------------------------------------------------------------------------------------------------

/** Collects `const x = useAnimatedValue(initial)` → Map(name → {cVar, initCode}). */
function collectAnims(fnBody, scope) {
  const anims = new Map();
  if (fnBody.type !== 'BlockStatement') return anims;
  for (const stmt of fnBody.body) {
    if (stmt.type !== 'VariableDeclaration') continue;
    for (const decl of stmt.declarations) {
      const init = decl.init;
      if (init?.type === 'CallExpression' && init.callee.name === 'useAnimatedValue' && decl.id.type === 'Identifier') {
        const initVal = init.arguments[0] ? evalStatic(init.arguments[0], scope) : 0;
        anims.set(decl.id.name, { cVar: `s_av_${decl.id.name}`, initCode: floatLit(initVal) });
      }
    }
  }
  return anims;
}

/**
 * Collects `const r = useRef(initial)` refs → Map(name → {cVar, cType, initCode, kind}). Two kinds:
 *  - VALUE ref (numeric initial): a mutable C slot (escape-hatch state that does NOT re-render;
 *    `.current` reads/writes).
 *  - NODE ref (`useRef()` / `useRef(null)`): holds an `ERNode*`, captured by `ref={r}` on an element and
 *    used as the target of imperative calls like updateVector(r, …). kind === 'node'.
 */
function collectRefs(fnBody, scope) {
  const refs = new Map();
  if (fnBody.type !== 'BlockStatement') return refs;
  for (const stmt of fnBody.body) {
    if (stmt.type !== 'VariableDeclaration') continue;
    for (const decl of stmt.declarations) {
      const init = decl.init;
      if (init?.type === 'CallExpression' && init.callee.name === 'useRef' && decl.id.type === 'Identifier') {
        const cVar = `s_ref_${decl.id.name}`;
        const arg = init.arguments[0];
        if (!arg || (arg.type === 'NullLiteral')) {
          refs.set(decl.id.name, { cVar, cType: 'ERNode*', initCode: 'NULL', kind: 'node' });
          continue;
        }
        const v = evalStatic(arg, scope);
        if (typeof v !== 'number') throw new Error(`AOT: useRef initial for "${decl.id.name}" must be a number (value ref) or null/empty (node ref)`);
        const cType = Number.isInteger(v) ? 'int' : 'float';
        refs.set(decl.id.name, { cVar, cType, initCode: cType === 'float' ? `${v}f` : String(v), kind: 'value' });
      }
    }
  }
  return refs;
}

/** Resolves a JSX tag to a name, mapping `Animated.View/Text/Image` to their host element. */
function resolveTag(openingElement) {
  const n = openingElement.name;
  if (n.type === 'JSXIdentifier') return n.name;
  if (n.type === 'JSXMemberExpression' && n.object.name === 'Animated') return n.property.name; // Animated.View → View
  throw new Error('AOT: unsupported JSX tag expression');
}

/** Style key → the ERAnimProp(s) an animated value binds to. */
const ANIM_STYLE_PROPS = { opacity: ['ER_PROP_OPACITY'], backgroundColor: ['ER_PROP_BACKGROUND_COLOR'], color: ['ER_PROP_COLOR'] };
const ANIM_TRANSFORM_PROPS = {
  scale: ['ER_PROP_SCALE_X', 'ER_PROP_SCALE_Y'],
  scaleX: ['ER_PROP_SCALE_X'],
  scaleY: ['ER_PROP_SCALE_Y'],
  translateX: ['ER_PROP_TRANSLATE_X'],
  translateY: ['ER_PROP_TRANSLATE_Y'],
  rotate: ['ER_PROP_ROTATE_Z'],
  rotateZ: ['ER_PROP_ROTATE_Z'],
};

/** Formats a number as a valid C float literal (`1` → `1.0f`, not `1f` which doesn't compile). */
function floatLit(n) {
  const v = Number(n);
  return Number.isInteger(v) ? `${v}.0f` : `${v}f`;
}

/** The polynomial family of an `Easing.quad` / `Easing.cubic` node, for in/out/inOut composition. */
function easingFamily(node) {
  if (node?.type === 'MemberExpression' && node.object?.name === 'Easing') {
    if (node.property.name === 'quad') return 'QUAD';
    if (node.property.name === 'cubic') return 'CUBIC';
  }
  return null;
}

/** Maps an `Easing.*` node → { ease: 'ER_EASE_*', bezier: [x1,y1,x2,y2] | null }. Handles the bare curves
 *  (linear/ease/quad/cubic/bounce/elastic), the in/out/inOut wrappers around quad/cubic, and
 *  Easing.bezier(x1,y1,x2,y2). No easing → ER_EASE_EASE_IN_OUT (RN's timing default); unknown → same. */
function easingInfo(node, env) {
  const FALLBACK = { ease: 'ER_EASE_EASE_IN_OUT', bezier: null };
  if (!node) return FALLBACK;
  // Bare member: Easing.linear / Easing.ease / Easing.quad (== quad-in) / ...
  if (node.type === 'MemberExpression' && node.object?.name === 'Easing') {
    const m = { linear: 'ER_EASE_LINEAR', ease: 'ER_EASE_EASE', quad: 'ER_EASE_QUAD_IN', cubic: 'ER_EASE_CUBIC_IN', bounce: 'ER_EASE_BOUNCE_OUT', elastic: 'ER_EASE_ELASTIC_OUT' };
    return { ease: m[node.property.name] || 'ER_EASE_EASE_IN_OUT', bezier: null };
  }
  // Call: Easing.bezier(...), Easing.elastic(n), Easing.in/out/inOut(inner)
  if (node.type === 'CallExpression' && node.callee.type === 'MemberExpression' && node.callee.object?.name === 'Easing') {
    const fn = node.callee.property.name;
    if (fn === 'bezier') {
      const cps = node.arguments.slice(0, 4).map((a) => Number(evalStaticOr(a, env, 0)));
      return cps.length === 4 ? { ease: 'ER_EASE_BEZIER', bezier: cps } : FALLBACK;
    }
    if (fn === 'elastic') return { ease: 'ER_EASE_ELASTIC_OUT', bezier: null };
    if (fn === 'in' || fn === 'out' || fn === 'inOut') {
      const fam = easingFamily(node.arguments[0]);
      const dir = fn === 'in' ? 'IN' : fn === 'out' ? 'OUT' : 'IN_OUT';
      return fam ? { ease: `ER_EASE_${fam}_${dir}`, bezier: null } : FALLBACK;
    }
  }
  return FALLBACK;
}

/** Pushes `cfg.easing = …;` (and bezier control points for Easing.bezier) onto a timing config's C lines. */
function pushEasing(lines, c, easingNode, env) {
  const { ease, bezier } = easingInfo(easingNode, env);
  lines.push(`        ${c}.easing = ${ease};`);
  if (bezier) {
    lines.push(`        ${c}.bezier_x1 = ${floatLit(bezier[0])}; ${c}.bezier_y1 = ${floatLit(bezier[1])};`);
    lines.push(`        ${c}.bezier_x2 = ${floatLit(bezier[2])}; ${c}.bezier_y2 = ${floatLit(bezier[3])};`);
  }
}

/** Parses a `.interpolate({ inputRange, outputRange, extrapolate })` config object → a static
 *  { input, output, exLeft, exRight } descriptor (ranges must be static, equal-length, 2..8 points). */
function parseInterp(cfgNode, env) {
  if (cfgNode?.type !== 'ObjectExpression') throw aotError('AOT: .interpolate() needs a config object literal { inputRange, outputRange }');
  const get = (k) => cfgNode.properties.find((p) => (p.key.name ?? p.key.value) === k)?.value;
  const arr = (node, name) => {
    if (node?.type !== 'ArrayExpression') throw aotError(`AOT: .interpolate() ${name} must be an array literal`);
    return node.elements.map((e) => Number(evalStatic(e, env.consts ?? {})));
  };
  const input = arr(get('inputRange'), 'inputRange');
  const output = arr(get('outputRange'), 'outputRange');
  if (input.length < 2 || input.length !== output.length) throw aotError('AOT: .interpolate() inputRange and outputRange must be the same length (>= 2)');
  if (input.length > 8) throw aotError('AOT: .interpolate() supports up to 8 breakpoints (ER_INTERPOLATE_MAX_POINTS)');
  const ex = (node) => {
    const v = node ? String(evalStaticOr(node, env, 'extend')) : 'extend';
    return v === 'clamp' ? 'ER_EXTRAPOLATE_CLAMP' : v === 'identity' ? 'ER_EXTRAPOLATE_IDENTITY' : 'ER_EXTRAPOLATE_EXTEND';
  };
  const both = get('extrapolate'); // RN: `extrapolate` sets both ends; extrapolateLeft/Right override.
  return { input, output, exLeft: ex(get('extrapolateLeft') ?? both), exRight: ex(get('extrapolateRight') ?? both) };
}

// ---------------------------------------------------------------------------------------------------
// JSX → style / text / events
// ---------------------------------------------------------------------------------------------------
function attrExpr(attr) {
  const v = attr.value;
  if (!v) return { type: 'BooleanLiteral', value: true };
  if (v.type === 'StringLiteral') return v;
  if (v.type === 'JSXExpressionContainer') return v.expression;
  return v;
}

/** Lowers a dynamic (state-referencing) color expression to a C ARGB expression. */
function emitColorExpr(node, env) {
  if (node.type === 'StringLiteral') return colorLiteral(node.value);
  if (node.type === 'ConditionalExpression') {
    const t = emitExpr(node.test, env).code;
    return `((${t}) ? ${emitColorExpr(node.consequent, env)} : ${emitColorExpr(node.alternate, env)})`;
  }
  // A statically-resolvable color (a const string, or a theme token like `theme.card`) folds to a literal.
  try {
    const s = evalStatic(node, env.consts ?? {});
    if (typeof s === 'string') return colorLiteral(s);
  } catch {
    /* not static — fall through to the error below */
  }
  throw new Error('AOT: a dynamic color must be a color string literal or a ternary of them');
}

/** Lowers a dynamic enum-style expression (e.g. `flexDirection: row ? 'row' : 'column'`) to its ER_* constant
 *  (or a C ternary of them), looking values up in the style key's enum `table`. */
function emitEnumExpr(node, table, env) {
  if (node.type === 'StringLiteral') {
    const c = table[node.value];
    if (!c) throw aotError(`AOT: unsupported enum value "${node.value}"`, `one of: ${Object.keys(table).join(', ')}`);
    return c;
  }
  if (node.type === 'ConditionalExpression') {
    const t = emitExpr(node.test, env).code;
    return `((${t}) ? ${emitEnumExpr(node.consequent, table, env)} : ${emitEnumExpr(node.alternate, table, env)})`;
  }
  // A statically resolvable enum (a const string) folds to its constant.
  try {
    const s = evalStatic(node, env.consts ?? {});
    if (typeof s === 'string' && table[s]) return table[s];
  } catch {
    /* not static — fall through */
  }
  throw aotError('AOT: a state-driven enum style must be a string literal or a ternary of them', "e.g. flexDirection: wide ? 'row' : 'column'");
}

/** Lowers one dynamic inline-style value to ERProps field assignment(s) (C expressions). */
function lowerDynamicStyleValue(key, valueNode, env) {
  const meta = DYN_FIELDS[key];
  if (!meta) throw aotError(`AOT: a state-driven value for style "${key}" is not supported (static only)`, `state-driven styles supported: colors, opacity, sizes/margins/padding, and the layout enums (flexDirection, alignItems, alignSelf, justifyContent, position). Make "${key}" static, or drive the change another way.`);
  if (meta.kind === 'color') return [{ field: meta.field, code: emitColorExpr(valueNode, env) }];
  if (meta.kind === 'opacity') return [{ field: meta.field, code: `(uint8_t)((${emitExpr(valueNode, env).code}) * 255.0f)` }];
  if (meta.kind === 'enum') return [{ field: meta.field, code: emitEnumExpr(valueNode, meta.table, env) }];
  return [{ field: meta.field, code: `(int16_t)(${emitExpr(valueNode, env).code})` }]; /* num */
}

/**
 * Collects an element's merged style into static field assigns and dynamic (state-driven) field assigns.
 * Inline object values are tried statically first; a value that references state becomes a dynAssign.
 * Later style sources override earlier ones per field (RN merge), kept in `fields` by ERProps field.
 */
function collectStyleAssigns(openingElement, scope, env) {
  const fields = new Map(); // ERProps field -> { dynamic: bool, code: string }
  const binds = []; // [{ cVar, prop, interp? }] — animated values bound to node properties (native driver)
  const animRef = (node) => (node?.type === 'Identifier' && env.anims?.has(node.name) ? env.anims.get(node.name).cVar : null);
  // `<animValue>.interpolate({ inputRange, outputRange, extrapolate })` → { cVar, interp } for a mapped bind.
  const animInterpRef = (node) => {
    if (node?.type === 'CallExpression' && node.callee.type === 'MemberExpression' && !node.callee.computed && node.callee.property.name === 'interpolate' && node.callee.object.type === 'Identifier' && env.anims?.has(node.callee.object.name)) {
      return { cVar: env.anims.get(node.callee.object.name).cVar, interp: parseInterp(node.arguments[0], env) };
    }
    return null;
  };
  const apply = (expr) => {
    if (expr.type === 'ArrayExpression') {
      for (const e of expr.elements) if (e) apply(e);
      return;
    }
    if (expr.type === 'ObjectExpression') {
      for (const prop of expr.properties) {
        if (prop.type !== 'ObjectProperty') throw new Error('AOT: spread/method in an inline style object not supported');
        const key = prop.computed ? evalStatic(prop.key, scope) : prop.key.name ?? prop.key.value;

        // Animated value bound directly to a prop (opacity / backgroundColor / color), optionally through
        // an .interpolate({ inputRange, outputRange }) mapping.
        const av = animRef(prop.value);
        if (av && ANIM_STYLE_PROPS[key]) {
          for (const p of ANIM_STYLE_PROPS[key]) binds.push({ cVar: av, prop: p });
          continue;
        }
        const ai = animInterpRef(prop.value);
        if (ai && ANIM_STYLE_PROPS[key]) {
          for (const p of ANIM_STYLE_PROPS[key]) binds.push({ cVar: ai.cVar, prop: p, interp: ai.interp });
          continue;
        }
        // transform: [{ scale: <anim> }, { translateX: <anim>.interpolate(...) }, ...] — bind each entry.
        if (key === 'transform' && prop.value.type === 'ArrayExpression') {
          let handled = false;
          for (const entry of prop.value.elements) {
            if (entry?.type !== 'ObjectExpression') continue;
            for (const tp of entry.properties) {
              const tk = tp.key.name ?? tp.key.value;
              const tav = animRef(tp.value);
              if (tav && ANIM_TRANSFORM_PROPS[tk]) {
                for (const p of ANIM_TRANSFORM_PROPS[tk]) binds.push({ cVar: tav, prop: p });
                handled = true;
                continue;
              }
              const tai = animInterpRef(tp.value);
              if (tai && ANIM_TRANSFORM_PROPS[tk]) {
                for (const p of ANIM_TRANSFORM_PROPS[tk]) binds.push({ cVar: tai.cVar, prop: p, interp: tai.interp });
                handled = true;
              }
            }
          }
          if (handled) continue;
        }

        try {
          for (const a of lowerStyle({ [key]: evalStatic(prop.value, scope) })) fields.set(a.field, { dynamic: false, code: a.expr });
        } catch {
          for (const a of lowerDynamicStyleValue(key, prop.value, env)) fields.set(a.field, { dynamic: true, code: a.code });
        }
      }
      return;
    }
    // A StyleSheet reference / identifier resolving to a static style object.
    for (const a of lowerStyle(evalStatic(expr, scope))) fields.set(a.field, { dynamic: false, code: a.expr });
  };
  for (const attr of openingElement.attributes) {
    if (attr.type !== 'JSXAttribute' || attr.name.name !== 'style') continue;
    apply(attrExpr(attr));
  }
  const staticAssigns = [];
  const dynAssigns = [];
  for (const [field, v] of fields) (v.dynamic ? dynAssigns : staticAssigns).push(v.dynamic ? { field, code: v.code } : { field, expr: v.code });
  return { staticAssigns, dynAssigns, binds };
}

const EVENT_TYPES = {
  onPress: 'ER_EVENT_PRESS',
  onLongPress: 'ER_EVENT_LONG_PRESS',
  onPressIn: 'ER_EVENT_PRESS_IN',
  onPressOut: 'ER_EVENT_PRESS_OUT',
  onTouchStart: 'ER_EVENT_TOUCH_START',
  onTouchMove: 'ER_EVENT_TOUCH_MOVE',
  onTouchEnd: 'ER_EVENT_TOUCH_END',
  onLayout: 'ER_EVENT_LAYOUT',
};

const cstr = (s) => `"${s.replace(/\\/g, '\\\\').replace(/"/g, '\\"').replace(/\n/g, '\\n').replace(/\t/g, '\\t')}"`;

/**
 * Builds a Text node's content. Static interpolations fold into the literal; any that reference state
 * make it dynamic (a printf format + C arg expressions recomputed on update).
 */
function buildText(children, scope, env) {
  let format = '';
  const args = [];
  let dynamic = false;
  for (const child of children) {
    if (child.type === 'JSXText') {
      const t = /\n/.test(child.value) ? child.value.replace(/\s+/g, ' ').trim() : child.value;
      format += t.replace(/%/g, '%%');
    } else if (child.type === 'JSXExpressionContainer') {
      if (child.expression.type === 'JSXEmptyExpression') continue;
      try {
        const v = evalStatic(child.expression, scope); // constant → fold in
        format += (v === undefined || v === null ? '' : String(v)).replace(/%/g, '%%');
      } catch {
        const e = emitExpr(child.expression, env); // references state → dynamic
        format += printfSpec(e.cType);
        args.push(e.code);
        dynamic = true;
      }
    } else if (child.type === 'JSXElement') {
      throw new Error('AOT: nested <Text> / element children inside <Text> not yet supported (spans)');
    }
  }
  return { dynamic, format, args };
}

/** Normalises JSX text the way Babel does: trim per-line, drop blank lines, join with single spaces; a
 *  same-line leading/trailing space is preserved (so `Hello <b>x</b>` keeps the space before the span). */
function cleanJsxText(value) {
  const lines = value.split(/\r\n|\n|\r/);
  let last = 0;
  for (let i = 0; i < lines.length; i++) if (/[^ \t]/.test(lines[i])) last = i;
  let out = '';
  for (let i = 0; i < lines.length; i++) {
    let line = lines[i].replace(/\t/g, ' ');
    if (i !== 0) line = line.replace(/^ +/, '');
    if (i !== lines.length - 1) line = line.replace(/ +$/, '');
    if (line) out += i !== last ? line + ' ' : line;
  }
  return out;
}

/** Concatenates a (span) <Text>'s static text content (literal + folded {expr}); a nested element throws. */
function staticTextContent(children, scope) {
  let s = '';
  for (const c of children) {
    if (c.type === 'JSXText') s += cleanJsxText(c.value);
    else if (c.type === 'JSXExpressionContainer' && c.expression.type !== 'JSXEmptyExpression') {
      const v = evalStatic(c.expression, scope); // throws if it references state
      if (v !== undefined && v !== null) s += String(v);
    } else if (c.type === 'JSXElement') throw aotError('AOT: a nested <Text> span may not itself contain another <Text> (one level of spans only)');
  }
  return s;
}

/**
 * If a <Text>'s children include a nested <Text>, returns inline SPANS [{text, color, font_size,
 * font_weight, font_style, text_decoration, letter_spacing}] (C-expr fields; inherit sentinels for unset).
 * Returns null when there's no nested <Text> (caller uses the single-string buildText path). Static only —
 * a dynamic {…} segment or a state-driven span style throws.
 */
function collectTextSpans(children, scope, env) {
  if (!children.some((c) => c.type === 'JSXElement' && c.openingElement.name.name === 'Text')) return null;
  // Inherit sentinels (see ERTextSpan doc): color 0, font_size 0, weight/style/decoration 0xFF, spacing AUTO.
  const inheritSpan = (text) => ({ text, color: '0u', font_size: '0', font_weight: '0xFF', font_style: '0xFF', text_decoration: '0xFF', letter_spacing: 'ER_LAYOUT_AUTO' });
  const spans = [];
  for (const c of children) {
    if (c.type === 'JSXText') {
      const t = cleanJsxText(c.value);
      if (t) spans.push(inheritSpan(cstr(t)));
    } else if (c.type === 'JSXExpressionContainer') {
      if (c.expression.type === 'JSXEmptyExpression') continue;
      let v;
      try {
        v = evalStatic(c.expression, scope);
      } catch {
        throw aotError('AOT: a dynamic {…} segment inside a multi-span <Text> is not supported', 'spans must be static; keep dynamic text in its own single <Text> (no nested <Text> siblings).');
      }
      if (v !== undefined && v !== null) spans.push(inheritSpan(cstr(String(v))));
    } else if (c.type === 'JSXElement' && c.openingElement.name.name === 'Text') {
      const { staticAssigns, dynAssigns } = collectStyleAssigns(c.openingElement, scope, env);
      if (dynAssigns.length) throw aotError('AOT: a state-driven style on a nested <Text> span is not supported', 'give the span <Text> a static style.');
      const field = (f, dflt) => staticAssigns.find((a) => a.field === f)?.expr ?? dflt;
      spans.push({
        text: cstr(staticTextContent(c.children, scope)),
        color: field('color', '0u'),
        font_size: field('font_size', '0'),
        font_weight: field('font_weight', '0xFF'),
        font_style: field('font_style', '0xFF'),
        text_decoration: field('text_decoration', '0xFF'),
        letter_spacing: field('letter_spacing', 'ER_LAYOUT_AUTO'),
      });
    } else throw aotError('AOT: unsupported child inside a multi-span <Text>', 'a <Text> with a nested <Text> may contain text, {static expressions}, and nested <Text> only.');
  }
  // The engine renders at most ER_TEXT_MAX_SPANS segments; refuse to silently drop the rest.
  if (spans.length > AOT_MAX_TEXT_SPANS) {
    throw aotError(`AOT: a <Text> has ${spans.length} inline segments but the engine renders at most ${AOT_MAX_TEXT_SPANS}`, `combine adjacent plain-text segments, or end the sentence right after a styled <Text> (e.g. "A <b>B</b> C <b>D</b>" is 4). If your engine build raised ER_TEXT_MAX_SPANS, set ER_AOT_MAX_TEXT_SPANS to match when running the AOT.`);
  }
  return spans;
}

// ---------------------------------------------------------------------------------------------------
// Handler compilation — an on* arrow/function → C statements that mutate state and re-render.
// ---------------------------------------------------------------------------------------------------
/** Compiles a list-state setter call (`setItems(...)`) to bounded C array mutations. */
function compileListOp(rec, arg, env) {
  const { arrayName: arr, countMember: cnt, cap, struct } = rec;
  // setItems([...items, a, b]) — append; setItems([]) — clear.
  if (arg.type === 'ArrayExpression') {
    if (arg.elements.length === 0) return [`    ${cnt} = 0;`];
    const [head, ...rest] = arg.elements;
    if (head?.type !== 'SpreadElement' || head.argument.name !== rec.name) throw new Error(`AOT: a list literal must spread the current list first: [...${rec.name}, item]`);
    const lines = [];
    for (const el of rest) {
      if (el.type !== 'ObjectExpression') throw new Error('AOT: appended list items must be object literals');
      const props = new Map(el.properties.map((p) => [p.key.name ?? p.key.value, p.value]));
      lines.push(`    if (${cnt} < ${cap})`, '    {');
      for (const f of struct.fields) {
        const valNode = props.get(f.key);
        if (!valNode) continue;
        const e = emitExpr(valNode, env);
        if (f.kind === 'string') lines.push(`        snprintf(${arr}[${cnt}].${f.key}, sizeof(${arr}[${cnt}].${f.key}), "${printfSpec(e.cType)}", ${e.code});`);
        else lines.push(`        ${arr}[${cnt}].${f.key} = ${e.code};`);
      }
      lines.push(`        ${cnt}++;`, '    }');
    }
    return lines;
  }
  // setItems(items.slice(0, X)) — slice(0,-1) pops the last; slice(0,n) truncates to n.
  if (arg.type === 'CallExpression' && arg.callee.type === 'MemberExpression' && arg.callee.object.name === rec.name && arg.callee.property.name === 'slice') {
    const end = arg.arguments[1];
    if (end?.type === 'UnaryExpression' && end.operator === '-' && end.argument.value === 1) return [`    if (${cnt} > 0) ${cnt}--;`];
    const e = emitExpr(end, env);
    return [`    ${cnt} = (${cnt} < (${e.code})) ? ${cnt} : (${e.code});`];
  }
  throw new Error(`AOT: unsupported list operation on "${rec.name}" (use [...${rec.name}, item], ${rec.name}.slice(0, -1), or [])`);
}

/** Builds a `get(key)` accessor over an `Animated.*(value, config)` call's config object literal. */
function animConfigGetter(cfgObj) {
  return (k) => cfgObj?.properties?.find((p) => (p.key.name ?? p.key.value) === k)?.value;
}

/** Emits one atomic animation (timing/spring/decay) → a scoped ERAnimConfig + er_anim_value_animate.
 *  `delayMs` is the absolute start delay (how composition offsets are realised); `loop` repeats a timing;
 *  `onCompleteCb` (optional) is a C function name set as cfg.on_complete — used to chain sequence steps. */
function emitAnimEntry(entry, env, idx, onCompleteCb) {
  const { cVar, kind, get, delayMs, loop } = entry;
  const c = `cfg${idx}`;
  const lines = ['    {', `        ERAnimConfig ${c};`, `        memset(&${c}, 0, sizeof(${c}));`];
  if (kind === 'spring') {
    lines.push(`        ${c}.type = ER_ANIM_SPRING;`);
    lines.push(`        ${c}.stiffness = ${floatLit(evalStaticOr(get('stiffness'), env, 200))};`);
    lines.push(`        ${c}.damping = ${floatLit(evalStaticOr(get('damping'), env, 18))};`);
    lines.push(`        ${c}.mass = ${floatLit(evalStaticOr(get('mass'), env, 1))};`);
  } else if (kind === 'decay') {
    lines.push(`        ${c}.type = ER_ANIM_DECAY;`);
    lines.push(`        ${c}.deceleration = ${floatLit(evalStaticOr(get('deceleration'), env, 0.998))};`);
    lines.push(`        ${c}.velocity = ${floatLit(evalStaticOr(get('velocity'), env, 0))};`);
  } else {
    lines.push(`        ${c}.type = ER_ANIM_TIMING;`);
    lines.push(`        ${c}.duration_ms = ${Math.round(Number(evalStaticOr(get('duration'), env, 250)))};`);
    pushEasing(lines, c, get('easing'), env);
  }
  if (delayMs > 0) lines.push(`        ${c}.delay_ms = ${delayMs};`);
  if (loop) lines.push(`        ${c}.loop = true;`);
  if (onCompleteCb) lines.push(`        ${c}.on_complete = ${onCompleteCb};`);
  // decay is velocity-driven and has no toValue target; every other type needs one.
  const toNode = get('toValue');
  if (!toNode && kind !== 'decay') throw aotError(`AOT: Animated.${kind}() config needs a toValue`);
  const toCode = toNode ? emitExpr(toNode, env).code : `er_anim_value_get(${cVar})`;
  lines.push(`        er_anim_value_animate(${cVar}, (float)(${toCode}), &${c});`, '    }');
  return lines;
}

/** Flattens an Animated composition (timing/spring/decay/sequence/parallel/stagger/delay/loop) into a flat
 *  list of atomic entries, each with an ABSOLUTE start delay (ms) — composition is realised purely through
 *  per-entry delay_ms (no engine grouping needed for standalone values). Returns { entries, duration } where
 *  `duration` is this node's own run length in ms, used to offset later siblings in a sequence/stagger;
 *  null = unknown (spring/decay/loop), which is illegal to sequence anything after. */
function flattenAnim(node, env, baseDelay, loop) {
  if (node?.type !== 'CallExpression' || node.callee.type !== 'MemberExpression' || node.callee.object?.name !== 'Animated')
    throw aotError('AOT: an animation must be Animated.timing/spring/decay/sequence/parallel/stagger/delay/loop(...)');
  const kind = node.callee.property.name;
  const args = node.arguments;

  if (kind === 'timing' || kind === 'spring' || kind === 'decay') {
    const valRef = args[0];
    if (valRef?.type !== 'Identifier' || !env.anims?.has(valRef.name)) throw aotError(`AOT: Animated.${kind}() first argument must be a useAnimatedValue`);
    const cVar = env.anims.get(valRef.name).cVar;
    const get = animConfigGetter(args[1]);
    const ownDelay = Math.round(Number(evalStaticOr(get('delay'), env, 0)));
    const duration = kind === 'timing' ? ownDelay + Math.round(Number(evalStaticOr(get('duration'), env, 250))) : null;
    return { entries: [{ cVar, kind, get, delayMs: baseDelay + ownDelay, loop }], duration };
  }
  if (kind === 'delay') {
    return { entries: [], duration: Math.round(Number(evalStaticOr(args[0], env, 0))) };
  }
  if (kind === 'sequence' || kind === 'parallel' || kind === 'stagger') {
    const list = kind === 'stagger' ? args[1] : args[0];
    const staggerMs = kind === 'stagger' ? Math.round(Number(evalStaticOr(args[0], env, 0))) : 0;
    if (list?.type !== 'ArrayExpression') throw aotError(`AOT: Animated.${kind}(...) needs an array of animations`);
    const entries = [];
    let off = baseDelay; // running offset (sequence)
    let groupDur = 0;    // max end-time relative to baseDelay (parallel/stagger)
    let i = 0;
    for (const child of list.elements) {
      if (!child) continue;
      const start = kind === 'sequence' ? off : baseDelay + i * staggerMs;
      const r = flattenAnim(child, env, start, loop);
      entries.push(...r.entries);
      if (kind === 'sequence') {
        if (r.duration == null) throw aotError('AOT: an Animated.sequence entry needs a known duration — use Animated.timing / Animated.delay (a spring/decay/loop inside a sequence is not supported; it has no fixed length to offset the next entry by)');
        off += r.duration;
      } else {
        const end = (start - baseDelay) + (r.duration ?? 0);
        if (end > groupDur) groupDur = end;
      }
      i++;
    }
    // Same value can't appear twice in a flat (delay_ms) composition: er_anim_value_animate cancels the
    // running anim on a value, so concurrent/flat-sequenced same-value steps cancel each other. (A
    // top-level Animated.sequence is handled separately via on_complete chaining, which does support this.)
    const seen = new Set();
    for (const e of entries) {
      if (seen.has(e.cVar))
        throw aotError('AOT: the same animated value is driven more than once in this composition — concurrent/flat same-value steps cancel each other. Use a top-level Animated.sequence(...) for multi-step animation of one value.');
      seen.add(e.cVar);
    }
    return { entries, duration: kind === 'sequence' ? off - baseDelay : groupDur };
  }
  if (kind === 'loop') {
    const r = flattenAnim(args[0], env, baseDelay, true);
    if (r.entries.length !== 1) throw aotError('AOT: Animated.loop currently wraps a single Animated.timing/spring/decay (looping a sequence/parallel is not yet supported)');
    return { entries: r.entries, duration: null };
  }
  throw aotError(`AOT: Animated.${kind}(...) is not a supported animation`);
}

/** Lowers `Animated.sequence([...]).start()` to an on_complete CHAIN: step 0 starts inline (in the handler),
 *  and each step's completion callback starts the next. Unlike the flat delay_ms path this is correct when
 *  steps share a value (out-and-back) — er_anim_value_animate cancels the running anim on a value, so
 *  synchronous same-value animates would cancel each other — and needs no fixed duration (spring/decay OK).
 *  delay() entries fold into the next step's delay_ms. Nested parallel/stagger/loop in a sequence throws. */
function compileSequenceChain(seqNode, env, ctx) {
  const list = seqNode.arguments[0];
  if (list?.type !== 'ArrayExpression') throw aotError('AOT: Animated.sequence(...) needs an array of animations');
  const steps = [];
  let pendingDelay = 0;
  for (const child of list.elements) {
    if (!child) continue;
    if (child.type !== 'CallExpression' || child.callee.type !== 'MemberExpression' || child.callee.object?.name !== 'Animated')
      throw aotError('AOT: Animated.sequence entries must be Animated.timing/spring/decay/delay(...)');
    const kind = child.callee.property.name;
    if (kind === 'delay') {
      pendingDelay += Math.round(Number(evalStaticOr(child.arguments[0], env, 0)));
      continue;
    }
    if (kind !== 'timing' && kind !== 'spring' && kind !== 'decay')
      throw aotError('AOT: Animated.sequence entries must be Animated.timing/spring/decay/delay (a nested parallel/stagger/loop inside a sequence is not yet supported — keep the sequence flat)');
    const valRef = child.arguments[0];
    if (valRef?.type !== 'Identifier' || !env.anims?.has(valRef.name)) throw aotError(`AOT: Animated.${kind}() first argument must be a useAnimatedValue`);
    const get = animConfigGetter(child.arguments[1]);
    const ownDelay = Math.round(Number(evalStaticOr(get('delay'), env, 0)));
    steps.push({ cVar: env.anims.get(valRef.name).cVar, kind, get, delayMs: pendingDelay + ownDelay, loop: false });
    pendingDelay = 0;
  }
  if (!steps.length) return [];
  const seqId = ctx.out.seqN++; // GLOBAL: callback names are file-scope, so must be unique across handlers
  let firstLines = [];
  // Build from the tail so each step knows its successor's callback name. Step 0 runs inline; the rest
  // become on_complete callbacks pushed to out.animCbs (emitted at file scope).
  for (let i = steps.length - 1; i >= 0; i--) {
    const nextCb = i < steps.length - 1 ? `er_seqcb_${seqId}_${i + 1}` : null;
    const lines = emitAnimEntry(steps[i], env, `${seqId}_${i}`, nextCb);
    if (i === 0) firstLines = lines;
    else ctx.out.animCbs.push({ name: `er_seqcb_${seqId}_${i}`, body: lines });
  }
  return firstLines;
}

/** Compiles `<animation>.start()` — a single Animated.timing/spring/decay or a composition
 *  (sequence/parallel/stagger/delay/loop). A top-level sequence chains via on_complete (compileSequenceChain);
 *  everything else flattens to one ERAnimConfig + er_anim_value_animate per atomic entry, composition
 *  expressed through per-entry delay_ms. Native-driven; sets no React state. */
function compileAnimateStart(expr, env, ctx) {
  const receiver = expr.callee.object;
  if (receiver?.type === 'CallExpression' && receiver.callee.type === 'MemberExpression' && receiver.callee.object?.name === 'Animated' && receiver.callee.property.name === 'sequence') {
    return compileSequenceChain(receiver, env, ctx);
  }
  const { entries } = flattenAnim(receiver, env, 0, false);
  const lines = [];
  for (const e of entries) lines.push(...emitAnimEntry(e, env, ctx.animIdx++));
  return lines;
}

/** evalStatic with a fallback default when the node is absent or not foldable. */
function evalStaticOr(node, env, dflt) {
  if (!node) return dflt;
  try {
    return evalStatic(node, env.consts ?? {});
  } catch {
    return dflt;
  }
}

/** Returns a statement node's body list: a BlockStatement's contents, or the lone statement wrapped. */
function blockList(node) {
  return node.type === 'BlockStatement' ? node.body : [node];
}

/** Emits C to write a value into a scalar state slot: snprintf for a string buffer, plain assign else. */
function scalarAssign(rec, e, indent) {
  if (rec.cType === 'string') return `${indent}snprintf(${rec.cMember}, sizeof(${rec.cMember}), "${printfSpec(e.cType)}", ${e.code});`;
  return `${indent}${rec.cMember} = ${e.code};`;
}

/** True if `node` is a `<ref>.current` member access on a known value ref. */
function refTarget(node, env) {
  if (node?.type === 'MemberExpression' && !node.computed && node.object.type === 'Identifier' && node.property.name === 'current' && env.refs?.has(node.object.name)) {
    return env.refs.get(node.object.name);
  }
  return null;
}

/** An imperative updateVector shape `{ arc:[…]|circle:[…]|rect:[…]|line:[…]|path:'…', fill, stroke, … }`
 *  → { entries (op-tape C exprs), paint (static 7-num record) }. Geometry coords may reference state/refs/
 *  event fields (emitExpr); paint must be static. Shares the ...EntriesC geometry with the JSX path. */
function imperativeShape(shapeNode, env) {
  if (shapeNode?.type !== 'ObjectExpression') throw new Error('AOT: each updateVector shape must be an object literal');
  const props = {};
  for (const p of shapeNode.properties) {
    if (p.type !== 'ObjectProperty') throw new Error('AOT: spread/method in an updateVector shape not supported');
    props[p.key.name ?? p.key.value] = p.value;
  }
  const arr = (key, n) => {
    if (props[key].type !== 'ArrayExpression') throw new Error(`AOT: updateVector "${key}" must be an array literal`);
    return props[key].elements.slice(0, n).map((el) => `(float)(${emitExpr(el, env).code})`);
  };
  let entries;
  if (props.arc) entries = arcEntriesC(...arr('arc', 5));
  else if (props.circle) entries = circleEntriesC(...arr('circle', 3));
  else if (props.rect) entries = rectEntriesC(...arr('rect', 4));
  else if (props.line) entries = lineEntriesC(...arr('line', 4));
  else if (props.path) entries = parsePath(String(evalStatic(props.path, env.consts ?? {}))).map(floatLit);
  else throw new Error('AOT: an updateVector shape needs one of arc / circle / rect / line / path');
  const stat = (key, dflt) => {
    if (props[key] == null) return dflt;
    try {
      return evalStatic(props[key], env.consts ?? {});
    } catch {
      throw new Error(`AOT: updateVector paint "${key}" must be static`);
    }
  };
  const paint = [parseColor(stat('fill', 'none')), parseColor(stat('stroke', 'none')), svgNum(stat('strokeWidth', 1), 1), svgNum(stat('miter', 4), 4), CAP_MAP[stat('cap', 'butt')] ?? 0, JOIN_MAP[stat('join', 'miter')] ?? 0, stat('fillRule', 'nonzero') === 'evenodd' ? 1 : 0];
  return { entries, paint };
}

/** Lowers `updateVector(nodeRef, shapes, [x,y,w,h]?)` to: fill a mutable op-tape, push it to the node, and
 *  (optionally) hint the dirty sub-rect — the imperative fast path (drag) that bypasses app_update. */
function compileUpdateVector(expr, env, ctx, indent) {
  const out = ctx.out;
  const [refArg, shapesArg, dirtyArg] = expr.arguments;
  const ref = refArg?.type === 'Identifier' ? env.refs?.get(refArg.name) : null;
  if (ref?.kind !== 'node') throw new Error('AOT: updateVector(ref, …) first arg must be a node ref (const r = useRef())');
  if (shapesArg?.type !== 'ArrayExpression') throw new Error('AOT: updateVector(ref, shapes, …) shapes must be an array literal');
  const entries = [];
  const paints = [];
  for (const s of shapesArg.elements) {
    const { entries: e, paint } = imperativeShape(s, env);
    entries.push('ER_VOP_SHAPE', floatLit(paints.length), ...e);
    paints.push(paint);
  }
  const id = out.svgN++;
  const len = entries.length;
  out.needsMath = true;
  out.vectorData.push(`static float s_uv${id}_ops[${len}];`);
  out.vectorData.push(`static const ERVectorPaint s_uv${id}_paints[] = {\n${paints.map((p) => '    ' + emitVectorPaint(p)).join(',\n')}\n};`);
  const lines = entries.map((e, i) => `${indent}s_uv${id}_ops[${i}] = ${e};`);
  lines.push(`${indent}er_node_set_vector_ops(${ref.cVar}, s_uv${id}_ops, ${len}, s_uv${id}_paints, ${paints.length});`);
  if (dirtyArg) {
    if (dirtyArg.type !== 'ArrayExpression' || dirtyArg.elements.length < 4) throw new Error('AOT: updateVector dirtyRect must be a [x, y, w, h] array literal');
    const [x, y, w, h] = dirtyArg.elements.map((el) => emitExpr(el, env).code);
    lines.push(`${indent}er_node_set_vector_dirty_rect(${ref.cVar}, ${x}, ${y}, ${w}, ${h});`);
  }
  return lines;
}

/** Compiles one handler ExpressionStatement: a state setter, ref mutation, or Animated.*(...).start(). */
/** setInterval/setTimeout(cb, ms) → a C `er_timer_add(ms, repeat, fn)` expr; registers cb as a timer fn. */
function compileTimerAdd(expr, env, state, ctx) {
  const cb = expr.arguments[0];
  if (!isFn(cb)) throw aotError('AOT: a setInterval/setTimeout callback must be an inline function', 'pass an inline arrow, e.g. setInterval(() => setTick((t) => t + 1), 1000).');
  const ms = expr.arguments[1] ? emitExpr(expr.arguments[1], env).code : '0';
  const repeat = expr.callee.name === 'setInterval';
  const slot = ctx.out.timerFns.length;
  const name = `er_timer_fn_${slot}`;
  ctx.out.usesTimers = true;
  ctx.out.timerFns.push({ name, body: null }); // reserve the slot before compiling the body (it may add more)
  ctx.out.timerFns[slot].body = compileHandler(cb, env, state, ctx.out);
  return `er_timer_add((int)(${ms}), ${repeat ? 'true' : 'false'}, ${name})`;
}

function compileHandlerExprImpl(expr, env, state, ctx, indent) {
  // updateVector(ref, shapes, dirtyRect?) — imperative vector redraw (no app_update).
  if (expr.type === 'CallExpression' && expr.callee.type === 'Identifier' && expr.callee.name === 'updateVector') {
    return compileUpdateVector(expr, env, ctx, indent);
  }
  // setInterval / setTimeout(cb, ms) → register a host-tick timer (the returned id is discarded here).
  if (expr.type === 'CallExpression' && expr.callee.type === 'Identifier' && (expr.callee.name === 'setInterval' || expr.callee.name === 'setTimeout')) {
    return [`${indent}${compileTimerAdd(expr, env, state, ctx)};`];
  }
  // clearInterval / clearTimeout(id) → deactivate the timer slot.
  if (expr.type === 'CallExpression' && expr.callee.type === 'Identifier' && (expr.callee.name === 'clearInterval' || expr.callee.name === 'clearTimeout')) {
    return [`${indent}er_timer_clear(${emitExpr(expr.arguments[0], env).code});`];
  }
  // `ref.current = expr` / `ref.current += expr` — a value ref write; does NOT trigger a re-render.
  if (expr.type === 'AssignmentExpression') {
    const r = refTarget(expr.left, env);
    if (!r) throw new Error('AOT: the only assignment allowed in a handler is `ref.current = ...`');
    return [`${indent}${r.cVar} ${expr.operator} ${emitExpr(expr.right, env).code};`];
  }
  // `ref.current++` / `ref.current--`.
  if (expr.type === 'UpdateExpression') {
    const r = refTarget(expr.argument, env);
    if (!r) throw new Error('AOT: the only ++/-- allowed in a handler is on `ref.current`');
    return [`${indent}${r.cVar}${expr.operator};`];
  }
  // Animated.*(…).start() — single timing/spring/decay OR a sequence/parallel/stagger/delay/loop
  // composition; native-driven, sets no React state, needs no app_update.
  if (expr.type === 'CallExpression' && expr.callee.type === 'MemberExpression' && expr.callee.property.name === 'start') {
    return compileAnimateStart(expr, env, ctx);
  }
  if (expr.type !== 'CallExpression' || expr.callee.type !== 'Identifier') throw aotError('AOT: a handler statement must be a state setter, a ref write, or Animated.timing/spring(...).start()', 'each statement in a handler must be one of: setX(value) / setX(prev => …), a `ref.current = …` write, an `updateVector(…)` call, or `Animated.timing|spring(v, …).start()`. Wrap conditional logic in `if (…) { … }`.');
  const rec = state.bySetter.get(expr.callee.name);
  if (!rec)
    throw aotError(
      `AOT: "${expr.callee.name}" is not a known state setter`,
      `a handler can only call a setter from this component's own useState (e.g. setCount), a ref write, updateVector(…), or Animated…start(). "${expr.callee.name}" isn't one of those — arbitrary functions (fetch, console.*, helpers) can't be lowered to C.`,
    );
  ctx.stateChanged = true;
  const arg = expr.arguments[0];
  if (rec.kind === 'list') return compileListOp(rec, arg, env);
  if (arg && (arg.type === 'ArrowFunctionExpression' || arg.type === 'FunctionExpression')) {
    // setState(prev => expr): bind the param to the current value, assign the result.
    const param = arg.params[0]?.name;
    const locals = new Map(env.locals);
    if (param) locals.set(param, { code: rec.cMember, cType: rec.cType });
    if (arg.body.type === 'BlockStatement') throw new Error('AOT: updater function must be a single expression (for now)');
    return [scalarAssign(rec, emitExpr(arg.body, { ...env, locals }), indent)];
  }
  return [scalarAssign(rec, emitExpr(arg, env), indent)];
}
const compileHandlerExpr = withLoc(compileHandlerExprImpl);

/**
 * Compiles a list of handler statements to C lines. Supports: `const x = expr` (a C local, visible to
 * later statements), `if (cond) {...} else {...}`, state setters, and Animated.*(...).start(). `ctx`
 * accumulates `stateChanged` (→ trailing app_update) and `animIdx` (unique ERAnimConfig locals).
 */
function compileStmts(list, env, state, ctx, indent) {
  const lines = [];
  for (const st of list) {
    if (st.type === 'VariableDeclaration') {
      for (const decl of st.declarations) {
        if (decl.id.type !== 'Identifier') throw new Error('AOT: destructuring a handler local is not supported');
        if (!decl.init) throw new Error('AOT: a handler local must have an initializer');
        const cName = `l_${decl.id.name}`;
        // `const id = setInterval/setTimeout(…)` → an int timer-id local (so a later clearInterval(id) resolves).
        if (decl.init.type === 'CallExpression' && decl.init.callee.type === 'Identifier' && (decl.init.callee.name === 'setInterval' || decl.init.callee.name === 'setTimeout')) {
          // The id is only needed for a later clear*(); a mount effect drops its cleanup, so mark it used.
          lines.push(`${indent}int ${cName} = ${compileTimerAdd(decl.init, env, state, ctx)};`, `${indent}(void)${cName};`);
          env = { ...env, locals: new Map(env.locals).set(decl.id.name, { code: cName, cType: 'int' }) };
          continue;
        }
        const e = emitExpr(decl.init, env);
        lines.push(`${indent}${e.cType === 'float' ? 'float' : 'int'} ${cName} = ${e.code};`);
        env = { ...env, locals: new Map(env.locals).set(decl.id.name, { code: cName, cType: e.cType }) };
      }
      continue;
    }
    // An effect's cleanup `return () => …` — not run on an MCU (the app never unmounts), so drop it. (A
    // dep-driven effect that needs cleanup on re-run isn't supported yet; mount-only effects use this.)
    if (st.type === 'ReturnStatement') {
      if (ctx.allowReturn) continue;
      throw new Error('AOT: `return` is only allowed as an effect cleanup');
    }
    if (st.type === 'IfStatement') {
      lines.push(`${indent}if (${emitExpr(st.test, env).code})`, `${indent}{`);
      lines.push(...compileStmts(blockList(st.consequent), env, state, ctx, indent + '    '));
      lines.push(`${indent}}`);
      if (st.alternate) {
        lines.push(`${indent}else`, `${indent}{`);
        lines.push(...compileStmts(blockList(st.alternate), env, state, ctx, indent + '    '));
        lines.push(`${indent}}`);
      }
      continue;
    }
    if (st.type !== 'ExpressionStatement')
      throw aotError(
        `AOT: unsupported statement "${st.type}" in event handler`,
        'a handler supports only `const x = …` locals, `if (…) { … } else { … }`, and expression statements (setters / ref writes / updateVector / Animated…start). Loops (for/while), switch, try/catch, and early return are not lowered — precompute values or flatten the logic into if/else.',
      );
    lines.push(...compileHandlerExpr(st.expression, env, state, ctx, indent));
  }
  return lines;
}

function compileHandler(fnNode, env, state, out) {
  const body = fnNode.body;
  const list = body.type === 'BlockStatement' ? body.body : [{ type: 'ExpressionStatement', expression: body }];
  // The handler's first parameter is the event; `<event>.x/.y/.dx/.dy` map to EREventData fields.
  const eventParam = fnNode.params[0]?.type === 'Identifier' ? fnNode.params[0].name : null;
  const henv = eventParam ? { ...env, event: eventParam } : env;
  const ctx = { stateChanged: false, animIdx: 0, out };
  const stmts = compileStmts(list, henv, state, ctx, '    ');
  if (ctx.stateChanged) stmts.push('    app_update();'); // re-apply state-dependent props once
  return stmts;
}

// ---------------------------------------------------------------------------------------------------
// Emit — control flow (components / conditionals / lists) all unroll at COMPILE TIME into fixed nodes.
// Runtime-dynamic conditionals/lists (where the node COUNT changes with state) are not yet supported
// and throw a clear "AOT: ..." — see /PLAN.md Flow B.
// ---------------------------------------------------------------------------------------------------

/** Reads a component instance's props (attributes) as static values; dynamic props throw (for now). */
/** Reads a component's props as descriptors: `{static:true,value}` (folded) or `{static:false,code,cType,struct}`
 *  (a runtime C expression — e.g. a list row's `item.field`). */
function extractProps(openingElement, scope, env) {
  const props = {};
  for (const attr of openingElement.attributes) {
    if (attr.type === 'JSXSpreadAttribute') {
      // Static spread: {...obj} where obj folds to a compile-time object → merge its keys as props.
      let obj;
      try {
        obj = evalStatic(attr.argument, scope);
      } catch {
        throw new Error('AOT: only a compile-time-constant object can be spread to a component ({...obj})');
      }
      if (obj == null || typeof obj !== 'object') throw new Error('AOT: a component spread {...x} must resolve to an object');
      for (const [k, v] of Object.entries(obj)) props[k] = { static: true, value: v };
      continue;
    }
    if (attr.type !== 'JSXAttribute' || attr.name.name === 'key') continue;
    const node = attrExpr(attr);
    // Callback prop: a function passed to a child (inline arrow, or an identifier bound to a useCallback in
    // the caller). Captured as a `fn` descriptor and resolved where the child uses it as an event handler.
    if (isFn(node)) {
      props[attr.name.name] = { fn: true, node };
      continue;
    }
    if (node.type === 'Identifier' && env.callbacks?.has(node.name)) {
      props[attr.name.name] = { fn: true, node: env.callbacks.get(node.name) };
      continue;
    }
    if (node.type === 'Identifier' && env.fnProps?.has(node.name)) {
      props[attr.name.name] = { fn: true, ...env.fnProps.get(node.name) }; // forward original {node, env, state}
      continue;
    }
    try {
      props[attr.name.name] = { static: true, value: evalStatic(node, scope) };
    } catch {
      props[attr.name.name] = { static: false, ...emitExpr(node, env) };
    }
  }
  return props;
}

/** Maps a component's parameter to its prop descriptors (handles destructure rename + defaults). */
function bindParams(fn, props) {
  const out = new Map();
  const param = fn.params[0];
  if (!param) return out;
  if (param.type === 'Identifier') {
    const obj = {};
    for (const [k, d] of Object.entries(props)) {
      if (!d.static) throw new Error('AOT: dynamic props require a destructured component parameter (e.g. `function C({ x })`)');
      obj[k] = d.value;
    }
    out.set(param.name, { static: true, value: obj });
  } else if (param.type === 'ObjectPattern') {
    for (const p of param.properties) {
      if (p.type === 'RestElement') throw new Error('AOT: rest props (...rest) in a component param not supported');
      const propName = p.key.name ?? p.key.value;
      const bindName = p.value?.type === 'Identifier' ? p.value.name : p.value?.type === 'AssignmentPattern' ? p.value.left.name : propName;
      let d = props[propName];
      if (!d && p.value?.type === 'AssignmentPattern') d = { static: true, value: evalStatic(p.value.right, {}) };
      out.set(bindName, d ?? { static: true, value: undefined });
    }
  } else {
    throw new Error('AOT: unsupported component parameter pattern');
  }
  return out;
}

/** True if `expr` is how a component body refers to its children (destructured {children} or props.children). */
function isChildrenRef(expr, env) {
  const cr = env.children?.ref;
  if (!cr) return false;
  if (cr.kind === 'local') return expr.type === 'Identifier' && expr.name === cr.name;
  return expr.type === 'MemberExpression' && !expr.computed && expr.object.type === 'Identifier' && expr.object.name === cr.name && expr.property.name === 'children';
}

/** Inlines a function component instance: bind props (static → scope, dynamic → locals), emit its JSX.
 *  Children passed at the call site are captured and emitted (in the CALLER's scope) where the body uses them. */
function emitComponent(el, scope, out, env, state, opts) {
  const tag = el.openingElement.name.name;
  const fn = out.components.get(tag);
  if (usesState(fn))
    throw aotError(
      `AOT: component <${tag}> uses useState — per-instance child state not yet supported`,
      `child components are inlined at compile time and can't yet own state. Lift the state into the parent (<${tag}>'s caller) and pass value + a setter callback down as props, or keep <${tag}> presentational (props only).`,
    );
  const childNodes = el.children.filter((c) => c.type === 'JSXElement' || (c.type === 'JSXExpressionContainer' && c.expression.type !== 'JSXEmptyExpression'));

  // How the body refers to children: destructured `{ children }` (a local) or whole `props` → props.children.
  const param = fn.params[0];
  let childrenRef = null;
  if (param?.type === 'ObjectPattern') {
    for (const p of param.properties) if ((p.key?.name ?? p.key?.value) === 'children') childrenRef = { kind: 'local', name: p.value?.name ?? 'children' };
  } else if (param?.type === 'Identifier') {
    childrenRef = { kind: 'props', name: param.name };
  }

  const childScope = { ...scope };
  const childLocals = new Map(env.locals);
  // Callback props bound here resolve to the CALLER's function (node + caller env/state) so the child can
  // use them as event handlers (onPress={onTap}); inherit any the caller itself received (forwarding).
  const fnProps = new Map(env.fnProps);
  for (const [name, d] of bindParams(fn, extractProps(el.openingElement, scope, env))) {
    if (childrenRef?.kind === 'local' && name === childrenRef.name) continue; // children come from the slot, not a value prop
    if (d.fn) fnProps.set(name, { node: d.node, env: d.env ?? env, state: d.state ?? state });
    else if (d.static) childScope[name] = d.value;
    else childLocals.set(name, { code: d.code, cType: d.cType, struct: d.struct });
  }
  const children = childNodes.length ? { nodes: childNodes, scope, env, ref: childrenRef } : null;
  return emitNode(componentReturnJSX(fn, childScope), childScope, out, { ...env, consts: childScope, locals: childLocals, children, fnProps }, state, opts);
}

/** Emits an element / component child and appends it to the parent. opts.displayCode toggles its show. */
function emitElementInto(node, parentVar, scope, out, env, state, opts) {
  if (node.type !== 'JSXElement') throw new Error(`AOT: expected a JSX element here, got ${node.type}`);
  const cv = emitNode(node, scope, out, env, state, opts);
  out.build.push(`    er_tree_append_child(${parentVar}, ${cv});`);
}

/** Unrolls `arr.map((item, i) => <JSX/>)` over a COMPILE-TIME-CONSTANT array. */
function emitMap(call, parentVar, scope, out, env, state) {
  let arr;
  try {
    arr = evalStatic(call.callee.object, scope);
  } catch {
    throw new Error('AOT: .map target must be a compile-time-constant array (dynamic lists not yet supported)');
  }
  if (!Array.isArray(arr)) throw new Error('AOT: .map target did not resolve to an array');
  const cb = call.arguments[0];
  if (!isFn(cb)) throw new Error('AOT: .map argument must be an inline function');
  const itemName = cb.params[0]?.name;
  const idxName = cb.params[1]?.name;
  const retJSX = componentReturnJSX(cb);
  arr.forEach((item, i) => {
    const iterScope = { ...scope };
    if (itemName) iterScope[itemName] = item;
    if (idxName) iterScope[idxName] = i;
    emitElementInto(retJSX, parentVar, iterScope, out, { ...env, consts: iterScope }, state);
  });
}

/**
 * `{listState.map((item, i) => <Row/>)}` over a STATE array of variable length. Pre-allocates a fixed
 * pool of `cap` rows (no runtime malloc); each row k binds `item` to `s_<name>[k]` (a struct local) and
 * is shown only while `k < count` (display toggle). app_update then drives every row's content and show.
 */
function emitDynamicMap(call, rec, parentVar, scope, out, env, state) {
  const cb = call.arguments[0];
  if (!isFn(cb)) throw new Error('AOT: .map argument must be an inline function');
  const itemName = cb.params[0]?.name;
  const idxName = cb.params[1]?.name;
  const retJSX = componentReturnJSX(cb);
  for (let k = 0; k < rec.cap; k++) {
    const iterScope = { ...scope };
    if (idxName) iterScope[idxName] = k; // the index is a compile-time literal per pooled row
    const locals = new Map(env.locals);
    if (itemName) locals.set(itemName, { code: `${rec.arrayName}[${k}]`, struct: rec.struct });
    emitElementInto(retJSX, parentVar, iterScope, out, { ...env, consts: iterScope, locals }, state, { displayCode: `(${k} < ${rec.countMember})` });
  }
}

/** Emits the children of a container node, handling element + {expression} children. */
function emitChildren(children, parentVar, scope, out, env, state) {
  for (const child of children) {
    if (child.type === 'JSXElement') {
      emitElementInto(child, parentVar, scope, out, env, state);
    } else if (child.type === 'JSXExpressionContainer') {
      const expr = child.expression;
      if (expr.type === 'JSXEmptyExpression') continue;
      if (isChildrenRef(expr, env)) {
        // {children} / {props.children}: emit the captured call-site children, in the caller's scope/env.
        emitChildren(env.children.nodes, parentVar, env.children.scope, out, env.children.env, state);
        continue;
      }
      if (expr.type === 'LogicalExpression' && expr.operator === '&&') {
        // `{cond && <X/>}`. Static cond → include/omit at compile time. Dynamic (state) cond → always
        // build X but toggle its display (none/flex) in app_update — show/hide without node churn.
        let cond;
        try {
          cond = evalStatic(expr.left, scope);
          if (cond) emitElementInto(expr.right, parentVar, scope, out, env, state);
        } catch {
          const code = emitExpr(expr.left, env).code;
          emitElementInto(expr.right, parentVar, scope, out, env, state, { displayCode: code });
        }
      } else if (expr.type === 'ConditionalExpression' && (expr.consequent.type === 'JSXElement' || expr.alternate.type === 'JSXElement')) {
        // `{cond ? <A/> : <B/>}`. Static cond picks a branch; dynamic cond builds both and toggles each.
        let test;
        try {
          test = evalStatic(expr.test, scope);
          emitElementInto(test ? expr.consequent : expr.alternate, parentVar, scope, out, env, state);
        } catch {
          const code = emitExpr(expr.test, env).code;
          if (expr.consequent.type === 'JSXElement') emitElementInto(expr.consequent, parentVar, scope, out, env, state, { displayCode: code });
          if (expr.alternate.type === 'JSXElement') emitElementInto(expr.alternate, parentVar, scope, out, env, state, { displayCode: `!(${code})` });
        }
      } else if (expr.type === 'CallExpression' && expr.callee.type === 'MemberExpression' && expr.callee.property.name === 'map') {
        const obj = expr.callee.object;
        const rec = obj.type === 'Identifier' ? env.state.get(obj.name) : null;
        if (rec?.kind === 'list') emitDynamicMap(expr, rec, parentVar, scope, out, env, state);
        else emitMap(expr, parentVar, scope, out, env, state);
      } else {
        // A constant that renders nothing (false/null/'') is fine; anything else is unsupported.
        let v;
        try {
          v = evalStatic(expr, scope);
        } catch {
          const e = aotError(
            `AOT: unsupported expression child "${expr.type}" in a container`,
            'a child expression must be a JSX element, `cond && <El/>` / a ternary of elements, or `list.map(item => <El/>)`. A bare variable holding JSX isn\'t inlined — write the element directly. (If this is a responsive Flow-A-only branch, compile at the target board size via ER_AOT_SCREEN_W/H so the AOT folds the supported branch.)',
          );
          if (expr.loc) e.aotLoc = expr.loc.start;
          throw e;
        }
        if (v !== false && v != null && v !== '') {
          const e = aotError(`AOT: a non-element expression child (${JSON.stringify(v)}) cannot render here`, 'only JSX elements render as children; wrap text in a <Text>{…}</Text>.');
          if (expr.loc) e.aotLoc = expr.loc.start;
          throw e;
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------------------------------
// Vector / Svg — a static <Svg> subtree is converted to flattenSvg()'s element shape (the same converter
// Flow A uses), giving a flat {ops, paints}. We bake those into C const arrays + er_node_set_vector_ops.
// ---------------------------------------------------------------------------------------------------

/** Converts an SVG JSX element (Svg/Circle/Path/Rect/Line/Arc/G/…) to flattenSvg's `{type, props}` shape,
 *  statically evaluating every attribute. Dynamic attrs/children throw (state-driven Svg is Phase 6b). */
function jsxToSvgElement(node, scope) {
  if (node.type !== 'JSXElement') return null;
  const type = node.openingElement.name.name;
  const props = {};
  for (const attr of node.openingElement.attributes) {
    if (attr.type !== 'JSXAttribute') throw new Error('AOT: spread attributes on an <Svg> element not supported');
    const name = attr.name.name;
    if (name === 'ref' || name === 'key') continue; // not geometry/paint
    if (attr.value == null) props[name] = true;
    else if (attr.value.type === 'StringLiteral') props[name] = attr.value.value;
    else if (attr.value.type === 'JSXExpressionContainer') props[name] = evalStatic(attr.value.expression, scope);
    else throw new Error(`AOT: unsupported <${type}> attribute value for "${name}"`);
  }
  const children = [];
  for (const c of node.children) {
    if (c.type === 'JSXElement') children.push(jsxToSvgElement(c, scope));
    else if (c.type === 'JSXExpressionContainer' && c.expression.type !== 'JSXEmptyExpression') throw new Error('AOT: dynamic <Svg> children ({…}) not yet supported — use literal shape elements');
  }
  if (children.length) props.children = children;
  return { type, props };
}

/** Emits one ERVectorPaint initializer from a 7-number flattenSvg paint record [fill,stroke,w,miter,cap,join,rule]. */
function emitVectorPaint(p) {
  return `{ .fill = ${p[0] >>> 0}u, .stroke = ${p[1] >>> 0}u, .stroke_w = ${floatLit(p[2])}, .miter = ${floatLit(p[3])}, .cap = ${p[4] | 0}, .join = ${p[5] | 0}, .fill_rule = ${p[6] | 0} }`;
}

/** Static numeric coercion for an SVG attribute value (mirrors svg-ops `num`). */
const svgNum = (v, d = 0) => {
  const n = typeof v === 'number' ? v : parseFloat(v);
  return Number.isNaN(n) ? d : n;
};
/** True if an attribute value is a state-driven C expression (vs a static number/string). */
const isDyn = (v) => v != null && typeof v === 'object' && 'dyn' in v;
/** Lowers an SVG coordinate attr to a C float expression (literal when static, cast expr when dynamic). */
const cf = (v, d = 0) => (isDyn(v) ? `(float)(${v.dyn})` : floatLit(svgNum(v, d)));

/** Reads an SVG element's attributes → { name: number|string|true | {dyn: cExpr} } (state attrs → {dyn}). */
function svgAttrs(openingElement, scope, env) {
  const out = {};
  for (const attr of openingElement.attributes) {
    if (attr.type !== 'JSXAttribute') throw new Error('AOT: spread attributes on an <Svg> element not supported');
    const name = attr.name.name;
    if (name === 'ref' || name === 'key') continue; // not geometry/paint
    const vn = attr.value;
    if (vn == null) out[name] = true;
    else if (vn.type === 'StringLiteral') out[name] = vn.value;
    else if (vn.type === 'JSXExpressionContainer') {
      try {
        out[name] = evalStatic(vn.expression, scope);
      } catch {
        // Keep the raw expression node too: color paint attrs (fill/stroke) lower via emitColorExpr (→ ARGB),
        // not the generic numeric `dyn` code, so a dynamic color resolves to a uint, not a char*.
        out[name] = { dyn: emitExpr(vn.expression, env).code, node: vn.expression };
      }
    } else throw new Error(`AOT: unsupported SVG attribute value for "${name}"`);
  }
  return out;
}

const CAP_MAP = { butt: 0, round: 1, square: 2 };
const JOIN_MAP = { miter: 0, round: 1, bevel: 2 };

/** The ERVectorPaint members, in op-tape paint order [fill,stroke,stroke_w,miter,cap,join,fill_rule]. */
const PAINT_FIELDS = ['fill', 'stroke', 'stroke_w', 'miter', 'cap', 'join', 'fill_rule'];

/**
 * A shape's paint as 7 C-expr fields (matching PAINT_FIELDS) + whether any is state-driven.
 *   - fill / stroke may be DYNAMIC → lowered via emitColorExpr to an ARGB uint expr (a color string, a
 *     ternary of them, or a folded theme token); static → a baked `0xAARRGGBBu` literal.
 *   - strokeWidth may be DYNAMIC (numeric C expr); static → a float literal.
 *   - cap / join / miterlimit / fillRule must be STATIC (a dynamic one throws clear).
 */
function paintSpec(a, env) {
  let anyDynamic = false;
  const color = (v, dflt) => {
    if (isDyn(v)) {
      anyDynamic = true;
      return emitColorExpr(v.node, env);
    }
    return `${parseColor(v ?? dflt) >>> 0}u`;
  };
  let strokeW;
  if (isDyn(a.strokeWidth)) {
    anyDynamic = true;
    strokeW = `(float)(${a.strokeWidth.dyn})`;
  } else strokeW = floatLit(svgNum(a.strokeWidth, 1));
  for (const k of ['strokeLinecap', 'strokeLinejoin', 'strokeMiterlimit', 'fillRule'])
    if (isDyn(a[k])) throw new Error(`AOT: a state-driven <Svg> "${k}" is not supported (only fill / stroke / strokeWidth can be state-driven)`);
  const fields = [color(a.fill, 'black'), color(a.stroke, 'none'), strokeW, floatLit(svgNum(a.strokeMiterlimit, 4)), String(CAP_MAP[a.strokeLinecap] ?? 0), String(JOIN_MAP[a.strokeLinejoin] ?? 0), String(a.fillRule === 'evenodd' ? 1 : 0)];
  return { fields, anyDynamic };
}

/** A `{ .fill = …, … }` ERVectorPaint initializer from a paintSpec's C-expr fields (used for static paints). */
function paintInitFromSpec(ps) {
  return `{ ${PAINT_FIELDS.map((f, i) => `.${f} = ${ps.fields[i]}`).join(', ')} }`;
}

// Per-shape op-tape entries (C-expr strings; opcodes as ER_VOP_* macros). The ...C base fns take resolved
// C-float expressions, so both the JSX path (6b, via cf) and the imperative updateVector path (6c, via
// arrays) share the geometry. Mirror svg-ops circleOps / arcOpsCW / etc.
const arcEntriesC = (cx, cy, r, a0deg, a1deg) => {
  const a0 = `((${a0deg} - 90.0f) * (float)M_PI / 180.0f)`;
  const a1 = `((${a1deg} - 90.0f) * (float)M_PI / 180.0f)`;
  return ['ER_VOP_MOVE', `(${cx} + ${r} * cosf(${a0}))`, `(${cy} + ${r} * sinf(${a0}))`, 'ER_VOP_ARC', cx, cy, r, a0, a1, '0.0f'];
};
const circleEntriesC = (cx, cy, r) => ['ER_VOP_MOVE', `(${cx} + ${r})`, cy, 'ER_VOP_ARC', cx, cy, r, '0.0f', '(2.0f * (float)M_PI)', '0.0f', 'ER_VOP_CLOSE'];
const rectEntriesC = (x, y, w, h) => ['ER_VOP_MOVE', x, y, 'ER_VOP_LINE', `(${x} + ${w})`, y, 'ER_VOP_LINE', `(${x} + ${w})`, `(${y} + ${h})`, 'ER_VOP_LINE', x, `(${y} + ${h})`, 'ER_VOP_CLOSE'];
const lineEntriesC = (x1, y1, x2, y2) => ['ER_VOP_MOVE', x1, y1, 'ER_VOP_LINE', x2, y2];

// JSX-attribute wrappers (6b): resolve each attr to a C float via cf().
const arcEntries = (a) => arcEntriesC(cf(a.cx), cf(a.cy), cf(a.r), cf(a.startAngle), cf(a.endAngle));
const circleEntries = (a) => circleEntriesC(cf(a.cx), cf(a.cy), cf(a.r));
const rectEntries = (a) => rectEntriesC(cf(a.x), cf(a.y), cf(a.width), cf(a.height));
const lineEntries = (a) => lineEntriesC(cf(a.x1), cf(a.y1), cf(a.x2), cf(a.y2));
const pathEntries = (a) => {
  if (a.d == null) return [];
  if (isDyn(a.d)) throw new Error('AOT: a state-driven <Path d=…> is not yet supported (use Arc/Circle/Rect/Line for dynamic shapes)');
  return parsePath(String(a.d)).map(floatLit); // opcodes are encoded as float values 0..6, like coords
};
const SHAPE_ENTRIES = { Arc: arcEntries, Circle: circleEntries, Rect: rectEntries, Line: lineEntries, Path: pathEntries };

/** True if any attribute anywhere in the <Svg> subtree references state (→ the state-driven path). */
function svgHasDynamic(el, scope) {
  let dyn = false;
  const walk = (node) => {
    if (node.type !== 'JSXElement') return;
    for (const attr of node.openingElement.attributes) {
      if (attr.type === 'JSXAttribute' && attr.name.name !== 'ref' && attr.name.name !== 'key' && attr.value?.type === 'JSXExpressionContainer') {
        try {
          evalStatic(attr.value.expression, scope);
        } catch {
          dyn = true;
        }
      }
    }
    for (const c of node.children) walk(c);
  };
  walk(el);
  return dyn;
}

/** Emits the vector node's box: create + props + width/height + optional style={}. */
function emitSvgBox(v, width, height, openingElement, scope, out, env) {
  const { staticAssigns } = collectStyleAssigns(openingElement, scope, env);
  out.build.push(`    ${v} = er_node_create(ER_NODE_VECTOR);`, `    er_props_default(&p);`);
  if (typeof width === 'number') out.build.push(`    p.width = (int16_t)${Math.round(width)};`);
  if (typeof height === 'number') out.build.push(`    p.height = (int16_t)${Math.round(height)};`);
  for (const a of staticAssigns) out.build.push(`    p.${a.field} = ${a.expr};`);
  out.build.push(`    er_node_set_props(${v}, &p);`);
  emitRefBind(v, openingElement, out, env);
}

/** <Svg> → ER_NODE_VECTOR. Static subtree → a baked const op-tape; any state-driven attr → a symbolic
 *  op-tape rebuilt by a generated build_svgN() at build time and on every app_update. */
function emitSvg(el, scope, out, env, state, opts) {
  if (opts.displayCode) throw new Error('AOT: an <Svg> inside a dynamic conditional is not yet supported');
  return svgHasDynamic(el, scope) ? emitSvgDynamic(el, scope, out, env, state) : emitSvgStatic(el, scope, out, env);
}

/** Static <Svg>: reuse flattenSvg (full feature set: viewBox, <G>, Path) and bake const arrays. */
function emitSvgStatic(el, scope, out, env) {
  const svgEl = jsxToSvgElement(el, scope);
  const { ops, paints } = flattenSvg(svgEl.props);
  const v = `n${out.n++}`;
  const id = out.svgN++;
  const nPaints = paints.length / 7;
  if (ops.length) {
    out.vectorData.push(`static const float s_svg${id}_ops[] = {\n    ${Array.from(ops, floatLit).join(', ')}\n};`);
    out.vectorData.push(`static const ERVectorPaint s_svg${id}_paints[] = {\n${Array.from({ length: nPaints }, (_, i) => '    ' + emitVectorPaint(paints.slice(i * 7, i * 7 + 7))).join(',\n')}\n};`);
  }
  emitSvgBox(v, svgEl.props.width, svgEl.props.height, el.openingElement, scope, out, env);
  if (ops.length) out.build.push(`    er_node_set_vector_ops(${v}, s_svg${id}_ops, ${ops.length}, s_svg${id}_paints, ${nPaints});`);
  return v;
}

/** State-driven <Svg> (flat Arc/Circle/Rect/Line/static-Path; no viewBox/<G>): emit a mutable op-tape
 *  + build_svgN() that recomputes it from state, called at build and re-called on each app_update. */
function emitSvgDynamic(el, scope, out, env, state) {
  const svgA = svgAttrs(el.openingElement, scope, env);
  if (svgA.viewBox != null) throw new Error('AOT: a viewBox on a state-driven <Svg> is not yet supported — size shapes in the width/height space');
  const entries = [];
  const specs = [];
  for (const c of el.children) {
    if (c.type !== 'JSXElement') continue;
    const type = c.openingElement.name.name;
    const fn = SHAPE_ENTRIES[type];
    if (!fn) throw new Error(`AOT: <${type}> is not a supported shape in a state-driven <Svg> (no <G>/viewBox yet)`);
    const a = svgAttrs(c.openingElement, scope, env);
    const shape = fn(a);
    if (!shape.length) continue;
    entries.push('ER_VOP_SHAPE', floatLit(specs.length), ...shape);
    specs.push(paintSpec(a, env));
  }
  const v = `n${out.n++}`;
  const id = out.svgN++;
  const len = entries.length;
  const nPaints = specs.length;
  const dynPaint = specs.some((p) => p.anyDynamic);
  out.needsMath = true; // build_svg uses cosf/sinf/M_PI for arcs
  out.vectorData.push(`static float s_svg${id}_ops[${len}];`);
  // Dynamic paint → a MUTABLE paint table (re)filled by build_svg from state each update; else a const table.
  if (dynPaint) out.vectorData.push(`static ERVectorPaint s_svg${id}_paints[${nPaints}];`);
  else out.vectorData.push(`static const ERVectorPaint s_svg${id}_paints[] = {\n${specs.map((p) => '    ' + paintInitFromSpec(p)).join(',\n')}\n};`);
  const builderLines = entries.map((e, i) => `    s_svg${id}_ops[${i}] = ${e};`);
  if (dynPaint) specs.forEach((ps, pi) => ps.fields.forEach((f, fi) => builderLines.push(`    s_svg${id}_paints[${pi}].${PAINT_FIELDS[fi]} = ${f};`)));
  out.vectorBuilders.push(`static void build_svg${id}(void)\n{\n${builderLines.join('\n')}\n}`);

  emitSvgBox(v, svgA.width, svgA.height, el.openingElement, scope, out, env);
  out.build.push(`    build_svg${id}();`, `    er_node_set_vector_ops(${v}, s_svg${id}_ops, ${len}, s_svg${id}_paints, ${nPaints});`, `    s_${v} = ${v};`);
  out.handles.push(v);
  out.svgUpdates.push({ id, len, nPaints, nodeVar: `s_${v}` });
  return v;
}

// ---------------------------------------------------------------------------------------------------
// Node emitters — typed components. Each maps one JSX element to its engine node + props: the shared
// helpers (emitRefBind, compileValueHandler) then Switch / TextInput / ActivityIndicator / Modal /
// FlatList. The generic host node (View/Text/Pressable/Image/ScrollView) + the element dispatcher live
// in the next section (emitNodeImpl).
// ---------------------------------------------------------------------------------------------------

/** Captures `ref={r}` (r a node ref) by storing the freshly-created node handle into the ref's slot. */
function emitRefBind(v, openingElement, out, env) {
  for (const attr of openingElement.attributes) {
    if (attr.type !== 'JSXAttribute' || attr.name.name !== 'ref') continue;
    const e = attr.value?.type === 'JSXExpressionContainer' ? attr.value.expression : null;
    if (e?.type === 'Identifier' && env.refs?.get(e.name)?.kind === 'node') out.build.push(`    ${env.refs.get(e.name).cVar} = ${v};`);
    else throw new Error('AOT: ref={…} must reference a node ref declared with useRef()');
  }
}

/** Compiles a value-callback (e.g. Switch onValueChange) — binds its first param to `valueCode`, not an event. */
function compileValueHandler(fnNode, valueCode, env, state, out, cType = 'int') {
  const param = fnNode.params[0]?.type === 'Identifier' ? fnNode.params[0].name : null;
  const locals = new Map(env.locals);
  if (param) locals.set(param, { code: valueCode, cType });
  const ctx = { stateChanged: false, animIdx: 0, out };
  const body = fnNode.body;
  const list = body.type === 'BlockStatement' ? body.body : [{ type: 'ExpressionStatement', expression: body }];
  const stmts = compileStmts(list, { ...env, locals }, state, ctx, '    ');
  if (ctx.stateChanged) stmts.push('    app_update();');
  return stmts;
}

/**
 * <Switch value={on} onValueChange={(v) => setOn(v)} trackColor={{false,true}} thumbColor=… style=… />
 * → ER_NODE_SWITCH. The engine flips its own value on press (+ animates the thumb) then fires ER_EVENT_PRESS,
 * so onValueChange maps to PRESS and its `v` param is the TOGGLED value (!value). `value` drives switch_value
 * (state → dynamic). Default RN 51×31 box (the renderer scales the track/thumb to it); style can override.
 */
function emitSwitch(el, scope, out, env, state) {
  const v = `n${out.n++}`;
  const { staticAssigns, dynAssigns } = collectStyleAssigns(el.openingElement, scope, env);
  const hasField = (f) => staticAssigns.some((a) => a.field === f) || dynAssigns.some((a) => a.field === f);
  if (!hasField('width')) staticAssigns.push({ field: 'width', expr: '51' });
  if (!hasField('height')) staticAssigns.push({ field: 'height', expr: '31' });

  let valueNode = null;
  let onChangeFn = null;
  for (const attr of el.openingElement.attributes) {
    if (attr.type !== 'JSXAttribute') throw aotError('AOT: spread props on <Switch> are not supported');
    const name = attr.name.name;
    if (name === 'style' || name === 'ref' || name === 'key') continue;
    const node = attrExpr(attr);
    if (name === 'value') valueNode = node;
    else if (name === 'onValueChange') onChangeFn = node;
    else if (name === 'thumbColor') staticAssigns.push({ field: 'thumb_color', expr: colorLiteral(String(evalStatic(node, scope))) });
    else if (name === 'trackColor') {
      const tc = evalStatic(node, scope);
      if (tc?.false != null) staticAssigns.push({ field: 'track_color_false', expr: colorLiteral(String(tc.false)) });
      if (tc?.true != null) staticAssigns.push({ field: 'track_color_true', expr: colorLiteral(String(tc.true)) });
    } else if (name === 'disabled') {
      /* accepted; the AOT has no disabled-visual yet, so it is a no-op */
    } else throw aotError(`AOT: <Switch> prop "${name}" is not supported`, 'supported props: value, onValueChange, trackColor, thumbColor, style.');
  }

  // value → switch_value (static or, when state-driven, recomputed in app_update).
  if (valueNode) {
    try {
      staticAssigns.push({ field: 'switch_value', expr: evalStatic(valueNode, scope) ? '1' : '0' });
    } catch {
      dynAssigns.push({ field: 'switch_value', code: `(uint8_t)((${emitExpr(valueNode, env).code}) ? 1 : 0)` });
    }
  }

  const isDynamic = dynAssigns.length > 0;
  out.build.push(`    ${v} = er_node_create(ER_NODE_SWITCH);`);
  if (isDynamic) {
    out.build.push(`    s_${v} = ${v};`);
    out.handles.push(v);
    out.updates.push({ v, styleAssigns: staticAssigns, text: null, dynAssigns });
  } else {
    out.build.push(`    er_props_default(&p);`);
    for (const a of staticAssigns) out.build.push(`    p.${a.field} = ${a.expr};`);
    out.build.push(`    er_node_set_props(${v}, &p);`);
  }

  if (onChangeFn) {
    if (!isFn(onChangeFn)) throw aotError('AOT: onValueChange must be an inline function', 'onValueChange={(v) => setX(v)}');
    if (!valueNode) throw aotError('AOT: a <Switch> with onValueChange needs a value prop', 'controlled switch: <Switch value={on} onValueChange={(v) => setOn(v)} />');
    const handlerName = `er_handler_${out.handlers.length}`;
    const toggled = `(!(${emitExpr(valueNode, env).code}))`; // the engine toggles on press → param is !value
    out.handlers.push({ name: handlerName, body: compileValueHandler(onChangeFn, toggled, env, state, out) });
    out.build.push(`    er_event_set(${v}, ER_EVENT_PRESS, ${handlerName}, NULL);`);
  }
  emitRefBind(v, el.openingElement, out, env);
  return v;
}

/**
 * <TextInput value={text} onChangeText={(t) => setText(t)} placeholder="…" placeholderTextColor=… style=… />
 * → ER_NODE_TEXT_INPUT. The engine auto-focuses on tap (hit_test) and edits its own buffer, firing
 * ER_EVENT_CHANGE_TEXT with the new text — bound to the handler's param via `data->changed_text` (a string).
 * `value` drives the text buffer (er_node_set_props → er_text_input_set_text; state → dynamic, re-synced in
 * app_update; set_text is a no-op when unchanged, so a controlled input is safe). Desktop types via the
 * keyboard; the touch-only CYD needs an on-screen keyboard to enter text (deferred follow-on).
 */
function emitTextInput(el, scope, out, env, state) {
  const v = `n${out.n++}`;
  const { staticAssigns, dynAssigns } = collectStyleAssigns(el.openingElement, scope, env);
  let valueNode = null;
  let onChangeFn = null;
  let placeholder = null;
  for (const attr of el.openingElement.attributes) {
    if (attr.type !== 'JSXAttribute') throw aotError('AOT: spread props on <TextInput> are not supported');
    const name = attr.name.name;
    if (name === 'style' || name === 'ref' || name === 'key') continue;
    const node = attrExpr(attr);
    if (name === 'value' || name === 'defaultValue') valueNode = node;
    else if (name === 'onChangeText') onChangeFn = node;
    else if (name === 'placeholder') placeholder = String(evalStatic(node, scope));
    else if (name === 'placeholderTextColor') staticAssigns.push({ field: 'placeholder_color', expr: colorLiteral(String(evalStatic(node, scope))) });
    else if (name === 'cursorColor') staticAssigns.push({ field: 'cursor_color', expr: colorLiteral(String(evalStatic(node, scope))) });
    else if (name === 'editable') {
      try {
        staticAssigns.push({ field: 'editable', expr: evalStatic(node, scope) ? '1' : '0' });
      } catch {
        dynAssigns.push({ field: 'editable', code: `(uint8_t)((${emitExpr(node, env).code}) ? 1 : 0)` });
      }
    } else if (['autoFocus', 'keyboardType', 'secureTextEntry', 'maxLength', 'multiline', 'autoCapitalize', 'autoCorrect', 'returnKeyType', 'onSubmitEditing', 'onFocus', 'onBlur'].includes(name)) {
      /* accepted but not yet lowered (no on-screen keyboard / submit wiring in the AOT path) */
    } else throw aotError(`AOT: <TextInput> prop "${name}" is not supported`, 'supported props: value, onChangeText, placeholder, placeholderTextColor, cursorColor, editable, style.');
  }

  // value → the input's text buffer (er_node_set_props → er_text_input_set_text): static literal, or a
  // state-driven value re-synced each app_update.
  let text = null;
  if (valueNode) {
    try {
      const cv = evalStatic(valueNode, scope);
      text = { dynamic: false, format: (cv == null ? '' : String(cv)).replace(/%/g, '%%'), args: [] };
    } catch {
      const e = emitExpr(valueNode, env);
      text = { dynamic: true, format: printfSpec(e.cType), args: [e.code] };
    }
  }

  const isDynamic = dynAssigns.length > 0 || (text && text.dynamic);
  out.build.push(`    ${v} = er_node_create(ER_NODE_TEXT_INPUT);`);
  if (isDynamic) {
    out.build.push(`    s_${v} = ${v};`);
    out.handles.push(v);
    out.updates.push({ v, styleAssigns: staticAssigns, text, dynAssigns, placeholder });
  } else {
    out.build.push(`    er_props_default(&p);`);
    for (const a of staticAssigns) out.build.push(`    p.${a.field} = ${a.expr};`);
    if (placeholder != null) out.build.push(`    snprintf(p.placeholder, sizeof(p.placeholder), "%s", ${cstr(placeholder)});`);
    if (text) out.build.push(`    snprintf(p.text, sizeof(p.text), "%s", ${cstr(text.format.replace(/%%/g, '%'))});`);
    out.build.push(`    er_node_set_props(${v}, &p);`);
  }

  if (onChangeFn) {
    if (!isFn(onChangeFn)) throw aotError('AOT: onChangeText must be an inline function', 'onChangeText={(t) => setText(t)}');
    const handlerName = `er_handler_${out.handlers.length}`;
    out.handlers.push({ name: handlerName, body: compileValueHandler(onChangeFn, 'data->changed_text', env, state, out, 'string') });
    out.build.push(`    er_event_set(${v}, ER_EVENT_CHANGE_TEXT, ${handlerName}, NULL);`);
  }
  emitRefBind(v, el.openingElement, out, env);
  return v;
}

/**
 * <ActivityIndicator color={…} size="small"|"large"|N animating={…} style={…} /> → ER_NODE_ACTIVITY_INDICATOR.
 * The engine spins it on its own (a looping rotate; render is a ring of 8 fading dots). No intrinsic size, so
 * a default box is set from `size` (small=20, large=36) unless style sets width/height.
 */
function emitActivityIndicator(el, scope, out, env) {
  const v = `n${out.n++}`;
  const { staticAssigns, dynAssigns } = collectStyleAssigns(el.openingElement, scope, env);
  const hasField = (f) => staticAssigns.some((a) => a.field === f) || dynAssigns.some((a) => a.field === f);
  let size = 36;
  for (const attr of el.openingElement.attributes) {
    if (attr.type !== 'JSXAttribute') throw aotError('AOT: spread props on <ActivityIndicator> are not supported');
    const name = attr.name.name;
    if (name === 'style' || name === 'ref' || name === 'key') continue;
    const node = attrExpr(attr);
    if (name === 'color') {
      try {
        staticAssigns.push({ field: 'indicator_color', expr: colorLiteral(String(evalStatic(node, scope))) });
      } catch {
        dynAssigns.push({ field: 'indicator_color', code: emitColorExpr(node, env) });
      }
    } else if (name === 'size') {
      const sv = evalStatic(node, scope);
      size = sv === 'small' ? 20 : sv === 'large' ? 36 : Number(sv) || 36;
    } else if (name === 'animating') {
      try {
        staticAssigns.push({ field: 'animating', expr: evalStatic(node, scope) ? '1' : '0' });
      } catch {
        dynAssigns.push({ field: 'animating', code: `(uint8_t)((${emitExpr(node, env).code}) ? 1 : 0)` });
      }
    } else throw aotError(`AOT: <ActivityIndicator> prop "${name}" is not supported`, 'supported props: color, size, animating, style.');
  }
  if (!hasField('width')) staticAssigns.push({ field: 'width', expr: String(size) });
  if (!hasField('height')) staticAssigns.push({ field: 'height', expr: String(size) });

  const isDynamic = dynAssigns.length > 0;
  out.build.push(`    ${v} = er_node_create(ER_NODE_ACTIVITY_INDICATOR);`);
  if (isDynamic) {
    out.build.push(`    s_${v} = ${v};`);
    out.handles.push(v);
    out.updates.push({ v, styleAssigns: staticAssigns, text: null, dynAssigns });
  } else {
    out.build.push(`    er_props_default(&p);`);
    for (const a of staticAssigns) out.build.push(`    p.${a.field} = ${a.expr};`);
    out.build.push(`    er_node_set_props(${v}, &p);`);
  }
  emitRefBind(v, el.openingElement, out, env);
  return v;
}

/**
 * <Modal visible={show} backdropColor=… style=…>{content}</Modal> → ER_NODE_MODAL. The engine draws a
 * full-screen backdrop then the modal + its children when visible, and toggles the node's layout display
 * from `visible`. Defaults to an absolute full-screen overlay centring its content (style can override).
 * transparent / animationType / onRequestClose are accepted but currently no-ops.
 */
function emitModal(el, scope, out, env, state) {
  const v = `n${out.n++}`;
  const { staticAssigns, dynAssigns } = collectStyleAssigns(el.openingElement, scope, env);
  const hasField = (f) => staticAssigns.some((a) => a.field === f) || dynAssigns.some((a) => a.field === f);
  // Overlay defaults: absolute, fill the parent via four 0 insets (the robust "stretch" for an absolute
  // node), centre the content. The user's style overrides any of these.
  const DEFAULTS = [
    ['position', 'ER_POS_ABSOLUTE'],
    ['left', '0'],
    ['top', '0'],
    ['right', '0'],
    ['bottom', '0'],
    ['align_items', 'ER_ALIGN_CENTER'],
    ['justify_content', 'ER_JUSTIFY_CENTER'],
  ];
  for (const [f, expr] of DEFAULTS) if (!hasField(f)) staticAssigns.push({ field: f, expr });

  let visibleNode = null;
  for (const attr of el.openingElement.attributes) {
    if (attr.type !== 'JSXAttribute') throw aotError('AOT: spread props on <Modal> are not supported');
    const name = attr.name.name;
    if (name === 'style' || name === 'ref' || name === 'key') continue;
    const node = attrExpr(attr);
    if (name === 'visible') visibleNode = node;
    else if (name === 'backdropColor') staticAssigns.push({ field: 'backdrop_color', expr: colorLiteral(String(evalStatic(node, scope))) });
    else if (name === 'transparent' || name === 'animationType' || name === 'onRequestClose' || name === 'statusBarTranslucent') {
      /* accepted for RN compatibility; no-op in the AOT today */
    } else throw aotError(`AOT: <Modal> prop "${name}" is not supported`, 'supported: visible, backdropColor, style, children (transparent / animationType / onRequestClose are accepted but no-ops).');
  }
  if (!visibleNode) throw aotError('AOT: a <Modal> needs a visible prop', '<Modal visible={show}>…</Modal>');
  try {
    staticAssigns.push({ field: 'modal_visible', expr: evalStatic(visibleNode, scope) ? '1' : '0' });
  } catch {
    dynAssigns.push({ field: 'modal_visible', code: `(uint8_t)((${emitExpr(visibleNode, env).code}) ? 1 : 0)` });
  }

  const isDynamic = dynAssigns.length > 0;
  out.build.push(`    ${v} = er_node_create(ER_NODE_MODAL);`);
  if (isDynamic) {
    out.build.push(`    s_${v} = ${v};`);
    out.handles.push(v);
    out.updates.push({ v, styleAssigns: staticAssigns, text: null, dynAssigns });
  } else {
    out.build.push(`    er_props_default(&p);`);
    for (const a of staticAssigns) out.build.push(`    p.${a.field} = ${a.expr};`);
    out.build.push(`    er_node_set_props(${v}, &p);`);
  }
  emitRefBind(v, el.openingElement, out, env);
  emitChildren(el.children, v, scope, out, env, state); // the modal's content (shown/hidden with the modal)
  return v;
}

/**
 * <FlatList data={items} renderItem={({ item, index }) => <Row …/>} keyExtractor=… style=… /> → the SAME as
 * <ScrollView style=…>{items.map((item, index) => <Row …/>)}</ScrollView>. The engine's FlatList IS a
 * ScrollView (no virtualization), and the AOT already unrolls a .map (static or state-list), so this is a thin
 * API-compat rewrite: synthesize that ScrollView+map AST and emit it. keyExtractor is ignored (no reconciler).
 */
function emitFlatList(el, scope, out, env, state, opts) {
  let dataNode = null;
  let renderItem = null;
  let styleAttr = null;
  for (const attr of el.openingElement.attributes) {
    if (attr.type !== 'JSXAttribute') throw aotError('AOT: spread props on <FlatList> are not supported');
    const name = attr.name.name;
    if (name === 'data') dataNode = attrExpr(attr);
    else if (name === 'renderItem') renderItem = attrExpr(attr);
    else if (name === 'style') styleAttr = attr;
    else if (name === 'keyExtractor' || name === 'ref' || name === 'key') {
      /* ignored — the AOT unrolls at compile time, so React keys are irrelevant */
    } else throw aotError(`AOT: <FlatList> prop "${name}" is not supported`, 'supported: data, renderItem, keyExtractor, style. For headers/footers/horizontal/onEndReached etc., use <ScrollView> + .map directly.');
  }
  if (!dataNode) throw aotError('AOT: <FlatList> needs a data prop', '<FlatList data={items} renderItem={({ item }) => <Row item={item} />} />');
  if (!renderItem || !isFn(renderItem)) throw aotError('AOT: <FlatList> needs a renderItem function', 'renderItem={({ item, index }) => <Row item={item} />}');
  const param = renderItem.params[0];
  if (!param || param.type !== 'ObjectPattern') throw aotError('AOT: FlatList renderItem must destructure ({ item, index })', 'renderItem={({ item }) => <Row item={item} />}');
  let itemName = null;
  let indexName = null;
  for (const prop of param.properties) {
    if (prop.type !== 'ObjectProperty' || prop.value.type !== 'Identifier') throw aotError('AOT: FlatList renderItem may destructure only item / index (to plain names)');
    if (prop.key.name === 'item') itemName = prop.value.name;
    else if (prop.key.name === 'index') indexName = prop.value.name;
    else throw aotError(`AOT: FlatList renderItem cannot destructure "${prop.key.name}" (only item / index)`);
  }
  if (!itemName) throw aotError('AOT: FlatList renderItem must destructure item', 'renderItem={({ item }) => …}');

  // Rewrite renderItem `({ item, index }) => BODY` → a positional `.map` callback `(item, index) => BODY`.
  const cbParams = [{ type: 'Identifier', name: itemName }];
  if (indexName) cbParams.push({ type: 'Identifier', name: indexName });
  const cb = { type: 'ArrowFunctionExpression', params: cbParams, body: renderItem.body, async: false, expression: renderItem.body.type !== 'BlockStatement' };
  const mapCall = { type: 'CallExpression', callee: { type: 'MemberExpression', object: dataNode, property: { type: 'Identifier', name: 'map' }, computed: false }, arguments: [cb] };
  const scrollView = {
    type: 'JSXElement',
    openingElement: { type: 'JSXOpeningElement', name: { type: 'JSXIdentifier', name: 'ScrollView' }, attributes: styleAttr ? [styleAttr] : [], selfClosing: false },
    closingElement: { type: 'JSXClosingElement', name: { type: 'JSXIdentifier', name: 'ScrollView' } },
    children: [{ type: 'JSXExpressionContainer', expression: mapCall }],
  };
  return emitNode(scrollView, scope, out, env, state, opts);
}

const RESIZE_MODES = {
  cover: 'ER_RESIZE_COVER',
  contain: 'ER_RESIZE_CONTAIN',
  stretch: 'ER_RESIZE_STRETCH',
  repeat: 'ER_RESIZE_REPEAT',
  center: 'ER_RESIZE_CENTER',
};

/** Resolves an <Image source>/imageName expression to its baked asset NAME (a string), or null if it can't
 *  be resolved at compile time (a runtime/dynamic source). Records the import actually used in out.images
 *  (name → importPath) so the CLI bakes only what the app references. */
function imageNameFromSource(expr, env, out) {
  if (!expr) return null;
  if (expr.type === 'StringLiteral') return expr.value; // source="wx_sun" / imageName="wx_sun"
  if (expr.type === 'Identifier') {
    const imp = env.imageImports?.get(expr.name); // import wxSun from './wx_sun.png'
    if (imp) {
      out.images.set(imp.name, imp.importPath);
      return imp.name;
    }
    const c = env.consts?.[expr.name]; // const NAME = 'wx_sun'
    return typeof c === 'string' ? c : null;
  }
  if (expr.type === 'ObjectExpression') {
    // source={{ uri: 'wx_sun' }} — RN's remote-image shape; here the uri IS the baked asset name.
    const uri = expr.properties.find((p) => p.type === 'ObjectProperty' && (p.key.name === 'uri' || p.key.value === 'uri'));
    if (uri?.value?.type === 'StringLiteral') return uri.value.value;
  }
  return null;
}

/** Resolves an <Image>'s source/imageName/resizeMode/tintColor to static C values for the node props. */
function resolveImageAttrs(el, env, out) {
  const attrs = el.openingElement.attributes;
  const find = (n) => attrs.find((a) => a.type === 'JSXAttribute' && a.name.name === n);
  const imAttr = find('imageName');
  const srcAttr = find('source');
  const srcExpr = imAttr ? attrExpr(imAttr) : srcAttr ? attrExpr(srcAttr) : null;
  let imageName = null;
  if (srcExpr) {
    imageName = imageNameFromSource(srcExpr, env, out);
    if (imageName == null) {
      const e = aotError(
        'AOT: could not resolve <Image source> to a baked asset',
        "an <Image>'s source must be a compile-time asset: `import logo from './logo.png'` then source={logo}, source=\"logo\", or source={{ uri: 'logo' }}. A runtime/state-driven source (e.g. a list-item field) isn't supported yet — split it into static <Image>s or a compile-time conditional.",
      );
      if (srcExpr.loc) e.aotLoc = srcExpr.loc.start;
      throw e;
    }
  }
  let resizeMode = null;
  const rmAttr = find('resizeMode');
  if (rmAttr) {
    const rm = evalStaticOr(attrExpr(rmAttr), env, null);
    resizeMode = RESIZE_MODES[rm];
    if (!resizeMode) {
      const e = aotError(`AOT: unsupported <Image resizeMode> "${rm}"`, `resizeMode must be one of: ${Object.keys(RESIZE_MODES).join(' / ')}.`);
      if (rmAttr.loc) e.aotLoc = rmAttr.loc.start;
      throw e;
    }
  }
  let tintColor = null;
  const tcAttr = find('tintColor');
  if (tcAttr) {
    const tc = evalStaticOr(attrExpr(tcAttr), env, null);
    if (typeof tc === 'string' || typeof tc === 'number') tintColor = '0x' + (parseColor(String(tc)) >>> 0).toString(16).padStart(8, '0').toUpperCase() + 'u';
  }
  return { imageName, resizeMode, tintColor };
}

// ---------------------------------------------------------------------------------------------------
// Node emitter — the element dispatcher + the generic host node. emitNodeImpl is the entry point for
// every JSX element: it routes Svg / typed components / FlatList / components to their emitters above,
// and handles the generic host nodes (View / Text / Pressable / Image / ScrollView) itself — style,
// text, events, refs, children. emitNode wraps it with withLoc so a thrown AOT error gets a location.
// ---------------------------------------------------------------------------------------------------

function emitNodeImpl(el, scope, out, env, state, opts = {}) {
  const tag = resolveTag(el.openingElement);
  if (tag === 'Svg') return emitSvg(el, scope, out, env, state, opts);
  if (tag === 'Switch') return emitSwitch(el, scope, out, env, state);
  if (tag === 'TextInput') return emitTextInput(el, scope, out, env, state);
  if (tag === 'ActivityIndicator') return emitActivityIndicator(el, scope, out, env);
  if (tag === 'Modal') return emitModal(el, scope, out, env, state);
  if (tag === 'FlatList') return emitFlatList(el, scope, out, env, state, opts);
  const nodeType = NODE_TYPES[tag];
  if (!nodeType) {
    if (out.components.has(tag)) return emitComponent(el, scope, out, env, state, opts);
    throw aotError(`AOT: unknown element <${tag}> (not a built-in or a component in this file)`, `<${tag}> must be a built-in (View / Text / Pressable / Image / ScrollView / Svg + shapes / Animated.*) or a function component defined in THIS file. Check the import/spelling, or define the component here.`);
  }

  // Spread attributes on a host element (`<View {...props} />`) aren't lowered — the style/event loops
  // below only read named JSXAttributes, so a spread would be SILENTLY dropped. Reject it explicitly
  // (the typed components — Switch/TextInput/Modal/… — already throw on spreads). Pin the location to the
  // spread itself, not the whole element, for a precise code-frame.
  const spread = el.openingElement.attributes.find((a) => a.type === 'JSXSpreadAttribute');
  if (spread) {
    const e = aotError(
      `AOT: a spread {...} on <${tag}> is not supported`,
      `list each prop explicitly (e.g. style={…} onPress={…}). Spread props are only supported on a function component instance whose spread object folds to a compile-time constant.`,
    );
    if (spread.loc) e.aotLoc = spread.loc.start;
    throw e;
  }

  const v = `n${out.n++}`;
  const { staticAssigns, dynAssigns, binds } = collectStyleAssigns(el.openingElement, scope, env);
  // A <Text> with a nested <Text> becomes inline SPANS; otherwise a single (possibly dynamic) string.
  const spans = tag === 'Text' ? collectTextSpans(el.children, scope, env) : null;
  const text = tag === 'Text' && !spans ? buildText(el.children, scope, env) : null;
  // An <Image>'s baked-asset name + resize/tint (resolved at compile time; the source is a compile-time import).
  const image = tag === 'Image' ? resolveImageAttrs(el, env, out) : null;

  // `displayCode` toggles show/hide for a state-driven conditional: the node is always built, its
  // `display` flips between flex and none in app_update (joining any state-driven style assigns).
  if (opts.displayCode) dynAssigns.push({ field: 'display', code: `((${opts.displayCode}) ? ER_DISPLAY_FLEX : ER_DISPLAY_NONE)` });

  const isDynamic = !!text?.dynamic || dynAssigns.length > 0;

  // An <Image>'s asset/resize/tint are static (resolved from a compile-time import); a state-driven
  // (dynamic-styled) <Image> would take the deferred-props path below, which doesn't carry them.
  if (image && isDynamic) {
    throw aotError(
      'AOT: a state-driven <Image> (dynamic styles) is not supported yet',
      'give the <Image> static styles; only its layout box can currently be animated (via an animated transform/opacity bind), not state-driven style props.',
    );
  }

  out.build.push(`    ${v} = er_node_create(${nodeType});`);
  if (isDynamic) {
    // Props are (re)applied in app_update(); here just create the node and remember its handle.
    out.build.push(`    s_${v} = ${v};`);
    out.handles.push(v);
    out.updates.push({ v, styleAssigns: staticAssigns, text, dynAssigns });
  } else {
    out.build.push(`    er_props_default(&p);`);
    for (const a of staticAssigns) out.build.push(`    p.${a.field} = ${a.expr};`);
    if (text) out.build.push(`    snprintf(p.text, sizeof(p.text), "%s", ${cstr(text.format.replace(/%%/g, '%'))});`);
    if (image?.imageName != null) out.build.push(`    snprintf(p.image_name, sizeof(p.image_name), "%s", ${cstr(image.imageName)});`);
    if (image?.resizeMode) out.build.push(`    p.resize_mode = ${image.resizeMode};`);
    if (image?.tintColor) out.build.push(`    p.tint_color = ${image.tintColor};`);
    out.build.push(`    er_node_set_props(${v}, &p);`);
  }

  // Inline text spans (a <Text> with nested <Text>): set once; each span inherits the node's base style
  // unless it overrides (the local array is copied by er_node_set_text_spans).
  if (spans) {
    out.build.push(`    {`, `        static const ERTextSpan spans_${v}[] = {`);
    for (const s of spans) out.build.push(`            { ${s.text}, ${s.color}, ${s.font_size}, ${s.font_weight}, ${s.font_style}, ${s.text_decoration}, ${s.letter_spacing} },`);
    out.build.push(`        };`, `        er_node_set_text_spans(${v}, spans_${v}, ${spans.length});`, `    }`);
  }

  // Animated style props (opacity / transform / color) → bind the node to its animated value. The
  // engine's native driver advances it each tick (no per-frame JS, no app_update for the motion). A bind
  // carrying an `interp` maps the raw value through a piecewise-linear range first (value.interpolate(...)).
  binds.forEach((b, i) => {
    if (b.interp) {
      const it = b.interp;
      out.build.push(
        `    {`,
        `        static const ERInterpolation interp_${v}_${i} = { { ${it.input.map(floatLit).join(', ')} }, { ${it.output.map(floatLit).join(', ')} }, ${it.input.length}, ${it.exLeft}, ${it.exRight} };`,
        `        er_anim_value_bind_interpolated(${b.cVar}, ${v}, ${b.prop}, &interp_${v}_${i});`,
        `    }`,
      );
    } else {
      out.build.push(`    er_anim_value_bind(${b.cVar}, ${v}, ${b.prop});`);
    }
  });

  emitRefBind(v, el.openingElement, out, env);

  for (const attr of el.openingElement.attributes) {
    if (attr.type !== 'JSXAttribute') continue;
    const evt = EVENT_TYPES[attr.name.name];
    if (!evt) continue;
    const fn = attrExpr(attr);
    let handlerName;
    if (fn.type === 'Identifier' && env.callbacks?.has(fn.name)) {
      // onPress={fn} where fn is a useCallback → emit one shared handler, reused across elements.
      handlerName = out.cbEmitted.get(fn.name);
      if (!handlerName) {
        handlerName = `er_cb_${fn.name}`;
        out.cbEmitted.set(fn.name, handlerName);
        out.handlers.push({ name: handlerName, body: compileHandler(env.callbacks.get(fn.name), env, state, out) });
      }
    } else if (fn.type === 'Identifier' && env.fnProps?.has(fn.name)) {
      // Callback prop: <Child onTap={() => …}/> where Child does onPress={onTap}. Inline the CALLER's
      // function as this handler, compiled in the caller's env/state so its setters/locals resolve there.
      const fp = env.fnProps.get(fn.name);
      handlerName = `er_handler_${out.handlers.length}`;
      out.handlers.push({ name: handlerName, body: compileHandler(fp.node, fp.env, fp.state, out) });
    } else if (isFn(fn)) {
      handlerName = `er_handler_${out.handlers.length}`;
      out.handlers.push({ name: handlerName, body: compileHandler(fn, env, state, out) });
    } else {
      throw aotError(`AOT: ${attr.name.name} must be an inline function, a useCallback, or a callback prop`, `pass an inline arrow (onPress={() => setX(…)}), a useCallback identifier, or a function prop received by this component.`);
    }
    out.build.push(`    er_event_set(${v}, ${evt}, ${handlerName}, NULL);`);
  }

  if (tag !== 'Text') emitChildren(el.children, v, scope, out, env, state);
  return v;
}
const emitNode = withLoc(emitNodeImpl);

// ---------------------------------------------------------------------------------------------------
// On-screen keyboard config — lower a module-level setKeyboardConfig({...}) call to static C tables
// (ERKeyboardKey/Row/Layer + ERKeyboardConfig) that er_app_build hands to er_keyboard_set_config.
// ---------------------------------------------------------------------------------------------------

/** Formats a color value (string like "#303030" or a number) as a C ARGB literal; null/undefined → "0". */
function kbdColor(v) {
  if (v == null) return '0';
  const argb = parseColor(String(v)) >>> 0;
  return '0x' + argb.toString(16).padStart(8, '0').toUpperCase() + 'u';
}

/** One JS keyboard key object → a C ERKeyboardKey initializer. `li` is its layer index (for shift highlight).
 *  Shapes: { char } (types it), { char:' ', span } (space), { label, layer } (switch), { label, backspace },
 *  { label, done }; optional span / highlight. */
function kbdKeyToC(k, li) {
  if (k == null || typeof k !== 'object') throw aotError('AOT: each setKeyboardConfig key must be an object', 'e.g. { char: "q" } or { label: "shift", layer: 1, highlight: true }');
  let type;
  let label;
  let text = 'NULL';
  let layer = 0;
  if (k.backspace) {
    type = 'ER_KBD_KEY_BACKSPACE';
    label = k.label ?? '<';
  } else if (k.done) {
    type = 'ER_KBD_KEY_DONE';
    label = k.label ?? 'OK';
  } else if (k.layer != null) {
    type = 'ER_KBD_KEY_LAYER';
    label = k.label ?? '';
    layer = Math.round(Number(k.layer));
  } else if (k.char != null) {
    type = 'ER_KBD_KEY_CHAR';
    text = cstr(String(k.char));
    label = k.label ?? (String(k.char) === ' ' ? '' : String(k.char)); // a space bar shows no label
  } else {
    throw aotError('AOT: a setKeyboardConfig key needs one of char / layer / backspace / done');
  }
  const span = k.span != null ? Math.round(Number(k.span)) : 1;
  const hl = k.highlight ? li : 255;
  return `{ ${label === '' ? 'NULL' : cstr(String(label))}, ${text}, ${type}, ${layer}, ${span}, ${hl} }`;
}

/** Lowers a module-level `setKeyboardConfig({...})` to a static ERKeyboardConfig + an er_keyboard_set_config()
 *  call in er_app_build — customising the on-screen keyboard (colours/sizes, and optionally a full layout) from
 *  the app, no engine edit. The config must be statically foldable; omit `layers` to keep the built-in QWERTY. */
function compileKeyboardConfig(program, out) {
  let arg = null;
  for (const stmt of program.body) {
    if (stmt.type === 'ExpressionStatement' && stmt.expression.type === 'CallExpression' && stmt.expression.callee.type === 'Identifier' && stmt.expression.callee.name === 'setKeyboardConfig') {
      arg = stmt.expression.arguments[0];
      break;
    }
  }
  if (!arg) return;
  let cfg;
  try {
    cfg = evalStatic(arg, {});
  } catch {
    throw aotError('AOT: setKeyboardConfig(...) needs a statically-foldable config object', 'pass an object literal of colours/sizes (+ an optional `layers` array) — no state or runtime values.');
  }
  if (cfg == null || typeof cfg !== 'object') throw aotError('AOT: setKeyboardConfig(...) needs a config object');

  const data = [];
  let layersExpr = 'NULL';
  let layerCount = 0;
  if (Array.isArray(cfg.layers)) {
    const layerVars = [];
    cfg.layers.forEach((layer, li) => {
      if (!Array.isArray(layer)) throw aotError('AOT: setKeyboardConfig `layers[i]` must be an array of rows');
      const rowVars = [];
      layer.forEach((row, ri) => {
        if (!Array.isArray(row) || !row.length) throw aotError('AOT: each keyboard row must be a non-empty array of keys');
        data.push(`static const ERKeyboardKey kbd_l${li}r${ri}[] = { ${row.map((k) => kbdKeyToC(k, li)).join(', ')} };`);
        rowVars.push(`{ kbd_l${li}r${ri}, ${row.length} }`);
      });
      data.push(`static const ERKeyboardRow kbd_l${li}rows[] = { ${rowVars.join(', ')} };`);
      layerVars.push(`{ kbd_l${li}rows, ${layer.length} }`);
    });
    data.push(`static const ERKeyboardLayer kbd_layers[] = { ${layerVars.join(', ')} };`);
    layersExpr = 'kbd_layers';
    layerCount = cfg.layers.length;
  }
  const num = (v) => (v == null ? 0 : Math.round(Number(v)));
  data.push(
    `static const ERKeyboardConfig kbd_cfg = { ${layersExpr}, ${layerCount}, ${num(cfg.gridCols)}, ${num(cfg.rowHeight)}, ` +
      `${num(cfg.keyGap)}, ${num(cfg.keyRadius)}, ${num(cfg.fontSize)}, ${kbdColor(cfg.panelColor)}, ${kbdColor(cfg.keyColor)}, ` +
      `${kbdColor(cfg.keyActiveColor)}, ${kbdColor(cfg.labelColor)} };`,
  );
  out.kbdData = data.join('\n');
  out.kbdSetup = '    er_keyboard_set_config(&kbd_cfg);';
}

// ---------------------------------------------------------------------------------------------------
// Compile orchestration — JSX source string → generated C. Pure (no I/O) so it can be unit-tested
// directly; the CLI entry at the bottom of the file wraps it with the file read/write.
// ---------------------------------------------------------------------------------------------------

/**
 * Compiles a Flow B app's JSX source to C.
 * @param {string} src   The App.jsx source text.
 * @param {string} demo  Demo name (only used in the generated-by header comment).
 * @returns {{c: string, h: string, nodes: number, state: number, handlers: number, updates: number}}
 */
function compileSourceImpl(src, demo = 'app', opts = {}) {
const ast = parse(src, { sourceType: 'module', plugins: ['jsx'] });

const screen = opts.screen ?? { width: SCREEN_W, height: SCREEN_H };
const scope = moduleScope(ast.program, screen);
const component = findComponent(ast.program);
// Fold statically-derived component-local consts (e.g. `const compact = screen.width < 400`) into the
// const scope, so responsive `if` branches and styles can switch on them at compile time. Dynamic consts
// (state-derived, useMemo, etc.) throw here and are skipped — they're handled later by memos/emitExpr.
for (const stmt of component.body.body) {
  if (stmt.type !== 'VariableDeclaration' || stmt.kind !== 'const') continue;
  for (const decl of stmt.declarations) {
    if (decl.id.type !== 'Identifier' || !decl.init || decl.id.name in scope) continue;
    try {
      scope[decl.id.name] = evalStatic(decl.init, scope);
    } catch {
      /* dynamic const — resolved later */
    }
  }
}
const state = collectState(component.body, scope);
const rootJSX = findReturnJSX(component.body, scope);

const anims = collectAnims(component.body, scope);
const refs = collectRefs(component.body, scope);
const callbacks = collectCallbacks(component.body);
const memos = collectMemos(component.body);
const imageImports = collectImageImports(ast.program);
const env = { state: state.byName, locals: new Map(), consts: scope, anims, refs, callbacks, imageImports };
// Resolve memos in declaration order: constant-fold into the const scope when possible, else register a
// derived C expression in locals so each reference inlines it (the AOT has no per-render cache — the dep
// tracking re-applies dependent nodes anyway). Done before emit so references resolve.
for (const [name, expr] of memos) {
  try {
    scope[name] = evalStatic(expr, scope);
  } catch {
    const e = emitExpr(expr, env);
    env.locals.set(name, { code: `(${e.code})`, cType: e.cType });
  }
}
const out = { n: 0, build: [], handlers: [], updates: [], handles: [], components: collectComponents(ast.program), cbEmitted: new Map(), vectorData: [], vectorBuilders: [], svgUpdates: [], svgN: 0, needsMath: false, timerFns: [], usesTimers: false, mountEffects: [], animCbs: [], seqN: 0, kbdData: '', kbdSetup: '', images: new Map() };
compileKeyboardConfig(ast.program, out); // module-level setKeyboardConfig({...}) → static ERKeyboardConfig
const appTop = emitNode(rootJSX, scope, out, env, state);

// Mount effects: each `useEffect(fn, [])` body runs ONCE after the initial app_update (typically to start a
// host-tick timer). Compiled after emit so out.timerFns/usesTimers also reflect timers created in handlers.
for (const eff of collectEffects(component.body)) {
  if (!eff.deps || eff.deps.type !== 'ArrayExpression' || eff.deps.elements.length !== 0) {
    throw aotError('AOT: only useEffect(fn, []) — run once on mount — is supported for now', 'use an empty deps array []; dependency-driven effects (re-run when a value changes) are not yet supported.');
  }
  const b = eff.fn.body;
  const stmts = b.type === 'BlockStatement' ? b.body : [{ type: 'ExpressionStatement', expression: b }];
  const ctx = { stateChanged: false, animIdx: 0, out, allowReturn: true };
  const lines = compileStmts(stmts, env, state, ctx, '    ');
  if (ctx.stateChanged) lines.push('    app_update();');
  out.mountEffects.push(...lines);
}

const nodeDecls = Array.from({ length: out.n }, (_, i) => `n${i}`);
const stateRecords = [...state.byName.values()];
const scalarRecords = stateRecords.filter((s) => s.kind === 'scalar');
const listRecords = stateRecords.filter((s) => s.kind === 'list');

// Scalar state → one ErAppState struct. List state → a fixed-capacity struct array + a count each.
const fieldCDecl = (f) => (f.kind === 'string' ? `    char ${f.key}[${LIST_STR_CAP}];` : `    ${f.kind} ${f.key};`);
const itemInit = (item, struct) => `{ ${struct.fields.map((f) => (f.kind === 'string' ? cstr(String(item[f.key] ?? '')) : f.kind === 'float' ? `${Number(item[f.key]) || 0}f` : String(Math.round(Number(item[f.key]) || 0)))).join(', ')} }`;
const listBlocks = listRecords
  .map(
    (s) =>
      `typedef struct\n{\n${s.struct.fields.map(fieldCDecl).join('\n')}\n} ${s.cTypeName};\n\n` +
      `static ${s.cTypeName} ${s.arrayName}[${s.cap}] = {${s.items.map((it) => '\n    ' + itemInit(it, s.struct)).join(',')}\n};\n` +
      `static int ${s.countMember} = ${s.items.length};\n`,
  )
  .join('\n');
const scalarFieldDecl = (s) => (s.cType === 'string' ? `    char ${s.name}[${LIST_STR_CAP}];` : `    ${s.cType === 'float' ? 'float' : 'int'} ${s.name};`);
const scalarBlock = scalarRecords.length
  ? `typedef struct\n{\n${scalarRecords.map(scalarFieldDecl).join('\n')}\n} ErAppState;\n\nstatic ErAppState s_state = {${scalarRecords.map((s) => ` .${s.name} = ${s.initCode}`).join(',')} };\n`
  : '';
const stateBlock = [scalarBlock, listBlocks].filter(Boolean).join('\n');

const handleDecls = out.handles.map((v) => `static ERNode* s_${v};`).join('\n');

// Value refs — a plain mutable static each (escape-hatch state that does not trigger a re-render).
const refDecls = [...refs.values()].map((r) => `static ${r.cType} ${r.cVar} = ${r.initCode};`).join('\n');

// Baked vector op-tapes + paint tables (static <Svg> geometry), emitted at file scope.
const vectorBlock = out.vectorData.join('\n\n');
// build_svgN() recompute functions (state-driven Svgs) — declared before app_update, which calls them.
const vectorBuilderBlock = out.vectorBuilders.join('\n\n');

// Animated values — one engine-side handle each, created at the top of er_app_build (binds reference them).
const animList = [...anims.values()];
const animDecls = animList.map((a) => `static ERAnimValueHandle ${a.cVar};`).join('\n');
const animCreate = animList.map((a) => `    ${a.cVar} = er_anim_value_create(${a.initCode});`).join('\n');

const hasUpdate = out.updates.length > 0 || out.svgUpdates.length > 0;
const updateBlock = (() => {
  if (!hasUpdate) return '';
  const lines = ['static void app_update(void)', '{'];
  if (out.updates.length) lines.push('    ERProps p;');
  for (const u of out.updates) {
    lines.push(`    er_props_default(&p);`);
    for (const a of u.styleAssigns) lines.push(`    p.${a.field} = ${a.expr};`);
    for (const a of u.dynAssigns) lines.push(`    p.${a.field} = ${a.code};`);
    if (u.placeholder != null) lines.push(`    snprintf(p.placeholder, sizeof(p.placeholder), "%s", ${cstr(u.placeholder)});`);
    if (u.text) {
      if (u.text.args.length) lines.push(`    snprintf(p.text, sizeof(p.text), ${cstr(u.text.format)}, ${u.text.args.join(', ')});`);
      else lines.push(`    snprintf(p.text, sizeof(p.text), "%s", ${cstr(u.text.format.replace(/%%/g, '%'))});`);
    }
    lines.push(`    er_node_set_props(s_${u.v}, &p);`);
  }
  // State-driven Svgs: recompute the op-tape from state and re-upload.
  for (const s of out.svgUpdates) {
    lines.push(`    build_svg${s.id}();`);
    lines.push(`    er_node_set_vector_ops(${s.nodeVar}, s_svg${s.id}_ops, ${s.len}, s_svg${s.id}_paints, ${s.nPaints});`);
  }
  lines.push('}');
  return lines.join('\n');
})();

const handlerDefs = out.handlers
  .map((h) => `static void ${h.name}(ERNode* node, const EREventData* data, void* user_data)\n{\n    (void)node;\n    (void)data;\n    (void)user_data;\n${h.body.join('\n')}\n}`)
  .join('\n\n');

// setInterval/setTimeout → a small fixed timer table advanced by er_app_tick(dt) (the host calls it each
// frame). The table + helpers are emitted only when timers are used; er_app_tick is always defined (a no-op
// otherwise) so a host can call it unconditionally. Timer callbacks become parameterless C functions.
const timerTableBlock = out.usesTimers
  ? `#include <stdbool.h>

#ifndef ER_AOT_MAX_TIMERS
#define ER_AOT_MAX_TIMERS 8
#endif

typedef struct
{
    int interval_ms;
    int remaining_ms;
    bool repeat;
    bool active;
    void (*fn)(void);
} ErTimer;
static ErTimer s_timers[ER_AOT_MAX_TIMERS];

static int er_timer_add(int ms, bool repeat, void (*fn)(void))
{
    for (int i = 0; i < ER_AOT_MAX_TIMERS; i++)
    {
        if (!s_timers[i].active)
        {
            s_timers[i].interval_ms = ms < 1 ? 1 : ms;
            s_timers[i].remaining_ms = s_timers[i].interval_ms;
            s_timers[i].repeat = repeat;
            s_timers[i].active = true;
            s_timers[i].fn = fn;
            return i;
        }
    }
    return -1; /* table full (raise ER_AOT_MAX_TIMERS) */
}

static void er_timer_clear(int id)
{
    if (id >= 0 && id < ER_AOT_MAX_TIMERS)
    {
        s_timers[id].active = false;
    }
}`
  : '';

const timerFnDefs = out.timerFns.map((t) => `static void ${t.name}(void)\n{\n${t.body.join('\n')}\n}`).join('\n\n');

// Animated.sequence on_complete callbacks: each starts the next step when the previous finishes. Forward-
// declared (the handler that starts step 0 references the first callback, and each callback the next).
const animCbDecls = out.animCbs.map((cb) => `static void ${cb.name}(bool finished, void* user_data);`).join('\n');
const animCbDefs = out.animCbs
  .map((cb) => `static void ${cb.name}(bool finished, void* user_data)\n{\n    (void)finished;\n    (void)user_data;\n${cb.body.join('\n')}\n}`)
  .join('\n\n');

const appTickFn = out.usesTimers
  ? `void er_app_tick(int dt_ms)
{
    for (int i = 0; i < ER_AOT_MAX_TIMERS; i++)
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
  : `void er_app_tick(int dt_ms)\n{\n    (void)dt_ms;\n}`;

const mountEffectsBlock = out.mountEffects.length ? '\n    /* useEffect(fn, []) — run once on mount. */\n' + out.mountEffects.join('\n') + '\n' : '';

// <math.h> when any libm symbol appears (Svg arc trig, or Math.* in expressions/handlers/timer callbacks).
const usesMath = out.needsMath || /\b(sinf|cosf|tanf|sqrtf|fabsf|roundf|floorf|ceilf|fminf|fmaxf|atan2f|powf|M_PI)\b/.test([stateBlock, refDecls, vectorBuilderBlock, updateBlock, handlerDefs, animCbDefs, timerFnDefs, out.mountEffects.join('\n'), out.build.join('\n')].join('\n'));
const body = `/*
 * Generated by the embedded-react Flow B AOT compiler (npm run aot -- ${demo}). DO NOT EDIT.
 * Builds the app's scene graph + state machine directly against er_scene.h — no QuickJS, no JS runtime.
 */
#include "app.gen.h"

#include "er_scene.h"

#include <stdio.h>
#include <string.h>
${usesMath ? '#include <math.h>\n' : ''}${stateBlock ? '\n' + stateBlock : ''}${refDecls ? '\n' + refDecls + '\n' : ''}${vectorBlock ? '\n' + vectorBlock + '\n' : ''}${vectorBuilderBlock ? '\n' + vectorBuilderBlock + '\n' : ''}${animDecls ? '\n' + animDecls + '\n' : ''}${handleDecls ? '\n' + handleDecls + '\n' : ''}${timerTableBlock ? '\n' + timerTableBlock + '\n' : ''}${updateBlock ? '\n' + updateBlock + '\n' : ''}${animCbDecls ? '\n' + animCbDecls + '\n' : ''}${handlerDefs ? '\n' + handlerDefs + '\n' : ''}${animCbDefs ? '\n' + animCbDefs + '\n' : ''}${timerFnDefs ? '\n' + timerFnDefs + '\n' : ''}${out.kbdData ? '\n' + out.kbdData + '\n' : ''}
${appTickFn}

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
    er_tree_append_child(root, ${appTop});
    er_tree_set_root(root);
${out.kbdSetup ? out.kbdSetup + ' /* app-supplied on-screen keyboard layout/appearance */\n' : ''}${hasUpdate ? '\n    app_update(); /* apply initial state-dependent props */\n' : ''}${mountEffectsBlock}}
`;

const header = `/* Generated by the embedded-react Flow B AOT compiler. DO NOT EDIT. */
#ifndef ER_APP_GEN_H
#define ER_APP_GEN_H

/** @brief Builds the AOT-compiled app's scene graph + state machine (call once after backend init). */
void er_app_build(int screen_w, int screen_h);

/** @brief Advances app timers (setInterval/setTimeout). Call once per frame with the elapsed ms; a no-op
 *         for apps that use no timers, so it is always safe to call. */
void er_app_tick(int dt_ms);

#endif
`;

  // images: the baked-image imports the app actually references (name + source-relative path) — the CLI
  // resolves each path against the demo dir and bakes them into assets.generated.c (er_register_assets).
  const images = [...out.images.entries()].map(([name, importPath]) => ({ name, importPath }));
  return { c: body, h: header, nodes: out.n, state: stateRecords.length, handlers: out.handlers.length, updates: out.updates.length, images };
}

/** Public entry: compile JSX source → { c, h, ... }. On an AOT error, annotate it with file:line:col + a
 *  source code-frame (+ hint) so the failure points at the exact unsupported construct. */
export function compileSource(src, demo = 'app', opts = {}) {
  try {
    return compileSourceImpl(src, demo, opts);
  } catch (e) {
    if (e && typeof e.message === 'string' && e.message.startsWith('AOT:')) {
      throw formatAotError(e, src, opts.filename || `demos/${demo}/App.jsx`);
    }
    throw e;
  }
}

// ---------------------------------------------------------------------------------------------------
// CLI entry — `node aot/compile.mjs [demo]`: read a demo's App.jsx, write dist/app.gen.{c,h}. Runs only
// when this file is invoked directly (not when imported by the test harness).
// ---------------------------------------------------------------------------------------------------
if (process.argv[1] && resolve(process.argv[1]) === resolve(fileURLToPath(import.meta.url))) {
  const demo = process.argv[2] || process.env.DEMO || 'thermostat';
  const appPath = resolve(demosDir, demo, 'App.jsx');
  if (!existsSync(appPath)) {
    const avail = existsSync(demosDir) ? readdirSync(demosDir, { withFileTypes: true }).filter((d) => d.isDirectory()).map((d) => d.name) : [];
    console.error(`AOT: demo "${demo}" not found (expected ${appPath}). Available: ${avail.join(', ') || '(none)'}`);
    process.exit(1);
  }
  let result;
  try {
    result = compileSource(readFileSync(appPath, 'utf8'), demo, { filename: resolve(demosDir, demo, 'App.jsx') });
  } catch (e) {
    // A located AOT error already reads as "<reason>\n  at file:line:col\n\n<frame>\n\nhint: ..."; print it
    // cleanly (no JS stack) so the developer sees exactly the unsupported construct.
    console.error(e && e.aotLoc ? e.message : e?.message || String(e));
    process.exit(1);
  }
  mkdirSync(distDir, { recursive: true });
  writeFileSync(resolve(distDir, 'app.gen.c'), result.c);
  writeFileSync(resolve(distDir, 'app.gen.h'), result.h);

  // Bake the images the app imports into dist/assets.generated.{c,h} (er_register_assets) — the SAME baker
  // Flow A uses. Always written (even with 0 images → a no-op register fn) so the AOT host can always
  // compile + call it. Each importPath is source-relative to the demo's App.jsx.
  const imageJobs = result.images.map((im) => ({ name: im.name, path: resolve(demosDir, demo, im.importPath) }));
  for (const j of imageJobs) {
    if (!existsSync(j.path)) {
      console.error(`AOT: <Image> asset "${j.name}" not found at ${j.path}`);
      process.exit(1);
    }
  }
  const baked = bakeAssets({ images: imageJobs, fonts: [], outDir: distDir });
  console.log(
    `AOT: compiled demo "${demo}" -> dist/app.gen.c (${result.nodes} nodes, ${result.state} state, ` +
      `${result.handlers} handler(s), ${result.updates} dynamic) + ${baked.images} image(s) -> dist/assets.generated.c`,
  );
}
