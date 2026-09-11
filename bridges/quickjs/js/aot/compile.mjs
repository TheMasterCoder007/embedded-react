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

// `npm run aot [demo]` — the Flow B ahead-of-time compiler (vertical slice).
//
// Compiles a demo's JSX straight to C against er_scene.h: no QuickJS, no JS at runtime. The generated
// app.gen.c builds the engine node tree directly and wires state + events, so it fits an MCU with only
// internal RAM.
//
// Supported subset (grows demo by demo; unsupported syntax throws "AOT: ..."):
//   - View / Text / Pressable / TouchableOpacity / Image / ScrollView elements
//   - StyleSheet + inline styles → ERProps (static values)
//   - text with literal + {interpolation} segments (interpolations may reference state)
//   - useState(initial) → C state; on* handlers (onPress/onPressIn/onPressOut/onLongPress) → C functions
//   - setState(value) and setState(prev => expr); a small C expression subset (literals, identifiers,
//     +-*/% , comparisons, ?:)
//
// The compiler tracks which nodes depend on which state, so a state change re-sets ONLY the dependent
// nodes (er_node_set_props) — no diffing, no reconciler. See the root README (Flow B).
//
//   npm run aot                      # default demo (thermostat) — but use a minimal demo for the slice
//   npm run aot -- watch-face        # a specific demo by folder name
import {parse} from '@babel/parser';
import {traverse} from '@babel/core';
import {codeFrameColumns} from '@babel/code-frame';
import {
  readFileSync,
  writeFileSync,
  mkdirSync,
  existsSync,
  readdirSync,
} from 'node:fs';
import {resolve, dirname} from 'node:path';
import {fileURLToPath} from 'node:url';
import {
  lowerStyle,
  isStyleKey,
  STYLE_KEYS,
  NODE_TYPES,
  DYN_FIELDS,
  colorLiteral,
} from './style-map.mjs';
import {
  flattenSvg,
  parseColor,
  parsePath,
  KAPPA,
  PAINT_STRIDE,
  GRAD_MAX_STOPS,
  scaleVectorArtifact,
} from '../src/embedded-react/svg-ops.js';
import {bakeAssets} from '../assets/index.mjs';
import {warnMissingGlyphs} from '../assets/glyph-coverage.mjs';
import {analyzeFontSizes, warnFontSizes} from '../assets/font-sizes.mjs';

const here = dirname(fileURLToPath(import.meta.url)); // bridges/quickjs/js/aot
const repoRoot = resolve(here, '../../../..');
const demosDir = resolve(repoRoot, 'demos');
const distDir = resolve(here, '..', 'dist');

// The compiler's own version (kept in lockstep with the engine via tools/sync-version.mjs). Stamped into the
// generated app + asserted against the engine's er_version.h so a version mismatch fails at COMPILE time.
const PKG_VERSION = JSON.parse(
  readFileSync(resolve(here, '..', 'package.json'), 'utf8'),
).version;
const [PKG_MAJOR, PKG_MINOR] = PKG_VERSION.split('.');

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

/**
 * The `#ifndef` marker a board example guards on, from a demo name. Every character outside [A-Za-z0-9]
 * becomes `_<hex code point>_`, so the result is a C identifier AND the mapping is one-to-one: `foo-bar`
 * is ER_AOT_DEMO_foo_2d_bar and `foo_bar` is ER_AOT_DEMO_foo_5f_bar. Flattening both to `foo_bar` would
 * let a board guard accept the wrong app, from either entry point that generates one.
 */
export const demoMarker = demo =>
  `ER_AOT_DEMO_${demo.replace(/[^A-Za-z0-9]/gu, ch => `_${ch.codePointAt(0).toString(16)}_`)}`;

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
      if (
        e &&
        typeof e.message === 'string' &&
        e.message.startsWith('AOT:') &&
        !e.aotLoc &&
        node &&
        node.loc
      ) {
        e.aotLoc = node.loc.start; // babel loc: { line (1-based), column (0-based) }
      }
      throw e;
    }
  };
}

/** Re-throws an AOT error with `file:line:col`, a code-frame, any hint, and the screen size this build
 *  folded its layout at — all folded into the message. */
function formatAotError(e, src, filename) {
  if (!e || !e.aotLoc) return e; // nothing to locate — leave the bare message
  const {line, column} = e.aotLoc;
  const loc = {start: {line, column: column + 1}}; // code-frame columns are 1-based
  let frame = '';
  try {
    frame = codeFrameColumns(src, loc, {highlightCode: false});
  } catch {
    /* code-frame is best-effort */
  }
  const hint = e.aotHint ? `\n\nhint: ${e.aotHint}` : '';
  // A responsive app folds its layout from `screen`, so the size baked into THIS build decides which
  // branch the compiler walks. Compile at the wrong one and the error points at whatever unsupported
  // thing the unintended branch happens to hold, with nothing on screen tying it back to the size.
  const screen = SCREEN_FROM_ENV
    ? `\n\nscreen: ${SCREEN_W}×${SCREEN_H} (from ER_AOT_SCREEN_W/H).`
    : `\n\nscreen: ${SCREEN_W}×${SCREEN_H} — ER_AOT_SCREEN_W/H did not supply both dimensions, so the ` +
      `default filled in. A responsive app picks its layout from \`screen\`, so this may be compiling a ` +
      `branch meant for another board.`;
  const out = new Error(
    `${e.message}\n  at ${filename}:${line}:${column + 1}\n\n${frame}${hint}${screen}`,
  );
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
        case '+':
          return l + r;
        case '-':
          return l - r;
        case '*':
          return l * r;
        case '/':
          return l / r;
        case '%':
          return l % r;
        case '<':
          return l < r;
        case '>':
          return l > r;
        case '<=':
          return l <= r;
        case '>=':
          return l >= r;
        case '==':
        case '===':
          return l === r;
        case '!=':
        case '!==':
          return l !== r;
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
      return evalStatic(node.test, scope)
        ? evalStatic(node.consequent, scope)
        : evalStatic(node.alternate, scope);
    case 'Identifier':
      if (node.name in scope) return scope[node.name];
      throw new Error(
        `AOT: cannot statically resolve identifier "${node.name}"`,
      );
    case 'MemberExpression': {
      const obj = evalStatic(node.object, scope);
      const key = node.computed
        ? evalStatic(node.property, scope)
        : node.property.name;
      if (obj == null)
        throw new Error(`AOT: member access on null/undefined ("${key}")`);
      return obj[key];
    }
    case 'ObjectExpression': {
      const o = {};
      for (const prop of node.properties) {
        if (prop.type !== 'ObjectProperty')
          throw new Error(
            'AOT: object spreads/methods not supported in static objects',
          );
        const k = prop.computed
          ? evalStatic(prop.key, scope)
          : (prop.key.name ?? prop.key.value);
        o[k] = evalStatic(prop.value, scope);
      }
      return o;
    }
    case 'ArrayExpression':
      return node.elements.map(e => (e ? evalStatic(e, scope) : null));
    case 'CallExpression': {
      const c = node.callee;
      if (
        c.type === 'MemberExpression' &&
        c.object.name === 'StyleSheet' &&
        c.property.name === 'create'
      ) {
        return evalStatic(node.arguments[0], scope);
      }
      throw new Error(`AOT: cannot statically evaluate call expression`);
    }
  }
  throw new Error(
    `AOT: unsupported expression "${node.type}" in static context`,
  );
}

// ---------------------------------------------------------------------------------------------------
// C expression emission — lowers a JS expression to C, given the current state + local bindings. Each
// result carries a C type so callers pick the right printf specifier / assignment.
//   env = { state: Map(name→record), locals: Map(name→{code,cType}), consts: scope object }
// ---------------------------------------------------------------------------------------------------
const ARITH = new Set(['+', '-', '*', '/', '%']);
const COMPARE = new Set(['<', '>', '<=', '>=', '==', '!=', '===', '!==']);

/**
 * `scope` minus every name `env` binds at RUNTIME — locals, state, refs, animated values, and the event /
 * gesture params of the handler being compiled. emitExpr resolves those before env.consts, so a fold
 * that consulted the raw scope would silently swap a module const in for the value the code actually
 * uses. Every env-driven constant fold goes through this; the JSX-side folds keep the same rule by
 * deleting a name from their scope copy when they bind it (see emitComponent / emitDynamicMap).
 */
function foldScope(env, scope) {
  const bound = new Set([
    ...(env.locals?.keys() ?? []),
    ...(env.state?.keys() ?? []),
    ...(env.refs?.keys() ?? []),
    ...(env.anims?.keys() ?? []),
  ]);
  if (env.event) bound.add(env.event);
  if (env.gesture) bound.add(env.gesture);
  let shadows = false;
  for (const k of bound)
    if (k in scope) {
      shadows = true;
      break;
    }
  if (!shadows) return scope;
  return Object.fromEntries(
    Object.entries(scope).filter(([k]) => !bound.has(k)),
  );
}

const withUndefined = scope => Object.assign(Object.create(scope), {undefined});

/**
 * `e` used as a C condition. JS treats a string as truthy when it is non-empty; a bare char[] in C tests
 * its ADDRESS, which is always true — and real GCC refuses that under -Werror=address.
 */
const asCond = e => (e.cType === 'string' ? `(${e.code}[0] != '\\0')` : e.code);

/** No state or ref slot widened to 64 bits — the collectors' default. */
const NO_WIDE = new Set();

/** A 64-bit timestamp met a float, which would round it. */
const mix64Error = () =>
  aotError(
    'AOT: a 64-bit time value cannot be mixed with a float',
    'Date.now() and performance.now() are whole milliseconds in a 64-bit integer, and a float would round them. Keep the arithmetic whole — e.g. Math.floor(ms / 1000), not ms * 0.001.',
  );

/** A 64-bit timestamp met a boolean in `&&` / `||` / `?:`, where JS would hand back the boolean itself. */
const bool64Error = what =>
  aotError(
    `AOT: ${what} cannot mix a boolean with a 64-bit time value`,
    'JS would give back the boolean itself (and false renders as no text), which a 64-bit integer cannot hold. Use a number on both sides, e.g. `on ? Date.now() : 0`.',
  );

/** Flow A's lite profile has Date.now() and no Date objects, and so does the AOT. */
const dateObjectError = () =>
  aotError(
    'AOT: Date objects are not supported — only Date.now()',
    'keep time as milliseconds from Date.now() and do the calendar math on the number.',
  );

/**
 * The `Date` / `performance` calls whose name is the app's own binding — a const, a param, an import —
 * rather than the global, so they are not the engine clock. Babel's scopes decide, as JS would.
 */
function findShadowedClockCalls(ast) {
  const calls = new WeakSet();
  traverse(ast, {
    'CallExpression|NewExpression'(path) {
      const c = path.node.callee;
      const id = c.type === 'MemberExpression' ? c.object : c;
      if (
        id.type === 'Identifier' &&
        (id.name === 'Date' || id.name === 'performance') &&
        path.scope.getBinding(id.name)
      )
        calls.add(path.node);
    },
  });
  return calls;
}

/** `node` as an integer compile-time constant, or null. */
function staticInt(node, env) {
  try {
    const v = evalStatic(node, foldScope(env, env.consts ?? {}));
    return Number.isInteger(v) ? v : null;
  } catch {
    return null;
  }
}

/**
 * The divisor of a 64-bit `%` or Math.floor(a / b), which has to be a nonzero constant: JS gives NaN or
 * Infinity for a zero divisor, which an integer cannot hold, and C would trap on it.
 */
function nonzeroDivisor(node, env) {
  const m = staticInt(node, env);
  if (m !== null && m !== 0) return m;
  const e = aotError(
    'AOT: a 64-bit time value can only be divided by a nonzero constant',
    'JS gives NaN or Infinity for a zero divisor, which an integer cannot hold, so the divisor must be known at compile time — e.g. `ms % 1000`, `Math.floor(ms / 60000)`. For a divisor from state, reduce the timestamp first: `(ms % 86400000) / period`.',
  );
  if (node.loc) e.aotLoc = node.loc.start;
  throw e;
}

/**
 * `+ - * %` with a 64-bit timestamp on either side, kept in whole milliseconds. `%` takes a nonzero constant,
 * and one that fits an int narrows the result back to int, so `ms % 1000` is an ordinary number again. `/` is
 * refused: JS would give a fraction, and the whole-number division is written Math.floor(a / b).
 */
function emitArith64(node, l, r, env) {
  if (l.cType === 'float' || r.cType === 'float') throw mix64Error();
  if (node.operator === '/')
    throw aotError(
      'AOT: `/` on a 64-bit time value is not supported',
      'JS division gives a fraction, which a 64-bit integer cannot hold. Use Math.floor(a / b) for whole units — e.g. Math.floor(ms / 1000) for seconds.',
    );
  if (node.operator === '%') {
    const m = nonzeroDivisor(node.right, env);
    if (Math.abs(m) <= 0x7fffffff)
      return {code: `((int)(${l.code} % ${r.code}))`, cType: 'int'};
  }
  return {code: `(${l.code} ${node.operator} ${r.code})`, cType: 'i64'};
}

/**
 * Math.floor / round / ceil over `a / b` with a 64-bit timestamp on either side. floor becomes an exact
 * integer floor division by a nonzero constant (C's `/` rounds toward zero, floor rounds down); round and
 * ceil are refused. Null when neither side is 64-bit, which leaves it to the float path.
 */
function emitFloorDiv64(fn, args, env) {
  const arg = args[0];
  if (
    (fn !== 'floor' && fn !== 'round' && fn !== 'ceil') ||
    args.length !== 1 ||
    arg.type !== 'BinaryExpression' ||
    arg.operator !== '/'
  )
    return null;
  const l = emitExprWide(arg.left, env);
  const r = emitExprWide(arg.right, env);
  if (l.cType !== 'i64' && r.cType !== 'i64') return null;
  if (l.cType === 'float' || r.cType === 'float') throw mix64Error();
  if (l.cType === 'string' || r.cType === 'string') return null;
  if (fn !== 'floor')
    throw aotError(
      `AOT: Math.${fn}(a / b) on a 64-bit time value is not supported`,
      'use Math.floor(a / b), which stays exact in whole milliseconds.',
    );
  nonzeroDivisor(arg.right, env);
  return {code: `app_floordiv64(${l.code}, ${r.code})`, cType: 'i64'};
}

/** Math.* over a 64-bit timestamp: only what stays exact in whole numbers. */
function emitMath64(fn, a) {
  if (a.some(x => x.cType === 'float')) throw mix64Error();
  if (a.every(x => x.cType === 'int' || x.cType === 'i64')) {
    // A timestamp is already whole, so rounding leaves it as it is.
    if ((fn === 'floor' || fn === 'round' || fn === 'ceil') && a.length === 1)
      return {code: a[0].code, cType: 'i64'};
    if (fn === 'abs' && a.length === 1)
      return {code: `app_abs64(${a[0].code})`, cType: 'i64'};
    if ((fn === 'min' || fn === 'max') && a.length === 2)
      return {code: `app_${fn}64(${a[0].code}, ${a[1].code})`, cType: 'i64'};
  }
  throw aotError(
    `AOT: Math.${fn}(...) on a 64-bit time value is not supported`,
    'Math.floor / round / ceil / abs / min / max keep a timestamp exact; for anything else, reduce it to a small number first, e.g. `ms % 1000`.',
  );
}

function emitExprImpl(node, env) {
  switch (node.type) {
    case 'NumericLiteral':
      return Number.isInteger(node.value)
        ? {code: String(node.value), cType: 'int'}
        : {code: `${node.value}f`, cType: 'float'};
    case 'StringLiteral':
      return {code: cstr(node.value), cType: 'string'};
    case 'BooleanLiteral':
      return {code: node.value ? '1' : '0', cType: 'int', isBool: true};
    case 'Identifier': {
      if (env.locals.has(node.name)) return env.locals.get(node.name);
      if (env.state.has(node.name)) {
        const s = env.state.get(node.name);
        if (s.kind === 'list')
          throw new Error(
            `AOT: a list state ("${node.name}") can only be used via .length or .map`,
          );
        return {code: s.cMember, cType: s.cType, isBool: s.isBool};
      }
      if (node.name in env.consts) {
        const v = env.consts[node.name];
        if (typeof v === 'number')
          return Number.isInteger(v)
            ? {code: String(v), cType: 'int'}
            : {code: `${v}f`, cType: 'float'};
        if (typeof v === 'string') return {code: cstr(v), cType: 'string'};
        if (typeof v === 'boolean')
          return {code: v ? '1' : '0', cType: 'int', isBool: true};
      }
      throw new Error(
        `AOT: cannot resolve identifier "${node.name}" in a dynamic expression`,
      );
    }
    case 'UnaryExpression': {
      const a = emitExprWide(node.argument, env);
      if (
        (node.operator === '-' || node.operator === '+') &&
        a.cType === 'string'
      )
        throw aotError(
          `AOT: unary "${node.operator}" on a string is not supported`,
          'JS would coerce the string to a number; C has no such coercion. Keep the operand numeric.',
        );
      // Parenthesize the operand so `-` on a negative literal emits `(-(-135))`, not `(--135)` (a decrement).
      if (
        node.operator === '-' ||
        node.operator === '+' ||
        node.operator === '!'
      )
        // Only `!` yields a boolean. Unary +/- are numeric coercions — `+flag` is 0 or 1 in JS, so the
        // operand's boolean-ness must not survive them.
        return {
          code: `(${node.operator}(${node.operator === '!' ? asCond(a) : a.code}))`,
          cType: node.operator === '!' ? 'int' : a.cType,
          isBool: node.operator === '!',
        };
      throw new Error(`AOT: unsupported unary operator "${node.operator}"`);
    }
    case 'BinaryExpression': {
      const l = emitExprWide(node.left, env);
      const r = emitExprWide(node.right, env);
      if (ARITH.has(node.operator)) {
        if (l.cType === 'string' || r.cType === 'string') {
          // `+` over a string builds text, which C cannot express as one value. Where the destination is
          // a char buffer the caller lowers it with emitFormat(); anywhere else there is nothing to lower
          // to.
          if (node.operator === '+')
            throw aotError(
              'AOT: string concatenation is not supported in this position',
              'a `+` chain over strings lowers to a printf format, which only works where the value lands in a text buffer: a <Text> body, a string useState setter, or a <TextInput value>.',
            );
          // JS would coerce the string to a number; C would do pointer arithmetic or refuse to compile.
          throw aotError(
            `AOT: "${node.operator}" on a string is not supported`,
            'keep both operands numeric — a string cannot be coerced to a number here.',
          );
        }
        if (l.cType === 'i64' || r.cType === 'i64')
          return emitArith64(node, l, r, env);
        if (node.operator === '/')
          return {
            code: `((float)(${l.code}) / (float)(${r.code}))`,
            cType: 'float',
          };
        const cType =
          l.cType === 'float' || r.cType === 'float' ? 'float' : 'int';
        return {code: `(${l.code} ${node.operator} ${r.code})`, cType};
      }
      if (COMPARE.has(node.operator)) {
        const op =
          node.operator === '==='
            ? '=='
            : node.operator === '!=='
              ? '!='
              : node.operator;
        if (l.cType === 'string' || r.cType === 'string') {
          if (l.cType !== r.cType) {
            // A strict comparison never coerces, so a string against a number is decided statically.
            if (node.operator === '===' || node.operator === '!==')
              return {
                code: node.operator === '===' ? '0' : '1',
                cType: 'int',
                isBool: true,
              };
            throw aotError(
              'AOT: a string cannot be compared with a number',
              `\`${node.operator}\` coerces the string to a number in JS, which C cannot reproduce. Compare like with like, or use === / !== (which never coerce).`,
            );
          }
          // Equality is byte-exact, which for UTF-8 is exactly JS string equality. Ordering is not: JS
          // orders by UTF-16 code unit and strcmp by UTF-8 byte, and the two disagree once a character
          // lies outside the Basic Multilingual Plane. Refuse rather than silently pick the other branch.
          // (A bare `<` on two char* would compare addresses — real GCC rejects that under -Werror=address.)
          if (op !== '==' && op !== '!=')
            throw aotError(
              `AOT: ordering strings with "${node.operator}" is not supported`,
              'JS orders strings by UTF-16 code unit but the device holds UTF-8, so the order can differ. Use === / !==, or compare numbers.',
            );
          return {
            code: `(strcmp(${l.code}, ${r.code}) ${op} 0)`,
            cType: 'int',
            isBool: true,
          };
        }
        // A boolean and a number share an int slot, so C would hold `true === 1`; a strict comparison never
        // coerces, so JS never does. Decided only when the other side is surely a number.
        if (
          (l.cType === 'i64' || r.cType === 'i64') &&
          (l.cType === 'float' || r.cType === 'float')
        )
          throw mix64Error();
        const surelyNum = (n, x) =>
          !x.isBool &&
          (x.cType === 'float' ||
            x.cType === 'i64' ||
            n.type === 'NumericLiteral' ||
            (n.type === 'UnaryExpression' &&
              n.argument.type === 'NumericLiteral') ||
            (n.type === 'Identifier' &&
              !env.locals.has(n.name) &&
              env.state.get(n.name)?.isBool === false));
        if (
          (node.operator === '===' || node.operator === '!==') &&
          ((l.isBool && surelyNum(node.right, r)) ||
            (r.isBool && surelyNum(node.left, l)))
        )
          return {
            code: node.operator === '===' ? '0' : '1',
            cType: 'int',
            isBool: true,
          };
        return {
          code: `(${l.code} ${op} ${r.code})`,
          cType: 'int',
          isBool: true,
        };
      }
      throw new Error(`AOT: unsupported binary operator "${node.operator}"`);
    }
    case 'LogicalExpression': {
      const op =
        node.operator === '&&' || node.operator === '||' ? node.operator : null;
      if (!op)
        throw new Error(`AOT: unsupported logical operator "${node.operator}"`);
      const l = emitExprWide(node.left, env);
      const r = emitExprWide(node.right, env);
      // JS gives back one of the operands, not true/false. The 0/1 below is only its truth, which would
      // store `1` in place of a timestamp — so with a 64-bit side, keep the operand's value.
      if (l.cType === 'i64' || r.cType === 'i64') {
        if (l.cType === 'float' || r.cType === 'float') throw mix64Error();
        if (l.cType === 'string' || r.cType === 'string')
          throw aotError(
            `AOT: "${op}" cannot mix a 64-bit time value with a string`,
            'keep both sides numbers, or write the branch out: {ms ? ms : 0}.',
          );
        if (l.isBool || r.isBool) throw bool64Error(`"${op}"`);
        // `l` is read twice, which is safe: AOT expressions have no side effects, and the engine clock only
        // moves in er_tick(), so a second Date.now() in the same expression reads the same value.
        return {
          code:
            op === '||'
              ? `(${l.code} ? ${l.code} : ${r.code})`
              : `(${l.code} ? ${r.code} : ${l.code})`,
          cType: 'i64',
        };
      }
      return {
        code: `(${asCond(l)} ${op} ${asCond(r)})`,
        cType: 'int',
        isBool: Boolean(l.isBool && r.isBool),
      };
    }
    case 'ConditionalExpression': {
      const t = emitExprWide(node.test, env);
      const c = emitExprWide(node.consequent, env);
      const a = emitExprWide(node.alternate, env);
      // `(ok ? 1 : "none")` is ill-typed C (int vs char*) and has no single printf spec either, so it
      // has to be refused here rather than handed to the host compiler.
      if ((c.cType === 'string') !== (a.cType === 'string'))
        throw aotError(
          'AOT: a ternary cannot mix a string branch with a numeric one',
          "both branches must be the same kind — quote the number to keep it text, e.g. {ok ? '1' : 'none'}.",
        );
      const either = k => c.cType === k || a.cType === k;
      if (either('i64') && either('float')) throw mix64Error();
      if (either('i64') && (c.isBool || a.isBool))
        throw bool64Error('a ternary');
      const cType = either('float')
        ? 'float'
        : either('i64')
          ? 'i64'
          : c.cType === a.cType
            ? c.cType
            : 'int';
      return {
        code: `(${asCond(t)} ? ${c.code} : ${a.code})`,
        cType,
        isBool: Boolean(c.isBool && a.isBool),
      };
    }
    case 'MemberExpression': {
      // Static fold: member access that resolves to a compile-time constant (e.g. a .map item's `.key`).
      try {
        const v = evalStatic(node, foldScope(env, env.consts ?? {}));
        if (typeof v === 'number')
          return Number.isInteger(v)
            ? {code: String(v), cType: 'int'}
            : {code: `${v}f`, cType: 'float'};
        if (typeof v === 'string') return {code: cstr(v), cType: 'string'};
        if (typeof v === 'boolean')
          return {code: v ? '1' : '0', cType: 'int', isBool: true};
      } catch {
        /* not static — fall through to the dynamic member forms below */
      }
      const obj = node.object;
      const prop = node.computed ? null : node.property.name;
      // `<list>.length` → the runtime count.
      if (
        obj.type === 'Identifier' &&
        env.state.get(obj.name)?.kind === 'list' &&
        prop === 'length'
      ) {
        return {code: env.state.get(obj.name).countMember, cType: 'int'};
      }
      // `<item>.field` where item is a struct local (a list row's bound element).
      if (
        obj.type === 'Identifier' &&
        env.locals.get(obj.name)?.struct &&
        prop
      ) {
        const f = env.locals
          .get(obj.name)
          .struct.fields.find(x => x.key === prop);
        if (!f) throw new Error(`AOT: unknown field "${prop}" on a list item`);
        return {
          code: `${env.locals.get(obj.name).code}.${f.key}`,
          cType: f.kind === 'string' ? 'string' : f.kind,
        };
      }
      // `<ref>.current` — a value ref's mutable C slot.
      if (
        obj.type === 'Identifier' &&
        env.refs?.has(obj.name) &&
        prop === 'current'
      ) {
        const r = env.refs.get(obj.name);
        r.used = true;
        return {code: r.cVar, cType: r.cType};
      }
      // `<event>.x / .y / .dx / .dy` — touch fields of the handler's EREventData.
      if (
        obj.type === 'Identifier' &&
        env.event === obj.name &&
        (prop === 'x' || prop === 'y' || prop === 'dx' || prop === 'dy')
      ) {
        return {code: `data->${prop}`, cType: 'int'};
      }
      // `<event>.vx / .vy` — how fast the finger was going (px/ms) at the last measured move. The engine
      // measures it; a Flow B handler could not, having neither a clock nor the previous point.
      if (
        obj.type === 'Identifier' &&
        env.event === obj.name &&
        (prop === 'vx' || prop === 'vy')
      ) {
        return {code: `data->${prop}`, cType: 'float'};
      }
      // `<gestureState>.…` — the second argument of a PanResponder callback (see emitPanResponder).
      if (obj.type === 'Identifier' && env.gesture === obj.name && prop) {
        return panGestureField(prop, env.pan);
      }
      // `<event>.layout.x / .y / .width / .height` — the onLayout rect (EREventData.layout_rect; ERRect uses w/h).
      if (
        obj.type === 'MemberExpression' &&
        !obj.computed &&
        obj.object.type === 'Identifier' &&
        env.event === obj.object.name &&
        obj.property.name === 'layout'
      ) {
        const RECT = {x: 'x', y: 'y', width: 'w', height: 'h'};
        const f = RECT[prop];
        if (!f)
          throw new Error(
            `AOT: unknown onLayout rect field "${prop}" (use x / y / width / height)`,
          );
        return {code: `data->layout_rect.${f}`, cType: 'int'};
      }
      if (obj.type === 'Identifier' && obj.name === 'Math' && prop === 'PI')
        return {code: '(float)M_PI', cType: 'float'};
      throw aotError(
        'AOT: unsupported member expression in a dynamic context',
        'in a handler or dynamic expression you can read state, `ref.current`, a `.map` item field, event fields (e.x / e.y / e.dx / e.dy / e.vx / e.vy / e.layout.*), and Math.PI — other member access must be a compile-time constant.',
      );
    }
    case 'CallExpression': {
      const c = node.callee;
      // Date.now() / performance.now() → the engine clock, as 64-bit whole milliseconds.
      if (
        c.type === 'MemberExpression' &&
        !c.computed &&
        c.object.type === 'Identifier' &&
        (c.object.name === 'Date' || c.object.name === 'performance') &&
        !env.shadowedClock?.has(node)
      ) {
        if (c.property.name === 'now')
          return {
            code:
              c.object.name === 'Date' ? 'app_date_now()' : 'app_perf_now()',
            cType: 'i64',
          };
        if (c.object.name === 'Date') throw dateObjectError();
      }
      if (
        c.type === 'Identifier' &&
        c.name === 'Date' &&
        !env.shadowedClock?.has(node)
      )
        throw dateObjectError();
      // Math.* helpers → libm (the generated C includes <math.h> when these appear).
      if (c.type === 'MemberExpression' && c.object.name === 'Math') {
        const fn = c.property.name;
        const floorDiv = emitFloorDiv64(fn, node.arguments, env);
        if (floorDiv) return floorDiv;
        const a = node.arguments.map(x => emitExprWide(x, env));
        if (a.some(x => x.cType === 'i64')) return emitMath64(fn, a);
        const UNARY = {
          sin: 'sinf',
          cos: 'cosf',
          tan: 'tanf',
          sqrt: 'sqrtf',
          abs: 'fabsf',
          round: 'roundf',
          floor: 'floorf',
          ceil: 'ceilf',
        };
        if (UNARY[fn] && a.length === 1) {
          const inner = `${UNARY[fn]}((float)(${a[0].code}))`;
          // round/floor/ceil yield a whole number — cast to int so %d / int assignments are correct.
          return fn === 'round' || fn === 'floor' || fn === 'ceil'
            ? {code: `((int)${inner})`, cType: 'int'}
            : {code: inner, cType: 'float'};
        }
        const BINARY = {
          min: 'fminf',
          max: 'fmaxf',
          atan2: 'atan2f',
          pow: 'powf',
        };
        if (BINARY[fn] && a.length === 2)
          return {
            code: `${BINARY[fn]}((float)(${a[0].code}), (float)(${a[1].code}))`,
            cType: 'float',
          };
        throw new Error(`AOT: unsupported Math.${fn}(...) (arity ${a.length})`);
      }
      throw new Error(
        'AOT: unsupported call expression in a dynamic expression',
      );
    }
    case 'NewExpression':
      if (
        node.callee.type === 'Identifier' &&
        node.callee.name === 'Date' &&
        !env.shadowedClock?.has(node)
      )
        throw dateObjectError();
      break;
  }
  throw new Error(
    `AOT: unsupported expression "${node.type}" in a dynamic context`,
  );
}
/** emitExpr for the destinations that can hold a 64-bit timestamp: state, refs, locals, text, conditions. */
const emitExprWide = withLoc(emitExprImpl);

/**
 * Lowers an expression for a destination that holds an int, a float or a string. C would narrow a 64-bit
 * timestamp (Date.now() / performance.now()) into one of those without a warning, so it is refused here;
 * the destinations that hold 64 bits call emitExprWide.
 */
function emitExpr(node, env) {
  const e = emitExprWide(node, env);
  if (e.cType !== 'i64') return e;
  const err = aotError(
    'AOT: a 64-bit time value (Date.now() / performance.now()) cannot be used here',
    'keep it in state, a ref or a local, compare it, or show it in text. Anywhere else, reduce it to a small number first — e.g. `ms % 1000`, or `Math.floor(ms / 1000) % 60`.',
  );
  if (node.loc) err.aotLoc = node.loc.start;
  throw err;
}

/** The C type a numeric state, ref or local slot of `cType` is declared with. */
function cScalarType(cType) {
  if (cType === 'int') return 'int';
  if (cType === 'float') return 'float';
  if (cType === 'i64') return 'int64_t';
  throw new Error(`AOT internal: no C scalar type for "${cType}"`);
}

const printfSpec = cType =>
  cType === 'string'
    ? '%s'
    : cType === 'float'
      ? '%g'
      : cType === 'i64'
        ? '%lld'
        : '%d';
/** An expression as a printf argument: `%lld` needs a long long, and int64_t is `long` on 64-bit Linux. */
const printfArg = e => (e.cType === 'i64' ? `(long long)(${e.code})` : e.code);

/** C source with its string and char literals and its comments blanked, so a scan for a call sees only code. */
const stripCLiterals = c =>
  c.replace(
    /"(?:[^"\\\n]|\\.)*"|'(?:[^'\\\n]|\\.)*'|\/\*[\s\S]*?\*\/|\/\/[^\n]*/g,
    ' ',
  );
const cTypeOfValue = v =>
  typeof v === 'string'
    ? 'string'
    : typeof v === 'number' && !Number.isInteger(v)
      ? 'float'
      : 'int';

/** An aotError pinned to the expression that cannot be lowered to text (concatParts is not withLoc-wrapped). */
function textShapeError(node, message, hint) {
  const e = aotError(message, hint);
  if (node.loc) e.aotLoc = node.loc.start;
  return e;
}

/**
 * Text a constant renders as a standalone JSX child. React draws nothing for null, undefined or a
 * boolean (see flattenTextChildren in Flow A), which is not how `+` treats the same values — a
 * concatenation operand goes through String() instead.
 */
const jsxChildText = v =>
  v === undefined || v === null || typeof v === 'boolean' ? '' : String(v);

/**
 * Splits a string-building `+` chain into printf parts, following JS's own left-to-right typing: a `+`
 * is a concatenation only once one of its sides is a string, so `n + 1 + ' ms'` still adds before it
 * appends. Parts are either a `literal` (folded into the format) or a `{spec, code}` pair (a runtime arg).
 *
 * @returns {{isString: boolean, parts: Array<{literal?: string, spec?: string, code?: string}>}}
 */
function concatParts(node, env, scope, operand = false) {
  // The global `undefined` (the name is reserved — see normalizeUndefined) renders as nothing for a
  // standalone child and as "undefined" when concatenated. It is handled here, the one place it has a
  // text meaning, rather than in the shared constant fold, where every prop reader would inherit it.
  if (node.type === 'Identifier' && node.name === 'undefined')
    return {isString: false, parts: [{literal: operand ? 'undefined' : ''}]};
  try {
    const v = evalStatic(node, scope);
    return {
      isString: typeof v === 'string',
      parts: [{literal: operand ? String(v) : jsxChildText(v)}],
    };
  } catch {
    /* not a compile-time constant — split it below */
  }
  if (node.type === 'BinaryExpression' && node.operator === '+') {
    const l = concatParts(node.left, env, scope, true);
    const r = concatParts(node.right, env, scope, true);
    if (l.isString || r.isString)
      return {isString: true, parts: [...l.parts, ...r.parts]};
  }
  // A branch or operand that itself concatenates (`on ? 'a' + s : 'b'`, `ok && 'n=' + n`) would need a
  // format of its own, and one snprintf has one format. Say that here, before emitExpr's generic "not
  // supported in this position" fires — its hint that text buffers are fine is exactly wrong here.
  const concatenates = n =>
    n.type === 'BinaryExpression' &&
    n.operator === '+' &&
    concatParts(n, env, scope, true).isString;
  if (
    node.type === 'ConditionalExpression' &&
    (concatenates(node.test) ||
      concatenates(node.consequent) ||
      concatenates(node.alternate))
  )
    throw textShapeError(
      node,
      'AOT: a ternary in text cannot concatenate inside its test or a branch',
      "each branch must be a single value — a literal, a state, or a number. Build the joined string first (a string useState set from a handler), or split it: {on ? 'a' : 'b'}{on ? s : ''}.",
    );
  if (
    node.type === 'LogicalExpression' &&
    (concatenates(node.left) || concatenates(node.right))
  )
    throw textShapeError(
      node,
      `AOT: "${node.operator}" in text cannot concatenate inside an operand`,
      "write it with plain values — {ok ? 'n=' : ''}{ok ? n : ''} — or build the joined string first in a string useState.",
    );
  const e = emitExprWide(node, env);
  // JS `&&`/`||` evaluate to one of their OPERANDS, and a ternary to one of its BRANCHES. The generated C
  // collapses a logical to 0/1 and gives a ternary a single slot, so text can only reproduce JS when the
  // operands agree in kind. Refuse rather than print a value the app never computed.
  if (node.type === 'LogicalExpression' && !e.isBool)
    throw textShapeError(
      node,
      `AOT: "${node.operator}" in text evaluates to one of its operands, not to true/false`,
      `\`a ${node.operator} b\` is a or b unless both sides are already booleans — the generated C only has 0/1. Write the branch out instead: {cond ? 'yes' : ''}.`,
    );
  if (
    node.type === 'ConditionalExpression' &&
    Boolean(emitExprWide(node.consequent, env).isBool) !==
      Boolean(emitExprWide(node.alternate, env).isBool)
  )
    throw textShapeError(
      node,
      'AOT: a ternary in text mixes a boolean branch with a non-boolean one',
      'JS renders those differently (`cond ? true : 5` is "true" or "5") but they share one C slot here. Make both branches the same kind.',
    );
  // A boolean draws nothing as a child but stringifies as an operand — same split as the constants above.
  // isString stays FALSE either way: a boolean is not a string in JS, so it must not by itself put an
  // enclosing `+` into concatenation mode (`on + n` adds, giving 0, and only then does `+ 'x'` append).
  // The %s form below is picked up only when some other operand of that `+` really is a string.
  if (e.isBool)
    return operand
      ? {
          isString: false,
          parts: [{spec: '%s', code: `((${e.code}) ? "true" : "false")`}],
        }
      : {isString: false, parts: [{literal: ''}]};
  return {
    isString: e.cType === 'string',
    parts: [{spec: printfSpec(e.cType), code: printfArg(e)}],
  };
}

/**
 * Lowers an expression to a printf format + args for a char-buffer destination (a <Text> body, a string
 * state slot, a <TextInput value>). A string-building `+` chain becomes one spec per dynamic part with
 * the literals folded into the format; anything else is a single spec over its own value.
 *
 * @returns {{format: string, args: string[]}} `format` already has literal `%` escaped as `%%`.
 */
function emitFormat(node, env, scope = env.consts ?? {}) {
  const visible = foldScope(env, scope);
  let format = '';
  const args = [];
  for (const part of concatParts(node, env, visible).parts) {
    if (part.literal !== undefined) format += part.literal.replace(/%/g, '%%');
    else {
      format += part.spec;
      args.push(part.code);
    }
  }
  return {format, args};
}

/** Renders an emitFormat() result as snprintf's trailing arguments (a constant string keeps its `%s` form). */
const formatArgs = ({format, args}) =>
  args.length
    ? `${cstr(format)}, ${args.join(', ')}`
    : `"%s", ${cstr(format.replace(/%%/g, '%'))}`;

// ---------------------------------------------------------------------------------------------------
// AST helpers + collection passes — small predicates (isFn, fnReturnsJSX, …) and the up-front scans
// that walk the component body ONCE to gather what later emission needs: the module scope, useState,
// child components, and the useCallback / useMemo / useEffect / useRef hooks.
// ---------------------------------------------------------------------------------------------------
const isFn = n =>
  n &&
  (n.type === 'FunctionDeclaration' ||
    n.type === 'FunctionExpression' ||
    n.type === 'ArrowFunctionExpression');

function findComponent(program) {
  for (const stmt of program.body) {
    const d = stmt.type === 'ExportNamedDeclaration' ? stmt.declaration : stmt;
    if (!d) continue;
    if (d.type === 'FunctionDeclaration' && d.id?.name === 'App') return d;
    if (d.type === 'VariableDeclaration')
      for (const decl of d.declarations)
        if (decl.id?.name === 'App' && isFn(decl.init)) return decl.init;
  }
  throw new Error(
    'AOT: no `App` component found (expected `export function App() { ... }`)',
  );
}

// Target screen size, baked at compile time so the demo's responsive `screen.width`/`screen.height`
// branching folds to the layout for THIS build (each board compiles its own binary). Override per target,
// e.g. ER_AOT_SCREEN_W=240 ER_AOT_SCREEN_H=320 for the CYD; defaults to a wide 800×480.
const SCREEN_W = Number(process.env.ER_AOT_SCREEN_W) || 800;
const SCREEN_H = Number(process.env.ER_AOT_SCREEN_H) || 480;
// Whether BOTH dimensions above came from the environment. A blank or unparsable override falls back to
// the default for that axis alone, so this asks what was actually used, not whether the vars are set.
const SCREEN_FROM_ENV =
  Number(process.env.ER_AOT_SCREEN_W) > 0 &&
  Number(process.env.ER_AOT_SCREEN_H) > 0;

/**
 * Every name a variable declaration directly in `body` binds, destructuring included. In JS each one
 * shadows a module binding of that name for the whole function body, not only from its declaration on.
 */
function declaredNames(body) {
  const names = [];
  const walk = p => {
    if (!p) return;
    if (p.type === 'Identifier') names.push(p.name);
    else if (p.type === 'ArrayPattern') p.elements.forEach(walk);
    else if (p.type === 'ObjectPattern')
      p.properties.forEach(q =>
        walk(q.type === 'RestElement' ? q.argument : q.value),
      );
    else if (p.type === 'AssignmentPattern') walk(p.left);
    else if (p.type === 'RestElement') walk(p.argument);
  };
  for (const stmt of body)
    if (stmt.type === 'VariableDeclaration')
      for (const d of stmt.declarations) walk(d.id);
  return names;
}

function moduleScope(program, screen, seed = {}) {
  // `seed` pre-populates the scope (e.g. image imports as their asset-name strings) BEFORE module consts are
  // folded, so a const that references one — `const DAYS = [{ icon: wxSun }]` — folds correctly.
  const scope = {screen, ...seed};
  for (const stmt of program.body) {
    const d = stmt.type === 'ExportNamedDeclaration' ? stmt.declaration : stmt;
    if (d?.type !== 'VariableDeclaration') continue;
    for (const decl of d.declarations) {
      if (
        !decl.id ||
        decl.id.type !== 'Identifier' ||
        !decl.init ||
        isFn(decl.init)
      )
        continue;
      try {
        scope[decl.id.name] = evalStatic(decl.init, withUndefined(scope));
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
    throw aotError(
      `AOT: list state "${name}" needs ≥1 initial element to infer its item shape`,
      shapeHint,
    );
  const first = items[0];
  if (typeof first !== 'object' || first === null || Array.isArray(first))
    throw aotError(
      `AOT: list state "${name}" elements must be objects`,
      shapeHint,
    );
  const fields = Object.keys(first).map(key => {
    const v = first[key];
    if (typeof v === 'string') return {key, kind: 'string'};
    if (typeof v === 'number')
      return {key, kind: Number.isInteger(v) ? 'int' : 'float'};
    throw aotError(
      `AOT: list state "${name}" field "${key}" must be a string or number`,
      shapeHint,
    );
  });
  return {fields};
}

/**
 * Collects a component's useState declarations → state descriptors keyed by JS name (reads) and setter
 * name (writes). `prefix` namespaces the C STORAGE so each inlined child instance gets its own slots: the
 * lookup keys stay the bare JS names (`count`), but the C field / array / count derive from `cField`
 * (`<prefix>count`). prefix='' (the App) leaves storage names exactly as the JS names — backward compatible.
 */
function collectState(fnBody, scope, prefix = '', wide = NO_WIDE) {
  const byName = new Map();
  const bySetter = new Map();
  for (const stmt of fnBody.body) {
    if (stmt.type !== 'VariableDeclaration') continue;
    for (const decl of stmt.declarations) {
      const init = decl.init;
      if (init?.type !== 'CallExpression') continue;
      // `useHostValue(initial)` — a scalar the HOST feeds at runtime (e.g. a step count from an IMU).
      // It lowers to an ordinary s_state field (so reads/dep-tracking work exactly like useState), plus a
      // generated public setter `er_app_set_<name>()` (see the host-setter emission). Written `const x =
      // useHostValue(0)` — no JS setter, the C host is the only writer.
      const isHost = init.callee.name === 'useHostValue';
      if (init.callee.name !== 'useState' && !isHost) continue;
      if (
        isHost ? decl.id.type !== 'Identifier' : decl.id.type !== 'ArrayPattern'
      )
        continue;
      const name = isHost ? decl.id.name : decl.id.elements[0]?.name;
      const setter = isHost ? undefined : decl.id.elements[1]?.name;
      if (!name) continue;
      const cField = prefix + name; // C storage name (== name for the App; instance-unique for a child)
      const initArg = init.arguments[0];

      let rec;
      if (!isHost && initArg?.type === 'ArrayExpression') {
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
        rec = {
          name,
          cField,
          setter,
          kind: 'list',
          struct,
          items,
          cap: LIST_CAP,
          cTypeName: `ErItem_${cField}`,
          arrayName: `s_${cField}`,
          countMember: `s_${cField}_count`,
        };
      } else {
        const initVal = initArg
          ? evalStaticOrThrow(
              initArg,
              scope,
              `AOT: the initial value of state "${name}" must be a compile-time constant`,
              'useState(x) initial must be a literal or a constant expression (number, string, bool, or arithmetic over consts) — not a runtime value or function call.',
            )
          : 0;
        // Host values (useHostValue) are numeric only — the public API is useHostValue(initial: number).
        // cTypeOfValue funnels booleans/null/objects to 'int', so validate the evaluated value itself
        // (Number.isFinite also rejects NaN/Infinity, and does not coerce booleans/strings).
        if (isHost && !Number.isFinite(initVal))
          throw aotError(
            `AOT: useHostValue("${name}") must be a number — its initial value evaluated to ${JSON.stringify(initVal)}`,
            'useHostValue(0) / useHostValue(0.0) — the host feeds an int or float; booleans, strings, and objects are not supported.',
          );
        // A scalar slot is an int, float, string or boolean. `null` or NaN would otherwise fold into an
        // initializer like `.value = NaN` — invalid C, with no location.
        if (
          initVal === undefined ||
          initVal === null ||
          (typeof initVal === 'number' && !Number.isFinite(initVal))
        ) {
          const e = aotError(
            `AOT: the initial value of state "${name}" is ${initVal === undefined ? 'undefined' : String(initVal)}`,
            "give it a concrete starting value: useState(0), useState(''), useState(false).",
          );
          if (initArg?.loc) e.aotLoc = initArg.loc.start;
          throw e;
        }
        let cType = cTypeOfValue(initVal);
        // JS prints a boolean as "true"/"false" and React renders one as nothing, so text lowering has
        // to tell useState(false) from useState(0) — cTypeOfValue funnels both to 'int'.
        const isBool = typeof initVal === 'boolean';
        // A numeric literal written with a decimal point or exponent (e.g. useState(70.0)) forces a FLOAT
        // slot even though the value is integral — lets the state hold sub-integer values (a smooth drag)
        // while the UI shows Math.round(value). (70.0 === 70 in JS, so we read the raw source to tell them apart.)
        if (
          cType === 'int' &&
          initArg?.type === 'NumericLiteral' &&
          typeof initArg.extra?.raw === 'string' &&
          /[.eE]/.test(initArg.extra.raw)
        ) {
          cType = 'float';
        }
        // A setter stores a timestamp in it, so it holds 64 bits (see compileWidened).
        if (cType === 'int' && !isBool && wide.has(cField)) cType = 'i64';
        // String scalar → a fixed char buffer in ErAppState; setters snprintf into it (see scalarAssign).
        const initCode =
          cType === 'string'
            ? cstr(String(initVal))
            : cType === 'float'
              ? floatLit(initVal)
              : String(Number(initVal));
        rec = {
          name,
          cField,
          setter,
          kind: 'scalar',
          cType,
          isBool,
          cMember: `s_state.${cField}`,
          initCode,
          host: isHost, // host-fed → also emit a public er_app_set_<name>() setter
        };
      }
      byName.set(name, rec);
      if (setter) bySetter.set(setter, rec);
    }
  }
  return {byName, bySetter};
}

function findReturnJSX(fnBody, scope = {}) {
  // Fold top-level `if (staticCond) return …` at compile time — responsive layouts switch on `screen`.
  const scan = stmts => {
    for (const stmt of stmts) {
      if (stmt.type === 'IfStatement') {
        let test;
        try {
          test = evalStatic(stmt.test, scope);
        } catch {
          throw new Error(
            'AOT: a top-level `if` in the component must have a compile-time-constant test (e.g. on the `screen` global) — runtime layout branching is not supported',
          );
        }
        const branch = test ? stmt.consequent : stmt.alternate;
        if (branch) {
          const r = scan(
            branch.type === 'BlockStatement' ? branch.body : [branch],
          );
          if (r) return r;
        }
        continue;
      }
      if (stmt.type === 'ReturnStatement' && stmt.argument) {
        if (stmt.argument.type === 'JSXElement') return stmt.argument;
        throw new Error(
          `AOT: the component must return a single JSX element (got ${stmt.argument.type})`,
        );
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

const fnReturnsJSX = fn =>
  fn.body.type === 'JSXElement' ||
  (fn.body.type === 'BlockStatement' &&
    fn.body.body.some(
      s => s.type === 'ReturnStatement' && s.argument?.type === 'JSXElement',
    ));

/** Collects top-level function components (name → fn node), excluding the `App` entry component. */
/** Resolves a component definition expression to its function node, unwrapping memo(fn) / React.memo(fn). */
function asComponentFn(node) {
  if (isFn(node)) return node;
  if (
    node?.type === 'CallExpression' &&
    isFn(node.arguments[0]) &&
    (node.callee.name === 'memo' ||
      (node.callee.type === 'MemberExpression' &&
        node.callee.property?.name === 'memo'))
  )
    return node.arguments[0];
  return null;
}

function collectComponents(program) {
  const comps = new Map();
  for (const stmt of program.body) {
    const d = stmt.type === 'ExportNamedDeclaration' ? stmt.declaration : stmt;
    if (!d) continue;
    if (
      d.type === 'FunctionDeclaration' &&
      d.id &&
      d.id.name !== 'App' &&
      fnReturnsJSX(d)
    )
      comps.set(d.id.name, d);
    if (d.type === 'VariableDeclaration')
      for (const decl of d.declarations) {
        if (decl.id?.type !== 'Identifier' || decl.id.name === 'App') continue;
        const fn = asComponentFn(decl.init); // unwrap memo(...)
        if (fn && fnReturnsJSX(fn)) comps.set(decl.id.name, fn);
      }
  }
  return comps;
}

/**
 * Collects callable HELPER functions (non-component, non-hook) a handler can inline: module-level
 * `function f(){}` / `const f = () => {}` and the component's own local `const f = (args) => {…}` arrows.
 * A helper is any function that does NOT return JSX (those are components) and is a plain function (not a
 * useCallback/useMemo/useState call). Returns Map(name → fn node). Re-collected per component (cheap).
 */
function collectHelpers(componentBody, program) {
  const helpers = new Map();
  const add = (name, fn) => {
    if (name && name !== 'App' && isFn(fn) && !fnReturnsJSX(fn))
      helpers.set(name, fn);
  };
  for (const stmt of program.body) {
    const d = stmt.type === 'ExportNamedDeclaration' ? stmt.declaration : stmt;
    if (!d) continue;
    if (d.type === 'FunctionDeclaration' && d.id) add(d.id.name, d);
    if (d.type === 'VariableDeclaration')
      for (const decl of d.declarations)
        if (decl.id?.type === 'Identifier') add(decl.id.name, decl.init);
  }
  if (componentBody.type === 'BlockStatement') {
    for (const stmt of componentBody.body) {
      if (stmt.type !== 'VariableDeclaration') continue;
      for (const decl of stmt.declarations)
        if (decl.id?.type === 'Identifier') add(decl.id.name, decl.init);
    }
  }
  return helpers;
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
    if (
      stmt.type !== 'ImportDeclaration' ||
      typeof stmt.source.value !== 'string'
    )
      continue;
    const importPath = stmt.source.value;
    if (!IMAGE_EXT_RE.test(importPath)) continue;
    const name = importPath.split(/[\\/]/).pop().replace(IMAGE_EXT_RE, '');
    for (const spec of stmt.specifiers) {
      if (spec.type === 'ImportDefaultSpecifier')
        byLocal.set(spec.local.name, {name, importPath});
    }
  }
  return byLocal;
}

/** Collects `import x from './foo.svg'` → Map(localName → { name, importPath }). Unlike an image (whose
 *  bytes the CLI bakes AFTER compile, so compile only needs its asset name), an <Svg source> needs the
 *  GEOMETRY during compile — so the CLI bakes the .svg to a vector artifact up front (bakeSvgArtifacts) and
 *  passes it in as opts.svgArtifacts, keyed by `name`. emitSvgSource resolves the local → name → artifact. */
function collectSvgImports(program) {
  const byLocal = new Map();
  for (const stmt of program.body) {
    if (
      stmt.type !== 'ImportDeclaration' ||
      typeof stmt.source.value !== 'string'
    )
      continue;
    const importPath = stmt.source.value;
    if (!/\.svg$/i.test(importPath)) continue;
    const name = importPath
      .split(/[\\/]/)
      .pop()
      .replace(/\.svg$/i, '');
    for (const spec of stmt.specifiers) {
      if (spec.type === 'ImportDefaultSpecifier')
        byLocal.set(spec.local.name, {name, importPath});
    }
  }
  return byLocal;
}

/** Bakes a Flow B app's <Svg source> imports → { assetName: artifact } via the SAME baker Flow A's esbuild
 *  .svg loader uses (svgToVector). This is the I/O half (reads .svg files) kept OUT of the pure compileSource;
 *  both CLI entries (aot/compile.mjs and the consumer cli.mjs) call it and hand the result to opts.svgArtifacts.
 *  @param {string} src       App.jsx source text.
 *  @param {string} baseDir   Directory the import paths are resolved against (the app's dir).
 *  @returns {Promise<Object>} name → { ops, paints, gradients, width, height }. */
export async function bakeSvgArtifacts(src, baseDir) {
  // Parse with the TS plugin too (harmless for plain JSX) so a .tsx app's <Svg source> imports are found.
  const program = parse(src, {
    sourceType: 'module',
    plugins: ['jsx', 'typescript'],
  }).program;
  const imports = collectSvgImports(program);
  if (imports.size === 0) return {};
  const {svgToVector, svgToRaster, writeRasterPng} =
    await import('../assets/bake-svg.mjs');
  const artifacts = {};
  for (const [, imp] of imports) {
    const p = resolve(baseDir, imp.importPath);
    if (!existsSync(p))
      throw new Error(
        `AOT: <Svg source> asset "${imp.name}" not found at ${p}`,
      );
    const svg = readFileSync(p, 'utf8');
    const art = await svgToVector(svg);
    // Raster fallback (Flow B): an SVG that uses features the vector baker can't represent is rasterized via
    // resvg and baked as a PNG (emitSvgSource emits an Image node for a kind:'raster' artifact + registers the
    // PNG into the AOT image baker), so the content renders instead of dropping.
    if (art.dropped && art.dropped.length) {
      console.warn(
        `embedded-react: ${imp.name}.svg uses unsupported SVG feature(s) [${art.dropped.join(', ')}] — ` +
          `rasterizing it as a fallback image (Flow B bakes the PNG into assets.generated.c). Simplify the SVG ` +
          `to keep it a live vector.`,
      );
      const {width, height, png} = await svgToRaster(svg);
      artifacts[imp.name] = {
        kind: 'raster',
        name: imp.name,
        width,
        height,
        png: writeRasterPng(imp.name, png),
      };
    } else {
      artifacts[imp.name] = art;
    }
  }
  return artifacts;
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
      if (
        init?.type === 'CallExpression' &&
        init.callee.name === 'useCallback' &&
        decl.id.type === 'Identifier' &&
        isFn(init.arguments[0])
      ) {
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
      if (
        init?.type === 'CallExpression' &&
        init.callee.name === 'useMemo' &&
        decl.id.type === 'Identifier' &&
        isFn(init.arguments[0])
      ) {
        const body = init.arguments[0].body;
        if (body.type === 'BlockStatement')
          throw new Error(
            `AOT: useMemo for "${decl.id.name}" must be a single expression (for now)`,
          );
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
    if (
      call?.type === 'CallExpression' &&
      call.callee.type === 'Identifier' &&
      call.callee.name === 'useEffect'
    ) {
      if (!isFn(call.arguments[0]))
        throw aotError(
          'AOT: useEffect must take an inline function',
          'write useEffect(() => { … }, []).',
        );
      effects.push({
        fn: call.arguments[0],
        deps: call.arguments[1],
        node: call,
      });
    }
  }
  return effects;
}

/** True if a function component declares any useState (per-instance child state — not yet supported). */
function usesState(fn) {
  if (fn.body.type !== 'BlockStatement') return false;
  return fn.body.body.some(
    s =>
      s.type === 'VariableDeclaration' &&
      s.declarations.some(
        d =>
          d.init?.type === 'CallExpression' &&
          d.init.callee.name === 'useState',
      ),
  );
}

// ---------------------------------------------------------------------------------------------------
// Animations — useAnimatedValue → an engine-side ERAnimValueHandle (native driver). The value binds to
// a node property (opacity / transform / color) via er_anim_value_bind, and Animated.timing/spring(...)
// .start() → er_anim_value_animate. The host's per-frame embedded_renderer_tick advances it in C — no
// per-frame JS, no app_update needed for the motion itself.
// ---------------------------------------------------------------------------------------------------

/** Collects `const x = useAnimatedValue(initial)` → Map(name → {cVar, initCode}). `prefix` namespaces the
 *  C var (`s_av_<prefix><name>`) so each inlined child instance gets its own engine value handle. */
function collectAnims(fnBody, scope, prefix = '') {
  const anims = new Map();
  if (fnBody.type !== 'BlockStatement') return anims;
  for (const stmt of fnBody.body) {
    if (stmt.type !== 'VariableDeclaration') continue;
    for (const decl of stmt.declarations) {
      const init = decl.init;
      if (
        init?.type === 'CallExpression' &&
        init.callee.name === 'useAnimatedValue' &&
        decl.id.type === 'Identifier'
      ) {
        const initVal = init.arguments[0]
          ? evalStaticOrThrow(
              init.arguments[0],
              scope,
              `AOT: the initial value of useAnimatedValue "${decl.id.name}" must be a compile-time constant`,
              'give it a finite starting number: useAnimatedValue(0).',
            )
          : 0;
        // An animated value is a float slot; null or a non-finite number would reach C as `NaNf`.
        // Refuse here, at the declaration, with its location.
        if (typeof initVal !== 'number' || !Number.isFinite(initVal)) {
          const e = aotError(
            `AOT: the initial value of useAnimatedValue "${decl.id.name}" is ${initVal === undefined ? 'undefined' : String(initVal)}`,
            'give it a finite starting number: useAnimatedValue(0).',
          );
          if (init.arguments[0]?.loc) e.aotLoc = init.arguments[0].loc.start;
          throw e;
        }
        anims.set(decl.id.name, {
          cVar: `s_av_${prefix}${decl.id.name}`,
          initCode: floatLit(initVal),
        });
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
function collectRefs(fnBody, scope, prefix = '', wide = NO_WIDE) {
  const refs = new Map();
  if (fnBody.type !== 'BlockStatement') return refs;
  for (const stmt of fnBody.body) {
    if (stmt.type !== 'VariableDeclaration') continue;
    for (const decl of stmt.declarations) {
      const init = decl.init;
      if (
        init?.type === 'CallExpression' &&
        init.callee.name === 'useRef' &&
        decl.id.type === 'Identifier'
      ) {
        const arg = init.arguments[0];
        if (isPanCreate(arg)) continue; // a PanResponder ref — collectPanResponders owns it
        const cVar = `s_ref_${prefix}${decl.id.name}`;
        if (!arg || arg.type === 'NullLiteral') {
          refs.set(decl.id.name, {
            cVar,
            cType: 'ERNode*',
            initCode: 'NULL',
            kind: 'node',
            used: false,
          });
          continue;
        }
        const v = evalStaticOrThrow(
          arg,
          scope,
          `AOT: useRef initial for "${decl.id.name}" must be a number (value ref) or null/empty (node ref)`,
          'a value ref needs a compile-time number: useRef(0).',
        );
        if (typeof v !== 'number' || !Number.isFinite(v)) {
          const e = aotError(
            `AOT: useRef initial for "${decl.id.name}" must be a number (value ref) or null/empty (node ref)`,
            `got ${v === undefined ? 'undefined' : String(v)}.`,
          );
          if (arg.loc) e.aotLoc = arg.loc.start;
          throw e;
        }
        // A ref a timestamp is written to holds 64 bits (see compileWidened).
        const cType = !Number.isInteger(v)
          ? 'float'
          : wide.has(cVar)
            ? 'i64'
            : 'int';
        refs.set(decl.id.name, {
          cVar,
          cType,
          initCode: cType === 'float' ? `${v}f` : String(v),
          kind: 'value',
          used: false,
        });
      }
    }
  }
  return refs;
}

/** Resolves a JSX tag to a name, mapping `Animated.View/Text/Image` to their host element. */
function resolveTag(openingElement) {
  const n = openingElement.name;
  if (n.type === 'JSXIdentifier') return n.name;
  if (n.type === 'JSXMemberExpression' && n.object.name === 'Animated')
    return n.property.name; // Animated.View → View
  throw new Error('AOT: unsupported JSX tag expression');
}

/** Style key → the ERAnimProp(s) an animated value binds to. */
const ANIM_STYLE_PROPS = {
  opacity: ['ER_PROP_OPACITY'],
  backgroundColor: ['ER_PROP_BACKGROUND_COLOR'],
  color: ['ER_PROP_COLOR'],
};
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
  // `NaNf` / `Infinityf` / `undefinedf` are not C. Every numeric-literal path funnels through here, so a
  // value that slipped past its own boundary check still fails at generate time, never in the host compiler.
  if (!Number.isFinite(v))
    throw aotError(
      `AOT: a numeric constant folded to ${n === undefined ? 'undefined' : String(n)}, which has no C form`,
      'the value must be a finite number.',
    );
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
  const FALLBACK = {ease: 'ER_EASE_EASE_IN_OUT', bezier: null};
  if (!node) return FALLBACK;
  // Bare member: Easing.linear / Easing.ease / Easing.quad (== quad-in) / ...
  if (node.type === 'MemberExpression' && node.object?.name === 'Easing') {
    const m = {
      linear: 'ER_EASE_LINEAR',
      ease: 'ER_EASE_EASE',
      quad: 'ER_EASE_QUAD_IN',
      cubic: 'ER_EASE_CUBIC_IN',
      bounce: 'ER_EASE_BOUNCE_OUT',
      elastic: 'ER_EASE_ELASTIC_OUT',
    };
    return {ease: m[node.property.name] || 'ER_EASE_EASE_IN_OUT', bezier: null};
  }
  // Call: Easing.bezier(...), Easing.elastic(n), Easing.in/out/inOut(inner)
  if (
    node.type === 'CallExpression' &&
    node.callee.type === 'MemberExpression' &&
    node.callee.object?.name === 'Easing'
  ) {
    const fn = node.callee.property.name;
    if (fn === 'bezier') {
      const cps = node.arguments
        .slice(0, 4)
        .map(a => Number(evalStaticOr(a, env, 0)));
      return cps.length === 4
        ? {ease: 'ER_EASE_BEZIER', bezier: cps}
        : FALLBACK;
    }
    if (fn === 'elastic') return {ease: 'ER_EASE_ELASTIC_OUT', bezier: null};
    if (fn === 'in' || fn === 'out' || fn === 'inOut') {
      const fam = easingFamily(node.arguments[0]);
      const dir = fn === 'in' ? 'IN' : fn === 'out' ? 'OUT' : 'IN_OUT';
      return fam ? {ease: `ER_EASE_${fam}_${dir}`, bezier: null} : FALLBACK;
    }
  }
  return FALLBACK;
}

/** Pushes `cfg.easing = …;` (and bezier control points for Easing.bezier) onto a timing config's C lines. */
function pushEasing(lines, c, easingNode, env) {
  const {ease, bezier} = easingInfo(easingNode, env);
  lines.push(`        ${c}.easing = ${ease};`);
  if (bezier) {
    lines.push(
      `        ${c}.bezier_x1 = ${floatLit(bezier[0])}; ${c}.bezier_y1 = ${floatLit(bezier[1])};`,
    );
    lines.push(
      `        ${c}.bezier_x2 = ${floatLit(bezier[2])}; ${c}.bezier_y2 = ${floatLit(bezier[3])};`,
    );
  }
}

/** Parses a `.interpolate({ inputRange, outputRange, extrapolate })` config object → a static
 *  { input, output, exLeft, exRight } descriptor (ranges must be static, equal-length, 2..8 points). */
function parseInterp(cfgNode, env) {
  if (cfgNode?.type !== 'ObjectExpression')
    throw aotError(
      'AOT: .interpolate() needs a config object literal { inputRange, outputRange }',
    );
  const get = k =>
    cfgNode.properties.find(p => (p.key.name ?? p.key.value) === k)?.value;
  const arr = (node, name) => {
    if (node?.type !== 'ArrayExpression')
      throw aotError(`AOT: .interpolate() ${name} must be an array literal`);
    return node.elements.map(e => Number(evalStatic(e, env.consts ?? {})));
  };
  const input = arr(get('inputRange'), 'inputRange');
  const output = arr(get('outputRange'), 'outputRange');
  if (input.length < 2 || input.length !== output.length)
    throw aotError(
      'AOT: .interpolate() inputRange and outputRange must be the same length (>= 2)',
    );
  if (input.length > 8)
    throw aotError(
      'AOT: .interpolate() supports up to 8 breakpoints (ER_INTERPOLATE_MAX_POINTS)',
    );
  const ex = node => {
    const v = node ? String(evalStaticOr(node, env, 'extend')) : 'extend';
    return v === 'clamp'
      ? 'ER_EXTRAPOLATE_CLAMP'
      : v === 'identity'
        ? 'ER_EXTRAPOLATE_IDENTITY'
        : 'ER_EXTRAPOLATE_EXTEND';
  };
  const both = get('extrapolate'); // RN: `extrapolate` sets both ends; extrapolateLeft/Right override.
  return {
    input,
    output,
    exLeft: ex(get('extrapolateLeft') ?? both),
    exRight: ex(get('extrapolateRight') ?? both),
  };
}

// ---------------------------------------------------------------------------------------------------
// JSX → style / text / events
// ---------------------------------------------------------------------------------------------------
function attrExpr(attr) {
  const v = attr.value;
  if (!v) return {type: 'BooleanLiteral', value: true};
  if (v.type === 'StringLiteral') return v;
  if (v.type === 'JSXExpressionContainer') return v.expression;
  return v;
}

/** A static ARGB8888 C literal (`0xAARRGGBBu`) from a CSS color string or number. */
function argbLiteral(value) {
  return (
    '0x' +
    (parseColor(String(value)) >>> 0)
      .toString(16)
      .padStart(8, '0')
      .toUpperCase() +
    'u'
  );
}

/** Lowers a dynamic (state-referencing) color expression to a C ARGB expression. */
function emitColorExpr(node, env) {
  if (node.type === 'StringLiteral') return colorLiteral(node.value);
  if (node.type === 'ConditionalExpression') {
    const t = asCond(emitExpr(node.test, env));
    return `((${t}) ? ${emitColorExpr(node.consequent, env)} : ${emitColorExpr(node.alternate, env)})`;
  }
  // A statically-resolvable color (a const string, or a theme token like `theme.card`) folds to a literal.
  try {
    const s = evalStatic(node, foldScope(env, env.consts ?? {}));
    if (typeof s === 'string') return colorLiteral(s);
  } catch {
    /* not static — fall through to the error below */
  }
  throw new Error(
    'AOT: a dynamic color must be a color string literal or a ternary of them',
  );
}

/** Lowers a dynamic enum-style expression (e.g. `flexDirection: row ? 'row' : 'column'`) to its ER_* constant
 *  (or a C ternary of them), looking values up in the style key's enum `table`. */
function emitEnumExpr(node, table, env) {
  if (node.type === 'StringLiteral') {
    const c = table[node.value];
    if (!c)
      throw aotError(
        `AOT: unsupported enum value "${node.value}"`,
        `one of: ${Object.keys(table).join(', ')}`,
      );
    return c;
  }
  if (node.type === 'ConditionalExpression') {
    const t = asCond(emitExpr(node.test, env));
    return `((${t}) ? ${emitEnumExpr(node.consequent, table, env)} : ${emitEnumExpr(node.alternate, table, env)})`;
  }
  // A statically resolvable enum (a const string) folds to its constant.
  try {
    const s = evalStatic(node, foldScope(env, env.consts ?? {}));
    if (typeof s === 'string' && table[s]) return table[s];
  } catch {
    /* not static — fall through */
  }
  throw aotError(
    'AOT: a state-driven enum style must be a string literal or a ternary of them',
    "e.g. flexDirection: wide ? 'row' : 'column'",
  );
}

/** The diagnostic for a style key the AOT has no lowering for, static or dynamic. */
const unknownStyleKey = key =>
  aotError(
    `AOT: style "${key}" is not supported by the AOT (no ERProps lowering)`,
    `Flow A may accept it; Flow B lowers these: ${STYLE_KEYS.join(', ')}.`,
  );

/**
 * Lowers one STATIC style key/value, telling the two failures apart: a key the AOT has no lowering for
 * at all, and a known key whose value it rejects. Both used to be indistinguishable from a state-driven
 * value, because a single `try` covered the constant fold and the lowering together.
 */
function lowerStyleChecked(key, value) {
  // `{transform: undefined}` is a no-op in Flow A and in lowerStyle itself; the key is only judged when
  // there is a value to lower.
  if (value === undefined || value === null) return [];
  if (!isStyleKey(key)) throw unknownStyleKey(key);
  try {
    return lowerStyle({[key]: value});
  } catch (e) {
    // style-map's own value errors already name the key in some cases — don't say it twice.
    const msg = String(e.message);
    const why = msg.startsWith(`${key}: `) ? msg.slice(key.length + 2) : msg;
    throw aotError(`AOT: unsupported value for style "${key}": ${why}`);
  }
}

/** Lowers one dynamic inline-style value to ERProps field assignment(s) (C expressions). */
function lowerDynamicStyleValue(key, valueNode, env) {
  // Own-property only: `DYN_FIELDS.toString` would otherwise hand back Object.prototype's method, look
  // like a known key, and emit `p.undefined = ...`.
  const meta = Object.hasOwn(DYN_FIELDS, key) ? DYN_FIELDS[key] : undefined;
  // An unknown key is unsupported outright — telling the author to "make it static" would only move
  // them on to the unknown-key error.
  if (!meta && !isStyleKey(key)) throw unknownStyleKey(key);
  if (!meta)
    throw aotError(
      `AOT: a state-driven value for style "${key}" is not supported (static only)`,
      `state-driven styles supported: colors, opacity, sizes/margins/padding, and the layout enums (flexDirection, alignItems, alignSelf, justifyContent, position, display). Make "${key}" static, or drive the change another way.`,
    );
  if (meta.kind === 'color')
    return [{field: meta.field, code: emitColorExpr(valueNode, env)}];
  if (meta.kind === 'enum')
    return [
      {field: meta.field, code: emitEnumExpr(valueNode, meta.table, env)},
    ];
  // Opacity and every size are numbers; C would take a char[] here as its address, or refuse it.
  const e = emitExpr(valueNode, env);
  if (e.cType === 'string') {
    const err = aotError(
      `AOT: style "${key}" needs a number, but the value is a string`,
      "JS would coerce the string; C cannot. Keep the state numeric — useState(10), not useState('10').",
    );
    if (valueNode.loc) err.aotLoc = valueNode.loc.start;
    throw err;
  }
  if (meta.kind === 'opacity')
    return [{field: meta.field, code: `(uint8_t)((${e.code}) * 255.0f)`}];
  return [{field: meta.field, code: `app_round_dim(${e.code})`}]; /* num */
}

/**
 * Whether a style already writes `field`, counting its percentage twin: `width: '50%'` lowers to
 * `width_pct`, and the author has still set the width. Used by the components that fall back to a
 * built-in size or inset when the style leaves one out — without the twin they inject a pixel default
 * ALONGSIDE the percentage, and the engine prefers the pixel value for a size.
 *
 * @param {{field: string}[]} staticAssigns  Static field writes from collectStyleAssigns().
 * @param {{field: string}[]} dynAssigns     State-driven field writes from collectStyleAssigns().
 * @param {string} field                     ERProps field to look for.
 */
const styleWrites = (staticAssigns, dynAssigns, field) =>
  [field, `${field}_pct`].some(
    f =>
      staticAssigns.some(a => a.field === f) ||
      dynAssigns.some(a => a.field === f),
  );

/**
 * Collects an element's merged style into static field assigns and dynamic (state-driven) field assigns.
 * Inline object values are tried statically first; a value that references state becomes a dynAssign.
 * Later style sources override earlier ones per field (RN merge), kept in `fields` by ERProps field.
 */
function collectStyleAssigns(openingElement, scope, env) {
  const fields = new Map(); // ERProps field -> { dynamic: bool, code: string }
  const binds = []; // [{ cVar, prop, interp? }] — animated values bound to node properties (native driver)
  const animRef = node =>
    node?.type === 'Identifier' && env.anims?.has(node.name)
      ? env.anims.get(node.name).cVar
      : null;
  // `<animValue>.interpolate({ inputRange, outputRange, extrapolate })` → { cVar, interp } for a mapped bind.
  const animInterpRef = node => {
    if (
      node?.type === 'CallExpression' &&
      node.callee.type === 'MemberExpression' &&
      !node.callee.computed &&
      node.callee.property.name === 'interpolate' &&
      node.callee.object.type === 'Identifier' &&
      env.anims?.has(node.callee.object.name)
    ) {
      return {
        cVar: env.anims.get(node.callee.object.name).cVar,
        interp: parseInterp(node.arguments[0], env),
      };
    }
    return null;
  };
  const apply = expr => {
    // RN ignores an undefined style or style-array entry; so does Flow A's flattenStyle.
    if (expr.type === 'Identifier' && expr.name === 'undefined') return;
    if (expr.type === 'ArrayExpression') {
      for (const e of expr.elements) if (e) apply(e);
      return;
    }
    if (expr.type === 'ObjectExpression') {
      for (const prop of expr.properties) {
        if (prop.type !== 'ObjectProperty')
          throw new Error(
            'AOT: spread/method in an inline style object not supported',
          );
        const key = prop.computed
          ? evalStatic(prop.key, scope)
          : (prop.key.name ?? prop.key.value);
        // `{transform: undefined}` is a no-op in RN and Flow A.
        if (prop.value.type === 'Identifier' && prop.value.name === 'undefined')
          continue;

        // Animated value bound directly to a prop (opacity / backgroundColor / color), optionally through
        // an .interpolate({ inputRange, outputRange }) mapping.
        const av = animRef(prop.value);
        if (av && ANIM_STYLE_PROPS[key]) {
          for (const p of ANIM_STYLE_PROPS[key])
            binds.push({cVar: av, prop: p});
          continue;
        }
        const ai = animInterpRef(prop.value);
        if (ai && ANIM_STYLE_PROPS[key]) {
          for (const p of ANIM_STYLE_PROPS[key])
            binds.push({cVar: ai.cVar, prop: p, interp: ai.interp});
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
                for (const p of ANIM_TRANSFORM_PROPS[tk])
                  binds.push({cVar: tav, prop: p});
                handled = true;
                continue;
              }
              const tai = animInterpRef(tp.value);
              if (tai && ANIM_TRANSFORM_PROPS[tk]) {
                for (const p of ANIM_TRANSFORM_PROPS[tk])
                  binds.push({cVar: tai.cVar, prop: p, interp: tai.interp});
                handled = true;
              }
            }
          }
          if (handled) continue;
        }

        // Only the FOLD may fall back to the dynamic path. Letting lowerStyle's own failure fall
        // through too made an unknown style key surface as "state-driven value ... (static only)",
        // which sends the author looking for state that isn't there.
        let staticValue;
        try {
          staticValue = {v: evalStatic(prop.value, withUndefined(scope))};
        } catch {
          staticValue = null; // references state — lower it as a dynamic value
        }
        if (staticValue) {
          for (const a of lowerStyleChecked(key, staticValue.v))
            fields.set(a.field, {dynamic: false, code: a.expr});
        } else {
          for (const a of lowerDynamicStyleValue(key, prop.value, env))
            fields.set(a.field, {dynamic: true, code: a.code});
        }
      }
      return;
    }
    // A StyleSheet reference / identifier resolving to a static style object. `style={null}` and a false
    // `cond && s` are valid RN and mean no style — Object.entries would throw on null.
    const resolved = evalStatic(expr, withUndefined(scope));
    if (resolved === null || resolved === undefined || resolved === false)
      return;
    for (const [k, v] of Object.entries(resolved)) {
      if (v === undefined || v === null) continue;
      for (const a of lowerStyleChecked(k, v))
        fields.set(a.field, {dynamic: false, code: a.expr});
    }
  };
  for (const attr of openingElement.attributes) {
    if (attr.type !== 'JSXAttribute' || attr.name.name !== 'style') continue;
    apply(attrExpr(attr));
  }

  const staticAssigns = [];
  const dynAssigns = [];
  for (const [field, v] of fields)
    (v.dynamic ? dynAssigns : staticAssigns).push(
      v.dynamic ? {field, code: v.code} : {field, expr: v.code},
    );
  return {staticAssigns, dynAssigns, binds};
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

const cstr = s =>
  `"${s.replace(/\\/g, '\\\\').replace(/"/g, '\\"').replace(/\n/g, '\\n').replace(/\t/g, '\\t')}"`;

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
      const t = /\n/.test(child.value)
        ? child.value.replace(/\s+/g, ' ').trim()
        : child.value;
      format += t.replace(/%/g, '%%');
    } else if (child.type === 'JSXExpressionContainer') {
      if (child.expression.type === 'JSXEmptyExpression') continue;
      // Constants fold into the literal; anything referencing state contributes a spec + arg.
      const f = emitFormat(child.expression, env, scope);
      format += f.format;
      args.push(...f.args);
      if (f.args.length) dynamic = true;
    } else if (child.type === 'JSXElement') {
      throw new Error(
        'AOT: nested <Text> / element children inside <Text> not yet supported (spans)',
      );
    }
  }
  return {dynamic, format, args};
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
    else if (
      c.type === 'JSXExpressionContainer' &&
      c.expression.type !== 'JSXEmptyExpression'
    ) {
      const v = evalStatic(c.expression, scope); // throws if it references state
      s += jsxChildText(v);
    } else if (c.type === 'JSXElement')
      throw aotError(
        'AOT: a nested <Text> span may not itself contain another <Text> (one level of spans only)',
      );
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
  if (
    !children.some(
      c => c.type === 'JSXElement' && c.openingElement.name.name === 'Text',
    )
  )
    return null;
  // Inherit sentinels (see ERTextSpan doc): color 0, font_size 0, weight/style/decoration 0xFF, spacing AUTO.
  const inheritSpan = text => ({
    text,
    color: '0u',
    font_size: '0',
    font_weight: '0xFF',
    font_style: '0xFF',
    text_decoration: '0xFF',
    letter_spacing: 'ER_LAYOUT_AUTO',
  });
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
        throw aotError(
          'AOT: a dynamic {…} segment inside a multi-span <Text> is not supported',
          'spans must be static; keep dynamic text in its own single <Text> (no nested <Text> siblings).',
        );
      }
      // React draws nothing for null, undefined, or a boolean child.
      if (v !== undefined && v !== null && typeof v !== 'boolean')
        spans.push(inheritSpan(cstr(String(v))));
    } else if (
      c.type === 'JSXElement' &&
      c.openingElement.name.name === 'Text'
    ) {
      const {staticAssigns, dynAssigns} = collectStyleAssigns(
        c.openingElement,
        scope,
        env,
      );
      if (dynAssigns.length)
        throw aotError(
          'AOT: a state-driven style on a nested <Text> span is not supported',
          'give the span <Text> a static style.',
        );
      const field = (f, dflt) =>
        staticAssigns.find(a => a.field === f)?.expr ?? dflt;
      spans.push({
        text: cstr(staticTextContent(c.children, scope)),
        color: field('color', '0u'),
        font_size: field('font_size', '0'),
        font_weight: field('font_weight', '0xFF'),
        font_style: field('font_style', '0xFF'),
        text_decoration: field('text_decoration', '0xFF'),
        letter_spacing: field('letter_spacing', 'ER_LAYOUT_AUTO'),
      });
    } else
      throw aotError(
        'AOT: unsupported child inside a multi-span <Text>',
        'a <Text> with a nested <Text> may contain text, {static expressions}, and nested <Text> only.',
      );
  }
  // The engine renders at most ER_TEXT_MAX_SPANS segments; refuse to silently drop the rest.
  if (spans.length > AOT_MAX_TEXT_SPANS) {
    throw aotError(
      `AOT: a <Text> has ${spans.length} inline segments but the engine renders at most ${AOT_MAX_TEXT_SPANS}`,
      `combine adjacent plain-text segments, or end the sentence right after a styled <Text> (e.g. "A <b>B</b> C <b>D</b>" is 4). If your engine build raised ER_TEXT_MAX_SPANS, set ER_AOT_MAX_TEXT_SPANS to match when running the AOT.`,
    );
  }
  return spans;
}

// ---------------------------------------------------------------------------------------------------
// Handler compilation — an on* arrow/function → C statements that mutate state and re-render.
// ---------------------------------------------------------------------------------------------------
/** Compiles a list-state setter call (`setItems(...)`) to bounded C array mutations. */
function compileListOp(rec, arg, env) {
  const {arrayName: arr, countMember: cnt, cap, struct} = rec;
  // setItems([...items, a, b]) — append; setItems([]) — clear.
  if (arg.type === 'ArrayExpression') {
    if (arg.elements.length === 0) return [`    ${cnt} = 0;`];
    const [head, ...rest] = arg.elements;
    if (head?.type !== 'SpreadElement' || head.argument.name !== rec.name)
      throw new Error(
        `AOT: a list literal must spread the current list first: [...${rec.name}, item]`,
      );
    const lines = [];
    for (const el of rest) {
      if (el.type !== 'ObjectExpression')
        throw new Error('AOT: appended list items must be object literals');
      const props = new Map(
        el.properties.map(p => [p.key.name ?? p.key.value, p.value]),
      );
      lines.push(`    if (${cnt} < ${cap})`, '    {');
      for (const f of struct.fields) {
        const valNode = props.get(f.key);
        if (!valNode) continue;
        if (f.kind === 'string')
          lines.push(
            `        snprintf(${arr}[${cnt}].${f.key}, sizeof(${arr}[${cnt}].${f.key}), ${formatArgs(emitFormat(valNode, env))});`,
          );
        else
          lines.push(
            `        ${arr}[${cnt}].${f.key} = ${emitExpr(valNode, env).code};`,
          );
      }
      lines.push(`        ${cnt}++;`, '    }');
    }
    return lines;
  }
  // setItems(items.slice(0, X)) — slice(0,-1) pops the last; slice(0,n) truncates to n.
  if (
    arg.type === 'CallExpression' &&
    arg.callee.type === 'MemberExpression' &&
    arg.callee.object.name === rec.name &&
    arg.callee.property.name === 'slice'
  ) {
    const end = arg.arguments[1];
    if (
      end?.type === 'UnaryExpression' &&
      end.operator === '-' &&
      end.argument.value === 1
    )
      return [`    if (${cnt} > 0) ${cnt}--;`];
    const e = emitExpr(end, env);
    return [`    ${cnt} = (${cnt} < (${e.code})) ? ${cnt} : (${e.code});`];
  }
  throw new Error(
    `AOT: unsupported list operation on "${rec.name}" (use [...${rec.name}, item], ${rec.name}.slice(0, -1), or [])`,
  );
}

/** Builds a `get(key)` accessor over an `Animated.*(value, config)` call's config object literal. */
function animConfigGetter(cfgObj) {
  return k =>
    cfgObj?.properties?.find(p => (p.key.name ?? p.key.value) === k)?.value;
}

/** Emits one atomic animation (timing/spring/decay) → a scoped ERAnimConfig + er_anim_value_animate.
 *  `delayMs` is the absolute start delay (how composition offsets are realised); `loop` repeats a timing;
 *  `onCompleteCb` (optional) is a C function name set as cfg.on_complete — used to chain sequence steps. */
function emitAnimEntry(entry, env, idx, onCompleteCb) {
  const {cVar, kind, get, delayMs, loop} = entry;
  const c = `cfg${idx}`;
  const lines = [
    '    {',
    `        ERAnimConfig ${c};`,
    `        memset(&${c}, 0, sizeof(${c}));`,
  ];
  if (kind === 'spring') {
    lines.push(`        ${c}.type = ER_ANIM_SPRING;`);
    lines.push(
      `        ${c}.stiffness = ${floatLit(evalStaticOr(get('stiffness'), env, 200))};`,
    );
    lines.push(
      `        ${c}.damping = ${floatLit(evalStaticOr(get('damping'), env, 18))};`,
    );
    lines.push(
      `        ${c}.mass = ${floatLit(evalStaticOr(get('mass'), env, 1))};`,
    );
  } else if (kind === 'decay') {
    lines.push(`        ${c}.type = ER_ANIM_DECAY;`);
    lines.push(
      `        ${c}.deceleration = ${floatLit(evalStaticOr(get('deceleration'), env, 0.998))};`,
    );
    lines.push(
      `        ${c}.velocity = ${floatLit(evalStaticOr(get('velocity'), env, 0))};`,
    );
  } else {
    lines.push(`        ${c}.type = ER_ANIM_TIMING;`);
    lines.push(
      `        ${c}.duration_ms = ${Math.round(Number(evalStaticOr(get('duration'), env, 250)))};`,
    );
    pushEasing(lines, c, get('easing'), env);
  }
  if (delayMs > 0) lines.push(`        ${c}.delay_ms = ${delayMs};`);
  if (loop) lines.push(`        ${c}.loop = true;`);
  if (onCompleteCb) lines.push(`        ${c}.on_complete = ${onCompleteCb};`);
  // decay is velocity-driven and has no toValue target; every other type needs one.
  const toNode = get('toValue');
  if (!toNode && kind !== 'decay')
    throw aotError(`AOT: Animated.${kind}() config needs a toValue`);
  const toCode = toNode
    ? emitExpr(toNode, env).code
    : `er_anim_value_get(${cVar})`;
  lines.push(
    `        er_anim_value_animate(${cVar}, (float)(${toCode}), &${c});`,
    '    }',
  );
  return lines;
}

/** Flattens an Animated composition (timing/spring/decay/sequence/parallel/stagger/delay/loop) into a flat
 *  list of atomic entries, each with an ABSOLUTE start delay (ms) — composition is realised purely through
 *  per-entry delay_ms (no engine grouping needed for standalone values). Returns { entries, duration } where
 *  `duration` is this node's own run length in ms, used to offset later siblings in a sequence/stagger;
 *  null = unknown (spring/decay/loop), which is illegal to sequence anything after. */
function flattenAnim(node, env, baseDelay, loop) {
  if (
    node?.type !== 'CallExpression' ||
    node.callee.type !== 'MemberExpression' ||
    node.callee.object?.name !== 'Animated'
  )
    throw aotError(
      'AOT: an animation must be Animated.timing/spring/decay/sequence/parallel/stagger/delay/loop(...)',
    );
  const kind = node.callee.property.name;
  const args = node.arguments;

  if (kind === 'timing' || kind === 'spring' || kind === 'decay') {
    const valRef = args[0];
    if (valRef?.type !== 'Identifier' || !env.anims?.has(valRef.name))
      throw aotError(
        `AOT: Animated.${kind}() first argument must be a useAnimatedValue`,
      );
    const cVar = env.anims.get(valRef.name).cVar;
    const get = animConfigGetter(args[1]);
    const ownDelay = Math.round(Number(evalStaticOr(get('delay'), env, 0)));
    const duration =
      kind === 'timing'
        ? ownDelay + Math.round(Number(evalStaticOr(get('duration'), env, 250)))
        : null;
    return {
      entries: [{cVar, kind, get, delayMs: baseDelay + ownDelay, loop}],
      duration,
    };
  }
  if (kind === 'delay') {
    return {
      entries: [],
      duration: Math.round(Number(evalStaticOr(args[0], env, 0))),
    };
  }
  if (kind === 'sequence' || kind === 'parallel' || kind === 'stagger') {
    const list = kind === 'stagger' ? args[1] : args[0];
    const staggerMs =
      kind === 'stagger'
        ? Math.round(Number(evalStaticOr(args[0], env, 0)))
        : 0;
    if (list?.type !== 'ArrayExpression')
      throw aotError(`AOT: Animated.${kind}(...) needs an array of animations`);
    const entries = [];
    let off = baseDelay; // running offset (sequence)
    let groupDur = 0; // max end-time relative to baseDelay (parallel/stagger)
    let i = 0;
    for (const child of list.elements) {
      if (!child) continue;
      const start = kind === 'sequence' ? off : baseDelay + i * staggerMs;
      const r = flattenAnim(child, env, start, loop);
      entries.push(...r.entries);
      if (kind === 'sequence') {
        if (r.duration == null)
          throw aotError(
            'AOT: an Animated.sequence entry needs a known duration — use Animated.timing / Animated.delay (a spring/decay/loop inside a sequence is not supported; it has no fixed length to offset the next entry by)',
          );
        off += r.duration;
      } else {
        const end = start - baseDelay + (r.duration ?? 0);
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
        throw aotError(
          'AOT: the same animated value is driven more than once in this composition — concurrent/flat same-value steps cancel each other. Use a top-level Animated.sequence(...) for multi-step animation of one value.',
        );
      seen.add(e.cVar);
    }
    return {
      entries,
      duration: kind === 'sequence' ? off - baseDelay : groupDur,
    };
  }
  if (kind === 'loop') {
    const r = flattenAnim(args[0], env, baseDelay, true);
    if (r.entries.length !== 1)
      throw aotError(
        'AOT: Animated.loop currently wraps a single Animated.timing/spring/decay (looping a sequence/parallel is not yet supported)',
      );
    return {entries: r.entries, duration: null};
  }
  throw aotError(`AOT: Animated.${kind}(...) is not a supported animation`);
}

/** Lowers `Animated.sequence([...]).start()` to an on_complete CHAIN: step 0 starts inline (in the handler),
 *  and each step's completion callback starts the next. Unlike the flat delay_ms path this is correct when
 *  steps share a value (out-and-back) — er_anim_value_animate cancels the running anim on a value, so
 *  synchronous same-value animates would cancel each other — and needs no fixed duration (spring/decay OK).
 *  delay() entries fold into the next step's delay_ms. Nested parallel/stagger/loop in a sequence throws. */
function compileSequenceChain(seqNode, env, ctx, doneCb = null) {
  const list = seqNode.arguments[0];
  if (list?.type !== 'ArrayExpression')
    throw aotError('AOT: Animated.sequence(...) needs an array of animations');
  const steps = [];
  let pendingDelay = 0;
  for (const child of list.elements) {
    if (!child) continue;
    if (
      child.type !== 'CallExpression' ||
      child.callee.type !== 'MemberExpression' ||
      child.callee.object?.name !== 'Animated'
    )
      throw aotError(
        'AOT: Animated.sequence entries must be Animated.timing/spring/decay/delay(...)',
      );
    const kind = child.callee.property.name;
    if (kind === 'delay') {
      pendingDelay += Math.round(
        Number(evalStaticOr(child.arguments[0], env, 0)),
      );
      continue;
    }
    if (kind !== 'timing' && kind !== 'spring' && kind !== 'decay')
      throw aotError(
        'AOT: Animated.sequence entries must be Animated.timing/spring/decay/delay (a nested parallel/stagger/loop inside a sequence is not yet supported — keep the sequence flat)',
      );
    const valRef = child.arguments[0];
    if (valRef?.type !== 'Identifier' || !env.anims?.has(valRef.name))
      throw aotError(
        `AOT: Animated.${kind}() first argument must be a useAnimatedValue`,
      );
    const get = animConfigGetter(child.arguments[1]);
    const ownDelay = Math.round(Number(evalStaticOr(get('delay'), env, 0)));
    steps.push({
      cVar: env.anims.get(valRef.name).cVar,
      kind,
      get,
      delayMs: pendingDelay + ownDelay,
      loop: false,
    });
    pendingDelay = 0;
  }
  if (!steps.length) return [];
  const seqId = ctx.out.seqN++; // GLOBAL: callback names are file-scope, so must be unique across handlers
  let firstLines = [];
  // Build from the tail so each step knows its successor's callback name. Step 0 runs inline; the rest
  // become on_complete callbacks pushed to out.animCbs (emitted at file scope).
  for (let i = steps.length - 1; i >= 0; i--) {
    // The last step's on_complete is the .start(onComplete) callback (if any) — the sequence is "done".
    const nextCb = i < steps.length - 1 ? `er_seqcb_${seqId}_${i + 1}` : doneCb;
    const lines = emitAnimEntry(steps[i], env, `${seqId}_${i}`, nextCb);
    if (i === 0) firstLines = lines;
    else ctx.out.animCbs.push({name: `er_seqcb_${seqId}_${i}`, body: lines});
  }
  return firstLines;
}

/** Compiles `<animation>.start()` — a single Animated.timing/spring/decay or a composition
 *  (sequence/parallel/stagger/delay/loop). A top-level sequence chains via on_complete (compileSequenceChain);
 *  everything else flattens to one ERAnimConfig + er_anim_value_animate per atomic entry, composition
 *  expressed through per-entry delay_ms. Native-driven; sets no React state. */
/** Compiles a `.start(onComplete)` completion callback to a file-scope C fn (ERAnimCompleteFn) set as the
 *  animation's on_complete; its body runs setters/refs/etc. and re-applies state via app_update if needed. */
function emitCompletionCb(fnNode, env, state, ctx) {
  const id = ctx.out.seqN++; // GLOBAL — callback names are file-scope
  const name = `er_donecb_${id}`;
  // `start(({ finished }) => …)`: expose the engine's `finished` bool as that local; a bare param is ignored.
  const locals = new Map(env.locals);
  const param = fnNode.params[0];
  if (param?.type === 'ObjectPattern') {
    for (const p of param.properties)
      if ((p.key?.name ?? p.key?.value) === 'finished')
        locals.set(p.value?.name ?? 'finished', {
          code: 'finished',
          cType: 'int',
        });
  }
  const body = fnNode.body;
  const list =
    body.type === 'BlockStatement'
      ? body.body
      : [{type: 'ExpressionStatement', expression: body}];
  const cctx = {stateChanged: false, animIdx: 0, out: ctx.out};
  const lines = compileStmts(list, {...env, locals}, state, cctx, '    ');
  if (cctx.stateChanged) lines.push('    app_update();');
  ctx.out.animCbs.push({name, body: lines});
  return name;
}

function compileAnimateStart(expr, env, state, ctx) {
  // .start(onComplete?) — an optional completion callback fired when the animation finishes.
  const doneCb = isFn(expr.arguments[0])
    ? emitCompletionCb(expr.arguments[0], env, state, ctx)
    : null;
  const receiver = expr.callee.object;
  if (
    receiver?.type === 'CallExpression' &&
    receiver.callee.type === 'MemberExpression' &&
    receiver.callee.object?.name === 'Animated' &&
    receiver.callee.property.name === 'sequence'
  ) {
    return compileSequenceChain(receiver, env, ctx, doneCb);
  }
  const {entries} = flattenAnim(receiver, env, 0, false);
  if (doneCb && entries.length > 1)
    throw aotError(
      'AOT: a .start(onComplete) callback on a parallel/stagger animation is not yet supported',
      'attach the completion callback to a single animation or an Animated.sequence(...). For "after all parallel anims", restructure as a sequence.',
    );
  const lines = [];
  entries.forEach((e, i) =>
    lines.push(
      ...emitAnimEntry(
        e,
        env,
        ctx.animIdx++,
        i === entries.length - 1 ? doneCb : null,
      ),
    ),
  );
  return lines;
}

/** evalStatic with a fallback default when the node is absent or not foldable. */
function evalStaticOr(node, env, dflt) {
  if (!node) return dflt;
  try {
    return evalStatic(node, foldScope(env, env.consts ?? {}));
  } catch {
    return dflt;
  }
}

/** Returns a statement node's body list: a BlockStatement's contents, or the lone statement wrapped. */
function blockList(node) {
  return node.type === 'BlockStatement' ? node.body : [node];
}

/** Matches `<member>` as a whole C lvalue, so `s_state.label` does not also match `s_state.label2`. */
const readsMember = member =>
  new RegExp(`(^|[^\\w.])${member.replace(/\./g, '\\.')}(?![\\w])`);

/**
 * Checks a value written to a numeric state or ref slot, whose C type came from its initial value. A 64-bit
 * timestamp written to an int slot is recorded in env.found, and compileWidened compiles again with that
 * slot widened to int64_t. It cannot go into a float or boolean slot, and a widened slot takes no floats.
 */
function storeCheck(e, slotType, isBool, key, what, env) {
  if (e.cType === 'i64' && slotType !== 'i64') {
    if (slotType === 'int' && !isBool) {
      env.found.add(key);
      return;
    }
    throw aotError(
      `AOT: a 64-bit time value cannot be stored in ${what}`,
      'give it a whole-number initial value — useState(0) or useRef(0) — and it widens to hold the timestamp.',
    );
  }
  if (slotType === 'i64' && e.cType === 'float') throw mix64Error();
}

/** Emits C to write an expression into a scalar state slot: snprintf for a string buffer (so a `+` chain
 *  becomes a format + args), plain assign otherwise. */
function scalarAssign(rec, node, env, indent) {
  if (rec.cType !== 'string') {
    const e = emitExprWide(node, env);
    storeCheck(
      e,
      rec.cType,
      rec.isBool,
      rec.cField,
      `state "${rec.name}"`,
      env,
    );
    return `${indent}${rec.cMember} = ${e.code};`;
  }
  const f = emitFormat(node, env);
  // snprintf's source and destination may not overlap (C11 7.21.6.6), and `setLabel(label + '!')` feeds
  // the slot its own contents. Build the new value in a temporary first — some embedded libcs write the
  // destination as they go, which would read back what they just overwrote.
  if (f.args.some(a => readsMember(rec.cMember).test(a)))
    return [
      `${indent}{`,
      `${indent}    char next[sizeof(${rec.cMember})];`,
      `${indent}    snprintf(next, sizeof(next), ${formatArgs(f)});`,
      `${indent}    memcpy(${rec.cMember}, next, strlen(next) + 1);`,
      `${indent}}`,
    ].join('\n');
  return `${indent}snprintf(${rec.cMember}, sizeof(${rec.cMember}), ${formatArgs(f)});`;
}

/** True if `node` is a `<ref>.current` member access on a known value ref. */
function refTarget(node, env) {
  if (
    node?.type === 'MemberExpression' &&
    !node.computed &&
    node.object.type === 'Identifier' &&
    node.property.name === 'current' &&
    env.refs?.has(node.object.name)
  ) {
    const r = env.refs.get(node.object.name);
    r.used = true;
    return r;
  }
  return null;
}

/** An imperative updateVector shape `{ arc:[…]|circle:[…]|rect:[x,y,w,h,rx?,ry?]|line:[…]|path:'…', fill, … }`
 *  → { entries (op-tape C exprs), locals (C declarations to emit ahead of them), paint (static 7-num
 *  record) }. Geometry coords may reference state/refs/event fields (emitExpr); paint must be static.
 *  Shares the ...EntriesC geometry with the JSX path. `tag` names any locals and must be unique in the
 *  handler block the caller writes them into. */
function imperativeShape(shapeNode, env, tag) {
  if (shapeNode?.type !== 'ObjectExpression')
    throw new Error('AOT: each updateVector shape must be an object literal');
  const props = {};
  for (const p of shapeNode.properties) {
    if (p.type !== 'ObjectProperty')
      throw new Error(
        'AOT: spread/method in an updateVector shape not supported',
      );
    props[p.key.name ?? p.key.value] = p.value;
  }
  const arr = (key, n) => {
    if (props[key].type !== 'ArrayExpression')
      throw new Error(`AOT: updateVector "${key}" must be an array literal`);
    return props[key].elements
      .slice(0, n)
      .map(el => `(float)(${emitExpr(el, env).code})`);
  };
  let geo;
  if (props.arc) geo = geometryOf(arcEntriesC(...arr('arc', 5)));
  else if (props.circle) geo = geometryOf(circleEntriesC(...arr('circle', 3)));
  else if (props.rect) {
    // Destructured rather than spread: the radii are optional, so a 4-element literal must not slide
    // `tag` into rx's slot.
    const [x, y, w, h, rx = null, ry = null] = arr('rect', 6);
    geo = geometryOf(rectEntriesC(x, y, w, h, rx, ry, tag));
  } else if (props.line) geo = geometryOf(lineEntriesC(...arr('line', 4)));
  else if (props.path)
    geo = geometryOf(
      parsePath(String(evalStatic(props.path, env.consts ?? {}))).map(floatLit),
    );
  else
    throw new Error(
      'AOT: an updateVector shape needs one of arc / circle / rect / line / path',
    );
  const stat = (key, dflt) => {
    if (props[key] == null) return dflt;
    try {
      return evalStatic(props[key], env.consts ?? {});
    } catch {
      throw new Error(`AOT: updateVector paint "${key}" must be static`);
    }
  };
  const paint = [
    parseColor(stat('fill', 'none')),
    parseColor(stat('stroke', 'none')),
    svgNum(stat('strokeWidth', 1), 1),
    svgNum(stat('miter', 4), 4),
    CAP_MAP[stat('cap', 'butt')] ?? 0,
    JOIN_MAP[stat('join', 'miter')] ?? 0,
    stat('fillRule', 'nonzero') === 'evenodd' ? 1 : 0,
  ];
  return {...geo, paint};
}

/** Lowers `updateVector(nodeRef, shapes, [x,y,w,h]?)` to: fill a mutable op-tape, push it to the node, and
 *  (optionally) hint the dirty sub-rect — the imperative fast path (drag) that bypasses app_update. */
function compileUpdateVector(expr, env, ctx, indent) {
  const out = ctx.out;
  const [refArg, shapesArg, dirtyArg] = expr.arguments;
  const ref = refArg?.type === 'Identifier' ? env.refs?.get(refArg.name) : null;
  if (ref?.kind !== 'node')
    throw new Error(
      'AOT: updateVector(ref, …) first arg must be a node ref (const r = useRef())',
    );
  ref.used = true;
  if (shapesArg?.type !== 'ArrayExpression')
    throw new Error(
      'AOT: updateVector(ref, shapes, …) shapes must be an array literal',
    );
  // Claimed before the shape loop so a shape's locals can carry it: two updateVector calls can land in
  // the same handler block, where a name keyed only on the shape index would collide.
  const id = out.svgN++;
  const entries = [];
  const decls = [];
  const paints = [];
  for (const s of shapesArg.elements) {
    const {
      entries: e,
      locals,
      paint,
    } = imperativeShape(s, env, `uv${id}_${paints.length}`);
    decls.push(...locals);
    entries.push('ER_VOP_SHAPE', floatLit(paints.length), ...e);
    paints.push(paint);
  }
  const len = entries.length;
  out.needsMath = true;
  out.vectorData.push(`static float s_uv${id}_ops[${len}];`);
  out.vectorData.push(
    `static const ERVectorPaint s_uv${id}_paints[] = {\n${paints.map(p => '    ' + emitVectorPaint(p)).join(',\n')}\n};`,
  );
  const lines = [
    ...decls.map(d => `${indent}${d}`),
    ...entries.map((e, i) => `${indent}s_uv${id}_ops[${i}] = ${e};`),
  ];
  lines.push(
    `${indent}er_node_set_vector_ops(${ref.cVar}, s_uv${id}_ops, ${len}, s_uv${id}_paints, ${paints.length}, NULL, 0);`,
  );
  if (dirtyArg) {
    if (dirtyArg.type !== 'ArrayExpression' || dirtyArg.elements.length < 4)
      throw new Error(
        'AOT: updateVector dirtyRect must be a [x, y, w, h] array literal',
      );
    const [x, y, w, h] = dirtyArg.elements.map(el => emitExpr(el, env).code);
    lines.push(
      `${indent}er_node_set_vector_dirty_rect(${ref.cVar}, ${x}, ${y}, ${w}, ${h});`,
    );
  }
  return lines;
}

/** Compiles one handler ExpressionStatement: a state setter, ref mutation, or Animated.*(...).start(). */
/** setInterval/setTimeout(cb, ms) → a C `er_timer_add(ms, repeat, fn)` expr; registers cb as a timer fn. */
function compileTimerAdd(expr, env, state, ctx) {
  const cb = expr.arguments[0];
  if (!isFn(cb))
    throw aotError(
      'AOT: a setInterval/setTimeout callback must be an inline function',
      'pass an inline arrow, e.g. setInterval(() => setTick((t) => t + 1), 1000).',
    );
  // A delay computed from a timestamp goes through ToInt32 and a floor at 0, as Flow A's setTimeout does.
  const delay = expr.arguments[1] ? emitExprWide(expr.arguments[1], env) : null;
  const ms = !delay
    ? '0'
    : delay.cType === 'i64'
      ? `app_delay_ms64(${delay.code})`
      : delay.code;
  const repeat = expr.callee.name === 'setInterval';
  const slot = ctx.out.timerFns.length;
  const name = `er_timer_fn_${slot}`;
  ctx.out.usesTimers = true;
  ctx.out.timerFns.push({name, body: null}); // reserve the slot before compiling the body (it may add more)
  ctx.out.timerFns[slot].body = compileHandler(cb, env, state, ctx.out);
  return `er_timer_add((int)(${ms}), ${repeat ? 'true' : 'false'}, ${name})`;
}

/** Inlines a handler-statement call to a helper / useCallback: binds the call's args to the helper's params
 *  as C locals, then compiles the helper body here in the current env/state/ctx. Guards against recursion. */
function inlineHelperCall(name, fn, args, env, state, ctx, indent) {
  ctx.inlining = ctx.inlining ?? new Set();
  if (ctx.inlining.has(name))
    throw aotError(
      `AOT: helper "${name}" is recursive — can't be inlined into a handler`,
      'handlers are flattened to straight-line C, so a helper that calls itself (directly or via another helper) has no base case to unroll. Move recursive logic out of the handler, or precompute the value.',
    );
  const locals = new Map(env.locals);
  fn.params.forEach((p, i) => {
    if (p.type !== 'Identifier')
      throw aotError(
        `AOT: helper "${name}" must take simple (identifier) params to be inlined`,
        'a helper called from a handler must use plain positional params (e.g. `(a, b) => …`) — destructuring or default params in the signature are not supported.',
      );
    if (args[i]) {
      const e = emitExprWide(args[i], env);
      locals.set(p.name, {code: e.code, cType: e.cType, isBool: e.isBool});
    }
  });
  const body = fn.body;
  const list =
    body.type === 'BlockStatement'
      ? body.body
      : [{type: 'ExpressionStatement', expression: body}];
  // The helper's statements are spliced into the CALLER, so its `return` is not the caller's return —
  // reject it here even when the caller is an effect body, where a `return` would otherwise be allowed.
  // Both marks are scoped to this call, so they unwind with it rather than outliving a thrown error.
  const outerAllowReturn = ctx.allowReturn;
  ctx.inlining.add(name);
  ctx.allowReturn = false;
  try {
    return compileStmts(list, {...env, locals}, state, ctx, indent);
  } finally {
    ctx.allowReturn = outerAllowReturn;
    ctx.inlining.delete(name);
  }
}

function compileHandlerExprImpl(expr, env, state, ctx, indent) {
  // updateVector(ref, shapes, dirtyRect?) — imperative vector redraw (no app_update).
  if (
    expr.type === 'CallExpression' &&
    expr.callee.type === 'Identifier' &&
    expr.callee.name === 'updateVector'
  ) {
    return compileUpdateVector(expr, env, ctx, indent);
  }
  // setInterval / setTimeout(cb, ms) → register a host-tick timer (the returned id is discarded here).
  if (
    expr.type === 'CallExpression' &&
    expr.callee.type === 'Identifier' &&
    (expr.callee.name === 'setInterval' || expr.callee.name === 'setTimeout')
  ) {
    return [`${indent}${compileTimerAdd(expr, env, state, ctx)};`];
  }
  // clearInterval / clearTimeout(id) → deactivate the timer slot.
  if (
    expr.type === 'CallExpression' &&
    expr.callee.type === 'Identifier' &&
    (expr.callee.name === 'clearInterval' ||
      expr.callee.name === 'clearTimeout')
  ) {
    return [
      `${indent}er_timer_clear(${emitExpr(expr.arguments[0], env).code});`,
    ];
  }
  // `ref.current = expr` / `ref.current += expr` — a value ref write; does NOT trigger a re-render.
  if (expr.type === 'AssignmentExpression') {
    const r = refTarget(expr.left, env);
    if (!r)
      throw new Error(
        'AOT: the only assignment allowed in a handler is `ref.current = ...`',
      );
    const e = emitExprWide(expr.right, env);
    if (expr.operator === '/=' && (r.cType === 'i64' || e.cType === 'i64'))
      throw aotError(
        'AOT: `/=` on a 64-bit time value is not supported',
        'JS division gives a fraction; write ref.current = Math.floor(ref.current / n) for whole units.',
      );
    if (expr.operator === '%=' && (r.cType === 'i64' || e.cType === 'i64'))
      nonzeroDivisor(expr.right, env);
    storeCheck(
      e,
      r.cType,
      false,
      r.cVar,
      `ref "${expr.left.object.name}"`,
      env,
    );
    return [`${indent}${r.cVar} ${expr.operator} ${e.code};`];
  }
  // `ref.current++` / `ref.current--`.
  if (expr.type === 'UpdateExpression') {
    const r = refTarget(expr.argument, env);
    if (!r)
      throw new Error(
        'AOT: the only ++/-- allowed in a handler is on `ref.current`',
      );
    return [`${indent}${r.cVar}${expr.operator};`];
  }
  // Animated.*(…).start() — single timing/spring/decay OR a sequence/parallel/stagger/delay/loop
  // composition; native-driven, sets no React state, needs no app_update.
  if (
    expr.type === 'CallExpression' &&
    expr.callee.type === 'MemberExpression' &&
    expr.callee.property.name === 'start'
  ) {
    return compileAnimateStart(expr, env, state, ctx);
  }
  if (expr.type !== 'CallExpression' || expr.callee.type !== 'Identifier')
    throw aotError(
      'AOT: a handler statement must be a state setter, a ref write, or Animated.timing/spring(...).start()',
      'each statement in a handler must be one of: setX(value) / setX(prev => …), a `ref.current = …` write, an `updateVector(…)` call, or `Animated.timing|spring(v, …).start()`. Wrap conditional logic in `if (…) { … }`.',
    );
  // A call to a helper / useCallback (e.g. `reset();`) → inline its body here so handlers can compose logic.
  const helperFn =
    env.helpers?.get(expr.callee.name) ?? env.callbacks?.get(expr.callee.name);
  if (helperFn)
    return inlineHelperCall(
      expr.callee.name,
      helperFn,
      expr.arguments,
      env,
      state,
      ctx,
      indent,
    );
  const rec = state.bySetter.get(expr.callee.name);
  if (!rec)
    throw aotError(
      `AOT: "${expr.callee.name}" is not a known state setter`,
      `a handler can only call a setter from this component's own useState (e.g. setCount), a ref write, updateVector(…), or Animated…start(). "${expr.callee.name}" isn't one of those — arbitrary functions (fetch, console.*, helpers) can't be lowered to C.`,
    );
  ctx.stateChanged = true;
  const arg = expr.arguments[0];
  if (rec.kind === 'list') return compileListOp(rec, arg, env);
  if (
    arg &&
    (arg.type === 'ArrowFunctionExpression' ||
      arg.type === 'FunctionExpression')
  ) {
    // setState(prev => expr): bind the param to the current value, assign the result.
    const param = arg.params[0]?.name;
    const locals = new Map(env.locals);
    if (param) locals.set(param, {code: rec.cMember, cType: rec.cType});
    if (arg.body.type === 'BlockStatement')
      throw new Error(
        'AOT: updater function must be a single expression (for now)',
      );
    return [scalarAssign(rec, arg.body, {...env, locals}, indent)];
  }
  return [scalarAssign(rec, arg, env, indent)];
}
const compileHandlerExpr = withLoc(compileHandlerExprImpl);

/**
 * Compiles a list of handler statements to C lines. Supports: `const x = expr` (a C local, visible to
 * later statements), `if (cond) {...} else {...}`, state setters, and Animated.*(...).start(). `ctx`
 * accumulates `stateChanged` (→ trailing app_update), `animIdx` (unique ERAnimConfig locals) and
 * `usedReturn` (an early `return` was lowered, so the body needs a C function of its own). `ctx.allowReturn`
 * and `ctx.bodyList` mark which body a `return` may exit and which statement of it is the tail.
 */
function compileStmts(list, env, state, ctx, indent) {
  const lines = [];
  for (let si = 0; si < list.length; si++) {
    const st = list[si];
    if (st.type === 'VariableDeclaration') {
      for (const decl of st.declarations) {
        if (decl.id.type !== 'Identifier')
          throw new Error(
            'AOT: destructuring a handler local is not supported',
          );
        if (!decl.init)
          throw new Error('AOT: a handler local must have an initializer');
        // A cleanup closure outlives the call that created it, so when one is emitted (a dep-driven
        // effect) the body's own locals become file-scope slots instead of C locals.
        const hoist = ctx.hoist && list === ctx.bodyList;
        const cName = hoist
          ? `${ctx.hoist.prefix}${decl.id.name}`
          : `l_${decl.id.name}`;
        // `const id = setInterval/setTimeout(…)` → an int timer-id local (so a later clearInterval(id) resolves).
        if (
          decl.init.type === 'CallExpression' &&
          decl.init.callee.type === 'Identifier' &&
          (decl.init.callee.name === 'setInterval' ||
            decl.init.callee.name === 'setTimeout')
        ) {
          // The id is only needed for a later clear*(); a mount effect drops its cleanup, so mark it used.
          if (hoist) ctx.hoist.decls.push(`static int ${cName};`);
          lines.push(
            `${indent}${hoist ? '' : 'int '}${cName} = ${compileTimerAdd(decl.init, env, state, ctx)};`,
            `${indent}(void)${cName};`,
          );
          env = {
            ...env,
            locals: new Map(env.locals).set(decl.id.name, {
              code: cName,
              cType: 'int',
            }),
          };
          continue;
        }
        const e = emitExprWide(decl.init, env);
        if (e.cType === 'string') {
          // A string local gets a buffer of its own rather than a char* into a state slot: a pointer alias
          // would let `setLabel(t + '!')` read and write the same bytes behind readsMember's back — and
          // `int l_t = s_state.label` was never valid C to begin with.
          if (hoist)
            ctx.hoist.decls.push(`static char ${cName}[${LIST_STR_CAP}];`);
          else lines.push(`${indent}char ${cName}[${LIST_STR_CAP}];`);
          lines.push(
            `${indent}snprintf(${cName}, sizeof(${cName}), "%s", ${e.code});`,
          );
        } else {
          if (e.cType !== 'int' && e.cType !== 'float' && e.cType !== 'i64')
            throw aotError(
              'AOT: a handler local must hold a number, a boolean or a string',
              'bind the value itself — e.g. `const id = item.id` — rather than a list item or a node ref.',
            );
          const cType = cScalarType(e.cType);
          if (hoist) ctx.hoist.decls.push(`static ${cType} ${cName};`);
          lines.push(
            `${indent}${hoist ? '' : cType + ' '}${cName} = ${e.code};`,
          );
        }
        env = {
          ...env,
          locals: new Map(env.locals).set(decl.id.name, {
            code: cName,
            cType: e.cType,
            isBool: e.isBool,
          }),
        };
      }
      continue;
    }
    // A `return` in an effect body. The LAST statement of the body is the cleanup `return () => …` (or a
    // bare `return`). A mount effect drops it — the app never unmounts, so it would never run. A dep-driven
    // effect re-runs, and React runs the previous cleanup first, so `ctx.cleanup` compiles it into a
    // companion C function and arms it here. Any EARLIER return is a guard that has to actually exit — it
    // emits `return;` and flags the body so compileEffect gives it a C function of its own.
    if (st.type === 'ReturnStatement') {
      if (!ctx.allowReturn) {
        const e = aotError(
          'AOT: `return` is only supported inside a useEffect body',
          'a handler, timer, animation or inlined-helper body cannot return early — flatten the logic into if/else.',
        );
        if (st.loc) e.aotLoc = st.loc.start;
        throw e;
      }
      const isTail = list === ctx.bodyList && si === list.length - 1;
      if (ctx.depDriven && isFn(st.argument) && !isTail) {
        const e = aotError(
          'AOT: a useEffect cleanup must be the last statement of the effect body',
          'a cleanup returned from inside an `if` would be dropped. Return `undefined` from the guard and put the single `return () => …` at the end of the body.',
        );
        if (st.loc) e.aotLoc = st.loc.start;
        throw e;
      }
      if (isTail) {
        if (ctx.cleanup && isFn(st.argument)) {
          ctx.cleanup.emit(st.argument, env);
          lines.push(`${indent}${ctx.cleanup.armed} = 1;`);
        }
        continue;
      }
      ctx.usedReturn = true;
      lines.push(`${indent}return;`);
      continue;
    }
    if (st.type === 'IfStatement') {
      lines.push(
        `${indent}if (${asCond(emitExprWide(st.test, env))})`,
        `${indent}{`,
      );
      lines.push(
        ...compileStmts(
          blockList(st.consequent),
          env,
          state,
          ctx,
          indent + '    ',
        ),
      );
      lines.push(`${indent}}`);
      if (st.alternate) {
        lines.push(`${indent}else`, `${indent}{`);
        lines.push(
          ...compileStmts(
            blockList(st.alternate),
            env,
            state,
            ctx,
            indent + '    ',
          ),
        );
        lines.push(`${indent}}`);
      }
      continue;
    }
    if (st.type !== 'ExpressionStatement')
      throw aotError(
        `AOT: unsupported statement "${st.type}" in event handler`,
        'a handler supports only `const x = …` locals, `if (…) { … } else { … }`, and expression statements (setters / ref writes / updateVector / Animated…start). Loops (for/while), switch and try/catch are not lowered — precompute values or flatten the logic into if/else.',
      );
    lines.push(...compileHandlerExpr(st.expression, env, state, ctx, indent));
  }
  return lines;
}

/**
 * Compiles a useEffect (App or child) into C. Two shapes:
 *  - `useEffect(fn, [])` — MOUNT-ONCE: body runs after the initial app_update (in er_app_build).
 *  - `useEffect(fn, [dep…])` — DEP-DRIVEN: body becomes a file-scope `er_effect_N()`; runs once at mount,
 *    then again from app_update whenever a SCALAR dep changes (compared against a stored prev). The body is
 *    compiled WITHOUT a trailing app_update — it runs INSIDE app_update / at mount, so re-applying state it
 *    sets happens on the next app_update (one-frame), and it can never re-enter app_update (no infinite loop).
 *    A dep-driven `return () => …` becomes `er_effect_N_cleanup()`, run at the top of the next re-run.
 */
function compileEffect(eff, env, state, out) {
  const body = eff.fn.body;
  const stmts =
    body.type === 'BlockStatement'
      ? body.body
      : [{type: 'ExpressionStatement', expression: body}];
  const isMount =
    !eff.deps ||
    (eff.deps.type === 'ArrayExpression' && eff.deps.elements.length === 0);
  if (isMount) {
    const ctx = {
      stateChanged: false,
      animIdx: 0,
      out,
      allowReturn: true,
      bodyList: stmts,
    };
    const lines = compileStmts(stmts, env, state, ctx, '    ');
    if (!ctx.usedReturn) {
      if (ctx.stateChanged) lines.push('    app_update();');
      out.mountEffects.push(...lines);
      return;
    }
    // A mount body is normally inlined into er_app_build, where a `return` would skip every later mount
    // effect — so one with an early return gets a C function of its own, and the app_update it may owe
    // moves to the call site (past the return, it would otherwise be dead code).
    const name = `er_effect_${out.effN++}`;
    out.effectFns.push({name, body: lines});
    out.mountEffects.push(`    ${name}();`);
    if (ctx.stateChanged) out.mountEffects.push('    app_update();');
    return;
  }
  if (eff.deps.type !== 'ArrayExpression')
    throw aotError(
      'AOT: a useEffect dependency list must be an array literal',
      'pass `[]` (run once) or `[a, b]` (re-run when a/b change).',
    );
  const id = out.effN++;
  const name = `er_effect_${id}`;
  const deps = eff.deps.elements.map(d => {
    if (!d) throw aotError('AOT: a useEffect dependency must be an expression');
    const e = emitExprWide(d, env);
    if (!['int', 'float', 'i64', 'string'].includes(e.cType))
      throw aotError(
        'AOT: useEffect dependencies must be scalar (number / bool / string)',
        'depend on scalar state values; object/array dependencies are not yet supported.',
      );
    return e;
  });
  // React runs the previous cleanup before re-running a dep-driven effect. The cleanup closes over the
  // body's locals, which have to outlive the call, so they become file-scope slots; an `armed` flag says
  // whether the last run actually reached its `return () => …` (a guarded run that returned early did not).
  const tail = stmts[stmts.length - 1];
  const hasCleanup = tail?.type === 'ReturnStatement' && isFn(tail.argument);
  const armed = `s_eff${id}_armed`;
  const ctx = {
    stateChanged: false,
    animIdx: 0,
    out,
    allowReturn: true,
    bodyList: stmts,
    depDriven: true,
  };
  if (hasCleanup) {
    ctx.hoist = {prefix: `s_eff${id}_l_`, decls: out.effectDecls};
    ctx.cleanup = {
      armed,
      emit: (fn, cenv) => {
        const cbody = fn.body;
        const clist =
          cbody.type === 'BlockStatement'
            ? cbody.body
            : [{type: 'ExpressionStatement', expression: cbody}];
        const cctx = {stateChanged: false, animIdx: 0, out};
        out.effectFns.push({
          name: `${name}_cleanup`,
          body: compileStmts(clist, cenv, state, cctx, '    '),
        });
      },
    };
    out.effectDecls.push(`static int ${armed};`);
  }
  const bodyLines = compileStmts(stmts, env, state, ctx, '    ');
  out.effectFns.push({
    name,
    body: hasCleanup
      ? [
          `    if (${armed})`,
          '    {',
          `        ${armed} = 0;`,
          `        ${name}_cleanup();`,
          '    }',
          ...bodyLines,
        ]
      : bodyLines,
  });
  // A static "previous value" per dep; snapshot at mount, then app_update detects changes against it.
  deps.forEach((d, j) =>
    out.effectDecls.push(
      d.cType === 'string'
        ? `static char s_eff${id}_d${j}[${LIST_STR_CAP}];`
        : `static ${cScalarType(d.cType)} s_eff${id}_d${j};`,
    ),
  );
  const snap = (j, d) =>
    d.cType === 'string'
      ? `snprintf(s_eff${id}_d${j}, sizeof(s_eff${id}_d${j}), "%s", ${d.code})`
      : `s_eff${id}_d${j} = ${d.code}`;
  out.mountEffects.push(
    `    ${name}();`,
    ...deps.map((d, j) => `    ${snap(j, d)};`),
  );
  const check = ['    {', '        int er_changed = 0;'];
  deps.forEach((d, j) => {
    if (d.cType === 'string')
      check.push(
        `        if (strcmp(s_eff${id}_d${j}, ${d.code}) != 0) { ${snap(j, d)}; er_changed = 1; }`,
      );
    else {
      const t = cScalarType(d.cType);
      check.push(
        `        ${t} er_d${j} = ${d.code}; if (er_d${j} != s_eff${id}_d${j}) { s_eff${id}_d${j} = er_d${j}; er_changed = 1; }`,
      );
    }
  });
  check.push(`        if (er_changed) ${name}();`, '    }');
  out.depEffects.push(check.join('\n'));
}

function compileHandler(fnNode, env, state, out, pan = null) {
  const body = fnNode.body;
  const list =
    body.type === 'BlockStatement'
      ? body.body
      : [{type: 'ExpressionStatement', expression: body}];
  // The handler's first parameter is the event; `<event>.x/.y/.dx/.dy/.vx/.vy` map to EREventData fields.
  // A PanResponder callback takes a second one, RN's gestureState — see panGestureField.
  const eventParam =
    fnNode.params[0]?.type === 'Identifier' ? fnNode.params[0].name : null;
  const gestureParam =
    pan && fnNode.params[1]?.type === 'Identifier'
      ? fnNode.params[1].name
      : null;
  let henv = env;
  if (eventParam) henv = {...henv, event: eventParam};
  if (gestureParam) henv = {...henv, gesture: gestureParam, pan};
  const ctx = {stateChanged: false, animIdx: 0, out};
  const stmts = compileStmts(list, henv, state, ctx, '    ');
  if (ctx.stateChanged) stmts.push('    app_update();'); // re-apply state-dependent props once
  return stmts;
}

// ---------------------------------------------------------------------------------------------------
// PanResponder (Flow B) — LOWERED onto the engine's responder system, not transpiled.
//
// RN's PanResponder is a JS state machine over raw touches. The engine already has that machine, in C:
// capture/bubble negotiation (er_responder_query_set) picks an owner, and GRANT / MOVE / RELEASE /
// TERMINATE then fire on the owner alone, carrying cumulative travel and velocity. So the should-set
// predicates become QUERIES and the callbacks become responder EVENT handlers — no gesture math is
// duplicated into the generated C, and the app gets real arbitration for free (a granted pan owns the
// gesture, so a ScrollView ancestor cannot auto-scroll out from under it).
//
// The one thing with no engine counterpart is RN's anchor: `g.dx` is measured from the GRANT, while the
// engine's `data->dx` runs from touch-down — a claim that needed 10 px of slop would otherwise open with
// a 10 px jump. That anchor, plus `x0`/`y0`, is a handful of file-scope ints per responder.
// ---------------------------------------------------------------------------------------------------

/** RN PanResponder config key → the engine responder QUERY it lowers to. */
const PAN_QUERIES = {
  onStartShouldSetPanResponder: 'ER_QUERY_START_SHOULD_SET',
  onStartShouldSetPanResponderCapture: 'ER_QUERY_START_SHOULD_SET_CAPTURE',
  onMoveShouldSetPanResponder: 'ER_QUERY_MOVE_SHOULD_SET',
  onMoveShouldSetPanResponderCapture: 'ER_QUERY_MOVE_SHOULD_SET_CAPTURE',
  onPanResponderTerminationRequest: 'ER_QUERY_TERMINATION_REQUEST',
};

/** Query constant → the suffix of the C function emitted for it. */
const PAN_QUERY_SUFFIX = {
  ER_QUERY_START_SHOULD_SET: 'start_should_set',
  ER_QUERY_START_SHOULD_SET_CAPTURE: 'start_should_set_capture',
  ER_QUERY_MOVE_SHOULD_SET: 'move_should_set',
  ER_QUERY_MOVE_SHOULD_SET_CAPTURE: 'move_should_set_capture',
  ER_QUERY_TERMINATION_REQUEST: 'termination_request',
};

/** RN PanResponder config key → the engine responder EVENT it lowers to. */
const PAN_EVENTS = {
  onPanResponderGrant: 'ER_EVENT_RESPONDER_GRANT',
  onPanResponderMove: 'ER_EVENT_RESPONDER_MOVE',
  onPanResponderRelease: 'ER_EVENT_RESPONDER_RELEASE',
  onPanResponderTerminate: 'ER_EVENT_RESPONDER_TERMINATE',
  onPanResponderReject: 'ER_EVENT_RESPONDER_REJECT',
};

/** Config keys the Flow A module acts on that have no Flow B lowering — rejected by name, not ignored. */
const PAN_FLOW_A_ONLY = {
  onPanResponderStart:
    'it reports EXTRA fingers joining a gesture already in flight; Flow B lowers one gesture per responder.',
  onPanResponderEnd:
    'it reports a finger lifting while others stay down; Flow B lowers one gesture per responder.',
};

/** True for a `PanResponder.create({…})` call. */
const isPanCreate = n =>
  n?.type === 'CallExpression' &&
  n.callee.type === 'MemberExpression' &&
  !n.callee.computed &&
  n.callee.object.type === 'Identifier' &&
  n.callee.object.name === 'PanResponder' &&
  n.callee.property.name === 'create';

/**
 * One `gestureState` field → the C expression that reads it. Travel is rebased onto the grant (RN's
 * anchor); the point and the velocity come straight off the engine payload; the finger count is the
 * engine's own, which is why er_touch_active_count() exists.
 */
function panGestureField(prop, pan) {
  switch (prop) {
    case 'dx':
      return {code: `(data->dx - ${pan.cPrefix}_base_dx)`, cType: 'int'};
    case 'dy':
      return {code: `(data->dy - ${pan.cPrefix}_base_dy)`, cType: 'int'};
    case 'moveX':
      return {code: 'data->x', cType: 'int'};
    case 'moveY':
      return {code: 'data->y', cType: 'int'};
    case 'x0':
      return {code: `${pan.cPrefix}_x0`, cType: 'int'};
    case 'y0':
      return {code: `${pan.cPrefix}_y0`, cType: 'int'};
    case 'vx':
    case 'vy':
      return {code: `data->${prop}`, cType: 'float'};
    case 'numberActiveTouches':
      return {code: 'er_touch_active_count()', cType: 'int'};
    case 'stateID':
      return {code: String(pan.stateID), cType: 'int'};
  }
  throw aotError(
    `AOT: unknown gestureState field "${prop}" in a PanResponder callback`,
    'a gestureState carries dx / dy / moveX / moveY / x0 / y0 / vx / vy / numberActiveTouches / stateID.',
  );
}

/** Validates one `PanResponder.create({…})` config and builds the descriptor the emitter works from. */
function buildPanDescriptor(name, createCall, prefix) {
  const cfg = createCall.arguments[0];
  if (!cfg || cfg.type !== 'ObjectExpression')
    throw aotError(
      'AOT: PanResponder.create(...) needs an object literal of callbacks',
      'write the config inline — `PanResponder.create({ onStartShouldSetPanResponder: () => true, … })` — so the AOT can see which callbacks exist at compile time.',
    );
  const queries = new Map();
  const events = new Map();
  for (const prop of cfg.properties) {
    if (prop.type !== 'ObjectProperty' && prop.type !== 'ObjectMethod')
      throw aotError(
        'AOT: a spread inside PanResponder.create({…}) is not supported',
        'list the callbacks explicitly.',
      );
    if (prop.computed)
      throw aotError(
        'AOT: a computed key in PanResponder.create({…}) is not supported',
        'name each callback literally — the AOT decides at compile time which engine query or event a key becomes.',
      );
    const key = prop.key.name ?? prop.key.value;
    // A shorthand method (`onPanResponderMove(e, g) { … }`) IS the function node; a property holds it.
    const fn = prop.type === 'ObjectMethod' ? prop : prop.value;
    if (PAN_FLOW_A_ONLY[key])
      throw aotError(
        `AOT: PanResponder "${key}" is not supported in Flow B`,
        `${PAN_FLOW_A_ONLY[key]} Use onPanResponderGrant / Release / Terminate instead, or build this screen for Flow A.`,
      );
    const query = PAN_QUERIES[key];
    const event = PAN_EVENTS[key];
    if (!query && !event)
      throw aotError(
        `AOT: unknown PanResponder config key "${key}"`,
        `supported: ${[...Object.keys(PAN_QUERIES), ...Object.keys(PAN_EVENTS)].join(', ')}.`,
      );
    if (prop.type !== 'ObjectMethod' && !isFn(fn))
      throw aotError(
        `AOT: PanResponder "${key}" must be a function`,
        'pass an inline arrow — `(e, g) => …` — so it can be compiled into the generated C.',
      );
    if (query) queries.set(query, fn);
    else events.set(event, fn);
  }
  return {
    name,
    cPrefix: `s_pan_${prefix}${name}`,
    fnPrefix: `er_pan_${prefix}${name}`,
    stateID: 0, // assigned when the responder is first wired to a node
    queries,
    events,
    emitted: null,
  };
}

/**
 * Collects `const pan = useRef(PanResponder.create({…})).current` → Map(name → descriptor). The
 * `useRef(…)` form (read back as `pan.current.panHandlers`) is accepted too; a bare `create(…)` is not,
 * because it would re-create the recogniser every render in Flow A and the two flows must agree.
 */
function collectPanResponders(fnBody, prefix = '') {
  const pans = new Map();
  if (fnBody.type !== 'BlockStatement') return pans;
  for (const stmt of fnBody.body) {
    if (stmt.type !== 'VariableDeclaration') continue;
    for (const decl of stmt.declarations) {
      const init = decl.init;
      if (decl.id.type !== 'Identifier' || !init) continue;
      const viaCurrent =
        init.type === 'MemberExpression' &&
        !init.computed &&
        init.property.name === 'current' &&
        init.object.type === 'CallExpression' &&
        init.object.callee.name === 'useRef'
          ? init.object
          : null;
      const viaRef =
        init.type === 'CallExpression' && init.callee.name === 'useRef'
          ? init
          : null;
      const useRefCall = viaCurrent || viaRef;
      if (!useRefCall || !isPanCreate(useRefCall.arguments[0])) {
        if (isPanCreate(init))
          throw aotError(
            'AOT: PanResponder.create(...) must be kept in a useRef',
            `write \`const ${decl.id.name} = useRef(PanResponder.create({…})).current;\` — the recogniser owns the live gesture, so a fresh one per render would throw the drag away (the same rule Flow A enforces).`,
          );
        continue;
      }
      const pan = buildPanDescriptor(
        decl.id.name,
        useRefCall.arguments[0],
        prefix,
      );
      pan.viaRefCurrent = !viaCurrent; // how the app spells the spread: pan.current.panHandlers
      pans.set(decl.id.name, pan);
    }
  }
  return pans;
}

/**
 * Lowers a should-set predicate to a single C boolean expression: the engine calls these synchronously
 * inside hit-testing, before anyone owns the gesture, so they may only READ.
 */
function compilePanQuery(fnNode, env, pan) {
  const eventParam =
    fnNode.params[0]?.type === 'Identifier' ? fnNode.params[0].name : null;
  const gestureParam =
    fnNode.params[1]?.type === 'Identifier' ? fnNode.params[1].name : null;
  const qenv = {...env, pan};
  if (eventParam) qenv.event = eventParam;
  if (gestureParam) qenv.gesture = gestureParam;
  const body = fnNode.body;
  const expr =
    body.type === 'BlockStatement'
      ? body.body.length === 1 && body.body[0].type === 'ReturnStatement'
        ? body.body[0].argument
        : null
      : body;
  if (!expr)
    throw aotError(
      'AOT: a PanResponder should-set predicate must be a single boolean expression',
      'write it as `() => true` or `(e, g) => Math.abs(g.dx) > 8`. The engine asks these mid-hit-test, so they may only read (state, event/gesture fields, constants) — never set state.',
    );
  return `(${emitExpr(expr, qenv).code}) != 0`;
}

/**
 * Emits one PanResponder's gesture state + C callbacks (once, however many nodes spread it), then wires
 * them onto a node. Grant/release/terminate are ALWAYS emitted even with no user callback: they own the
 * anchor bookkeeping that makes `g.dx` grant-relative.
 */
function emitPanResponder(pan, v, out, env, state) {
  if (!pan.emitted) {
    pan.stateID = ++out.panN;
    const p = pan.cPrefix;
    const penv = {...env, pan};
    const userBody = evt =>
      pan.events.has(evt)
        ? compileHandler(pan.events.get(evt), penv, state, out, pan)
        : [];
    const guard = [
      '    if (!' + p + '_granted)',
      '    {',
      '        return;',
      '    }',
    ];

    out.panDecls.push(
      `/* PanResponder "${pan.name}" (gesture ${pan.stateID}) — RN's gestureState. The grant anchors the`,
      `   travel so g.dx opens at 0 however much slop the claim cost; data->dx runs from touch-down. */`,
      `static int ${p}_granted = 0;`,
      `static int ${p}_base_dx = 0;`,
      `static int ${p}_base_dy = 0;`,
      `static int ${p}_x0 = 0;`,
      `static int ${p}_y0 = 0;`,
    );

    pan.emitted = {events: [], queries: []};
    const push = (evt, suffix, body) => {
      const name = `${pan.fnPrefix}_${suffix}`;
      out.handlers.push({name, body});
      pan.emitted.events.push([evt, name]);
    };
    push('ER_EVENT_RESPONDER_GRANT', 'grant', [
      `    if (${p}_granted)`,
      '    {',
      '        return; /* a second finger re-granted this node: one gesture, not two */',
      '    }',
      `    ${p}_granted = 1;`,
      `    ${p}_base_dx = data->dx;`,
      `    ${p}_base_dy = data->dy;`,
      `    ${p}_x0 = data->x;`,
      `    ${p}_y0 = data->y;`,
      ...userBody('ER_EVENT_RESPONDER_GRANT'),
    ]);
    // Release and terminate both END the gesture: run the app's callback while the anchor is still
    // valid (it is what g.dx reads through), then clear it for the next one.
    for (const [evt, suffix] of [
      ['ER_EVENT_RESPONDER_RELEASE', 'release'],
      ['ER_EVENT_RESPONDER_TERMINATE', 'terminate'],
    ])
      push(evt, suffix, [
        ...guard,
        ...userBody(evt),
        `    ${p}_granted = 0;`,
        `    ${p}_base_dx = 0;`,
        `    ${p}_base_dy = 0;`,
      ]);
    if (pan.events.has('ER_EVENT_RESPONDER_MOVE'))
      push('ER_EVENT_RESPONDER_MOVE', 'move', [
        ...guard,
        ...userBody('ER_EVENT_RESPONDER_MOVE'),
      ]);
    // Reject fires on a node that ASKED for the gesture and was refused — it never owned it, so no guard.
    if (pan.events.has('ER_EVENT_RESPONDER_REJECT'))
      push(
        'ER_EVENT_RESPONDER_REJECT',
        'reject',
        userBody('ER_EVENT_RESPONDER_REJECT'),
      );

    for (const [query, fn] of pan.queries) {
      const name = `${pan.fnPrefix}_${PAN_QUERY_SUFFIX[query]}`;
      out.queries.push({name, expr: compilePanQuery(fn, penv, pan)});
      pan.emitted.queries.push([query, name]);
    }
  }

  for (const [evt, name] of pan.emitted.events)
    out.build.push(`    er_event_set(${v}, ${evt}, ${name}, NULL);`);
  for (const [query, name] of pan.emitted.queries)
    out.build.push(
      `    er_responder_query_set(${v}, ${query}, ${name}, NULL);`,
    );
}

/**
 * Resolves a JSX spread to the PanResponder it spreads, or null when it is some other spread. A name
 * that IS a responder but is spelled the other way round (`.current` where the app already unwrapped it,
 * or vice versa) is an error rather than a silent miss — Flow A would throw at runtime on the same code.
 */
function panSpreadTarget(argument, env) {
  if (
    argument?.type !== 'MemberExpression' ||
    argument.computed ||
    argument.property.name !== 'panHandlers'
  )
    return null;
  const obj = argument.object;
  const viaCurrent =
    obj.type === 'MemberExpression' &&
    !obj.computed &&
    obj.property.name === 'current' &&
    obj.object.type === 'Identifier';
  const rootName = viaCurrent ? obj.object.name : obj.name;
  const pan =
    obj.type === 'Identifier' || viaCurrent ? env.pans?.get(rootName) : null;
  if (!pan) return null;
  if (pan.viaRefCurrent !== viaCurrent)
    throw aotError(
      `AOT: "${rootName}" is a PanResponder, but this spread does not match how it was declared`,
      pan.viaRefCurrent
        ? `it was declared as \`useRef(PanResponder.create({…}))\`, so spread \`{...${rootName}.current.panHandlers}\`.`
        : `it was declared as \`useRef(PanResponder.create({…})).current\`, so spread \`{...${rootName}.panHandlers}\`.`,
    );
  return pan;
}

// ---------------------------------------------------------------------------------------------------
// Emit — control flow (components / conditionals / lists) all unroll at COMPILE TIME into fixed nodes.
// Runtime-dynamic conditionals/lists (where the node COUNT changes with state) are not yet supported
// and throw a clear "AOT: ..." — see the root README (Flow B).
// ---------------------------------------------------------------------------------------------------

/** Reads a component instance's props (attributes) as static values; dynamic props throw (for now). */
/** Reads a component's props as descriptors: `{static:true,value}` (folded) or `{static:false,code,cType,struct}`
 *  (a runtime C expression — e.g. a list row's `item.field`). */
function extractProps(openingElement, scope, env) {
  const props = {};
  for (const attr of openingElement.attributes) {
    if (attr.type === 'JSXSpreadAttribute') {
      // Static spread: {...obj} where obj folds to a compile-time object → merge its keys as props.
      if (panSpreadTarget(attr.argument, env))
        throw aotError(
          'AOT: a PanResponder can only be spread onto a host element',
          'spread `{...pan.panHandlers}` onto the <View> (or <Pressable>/<ScrollView>) that should own the gesture, not onto a component instance — the AOT wires the responder to a real scene node.',
        );
      let obj;
      try {
        obj = evalStatic(attr.argument, scope);
      } catch {
        throw new Error(
          'AOT: only a compile-time-constant object can be spread to a component ({...obj})',
        );
      }
      if (obj == null || typeof obj !== 'object')
        throw new Error(
          'AOT: a component spread {...x} must resolve to an object',
        );
      for (const [k, v] of Object.entries(obj))
        props[k] = {static: true, value: v};
      continue;
    }
    if (attr.type !== 'JSXAttribute' || attr.name.name === 'key') continue;
    const node = attrExpr(attr);
    // Callback prop: a function passed to a child (inline arrow, or an identifier bound to a useCallback in
    // the caller). Captured as a `fn` descriptor and resolved where the child uses it as an event handler.
    if (isFn(node)) {
      props[attr.name.name] = {fn: true, node};
      continue;
    }
    if (node.type === 'Identifier' && env.callbacks?.has(node.name)) {
      props[attr.name.name] = {fn: true, node: env.callbacks.get(node.name)};
      continue;
    }
    if (node.type === 'Identifier' && env.fnProps?.has(node.name)) {
      props[attr.name.name] = {fn: true, ...env.fnProps.get(node.name)}; // forward original {node, env, state}
      continue;
    }
    try {
      props[attr.name.name] = {static: true, value: evalStatic(node, scope)};
    } catch {
      props[attr.name.name] = {static: false, ...emitExprWide(node, env)};
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
      if (!d.static)
        throw new Error(
          'AOT: dynamic props require a destructured component parameter (e.g. `function C({ x })`)',
        );
      obj[k] = d.value;
    }
    out.set(param.name, {static: true, value: obj});
  } else if (param.type === 'ObjectPattern') {
    for (const p of param.properties) {
      if (p.type === 'RestElement')
        throw new Error(
          'AOT: rest props (...rest) in a component param not supported',
        );
      const propName = p.key.name ?? p.key.value;
      const bindName =
        p.value?.type === 'Identifier'
          ? p.value.name
          : p.value?.type === 'AssignmentPattern'
            ? p.value.left.name
            : propName;
      let d = props[propName];
      if (!d && p.value?.type === 'AssignmentPattern')
        d = {static: true, value: evalStatic(p.value.right, {})};
      out.set(bindName, d ?? {static: true, value: undefined});
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
  if (cr.kind === 'local')
    return expr.type === 'Identifier' && expr.name === cr.name;
  return (
    expr.type === 'MemberExpression' &&
    !expr.computed &&
    expr.object.type === 'Identifier' &&
    expr.object.name === cr.name &&
    expr.property.name === 'children'
  );
}

/** Inlines a function component instance: bind props (static → scope, dynamic → locals), emit its JSX.
 *  Children passed at the call site are captured and emitted (in the CALLER's scope) where the body uses them. */
function emitComponent(el, scope, out, env, state, opts) {
  const tag = el.openingElement.name.name;
  const fn = out.components.get(tag);
  const childNodes = el.children.filter(
    c =>
      c.type === 'JSXElement' ||
      (c.type === 'JSXExpressionContainer' &&
        c.expression.type !== 'JSXEmptyExpression'),
  );

  // How the body refers to children: destructured `{ children }` (a local) or whole `props` → props.children.
  const param = fn.params[0];
  let childrenRef = null;
  if (param?.type === 'ObjectPattern') {
    for (const p of param.properties)
      if ((p.key?.name ?? p.key?.value) === 'children')
        childrenRef = {kind: 'local', name: p.value?.name ?? 'children'};
  } else if (param?.type === 'Identifier') {
    childrenRef = {kind: 'props', name: param.name};
  }

  // A child component is a module-level function: its body resolves names against MODULE scope, not the
  // caller's locals. Copying the caller's scope here would let an App-local const shadow a module one
  // inside the child, which JavaScript never does.
  const childScope = {...(env.moduleConsts ?? scope)};
  // A child is a module-level function: it closes over module scope and receives everything else
  // through props. Starting from the caller's locals let App's memos and dynamic consts leak in —
  // and then masked the child's own `const` of the same name.
  const childLocals = new Map();
  // Callback props bound here resolve to the CALLER's function (node + caller env/state) so the child can
  // use them as event handlers (onPress={onTap}); inherit any the caller itself received (forwarding).
  const fnProps = new Map(env.fnProps);
  for (const [name, d] of bindParams(
    fn,
    extractProps(el.openingElement, scope, env),
  )) {
    if (childrenRef?.kind === 'local' && name === childrenRef.name) continue; // children come from the slot, not a value prop
    if (d.fn)
      fnProps.set(name, {
        node: d.node,
        env: d.env ?? env,
        state: d.state ?? state,
      });
    else if (d.static) childScope[name] = d.value;
    else {
      childLocals.set(name, {
        code: d.code,
        cType: d.cType,
        struct: d.struct,
        isBool: d.isBool, // keep boolean-ness across the prop boundary (extractProps supplies it)
      });
      // A dynamic prop is a RUNTIME binding. Every constant fold in the child (styles, colors, enums,
      // svg attrs, text) consults childScope first, so a module const of the same name must not be
      // left there to win — the fold would silently emit the module value instead of the prop.
      delete childScope[name];
    }
  }
  const children = childNodes.length
    ? {nodes: childNodes, scope, env, ref: childrenRef}
    : null;

  // Per-instance hooks: a child component is inlined, so EACH instance gets its OWN state, refs, animated
  // values, callbacks, memos and mount-effects — namespaced by a unique prefix (`c<N>_`) so two instances
  // (e.g. two animated <Card/>s) stay fully independent. The child sees ITS OWN hooks (not the parent's),
  // exactly like a React component — it receives everything else through props. All initials/values fold
  // against the child's static-prop scope. prefix is per-instance; the App keeps the bare (unprefixed) names.
  const prefix = `c${out.instN++}_`;
  // Fold the child's own statically-derived body consts, the way compileSourceImpl does for App. Without
  // this a `const L = …` in a child body either failed to resolve or, when a module const shared the
  // name, silently rendered the MODULE value — the child's declaration must shadow it.
  if (fn.body.type === 'BlockStatement') {
    // As in App: the body's own names, hook bindings included, shadow module ones before anything folds.
    for (const name of declaredNames(fn.body.body)) delete childScope[name];
    for (const stmt of fn.body.body) {
      if (stmt.type !== 'VariableDeclaration' || stmt.kind !== 'const')
        continue;
      for (const decl of stmt.declarations) {
        if (decl.id.type !== 'Identifier' || !decl.init) continue;
        if (childLocals.has(decl.id.name)) continue; // a dynamic prop of that name is already bound
        try {
          childScope[decl.id.name] = evalStatic(
            decl.init,
            withUndefined(childScope),
          );
        } catch {
          // Dynamic (state-derived, useMemo, …): the child's binding shadows any module const of the
          // same name, so that const must not stay visible — a hook initializer or text reading it would
          // silently get the module value. A memo re-binds it below; anything else is an unresolved name.
          delete childScope[decl.id.name];
        }
      }
    }
  }

  const childAnims = collectAnims(fn.body, childScope, prefix);
  const childRefs = collectRefs(fn.body, childScope, prefix, env.wide);
  const childPans = collectPanResponders(fn.body, prefix);
  const childCallbacks = collectCallbacks(fn.body);
  const childMemos = collectMemos(fn.body);
  let childState = state;
  if (usesState(fn)) {
    childState = collectState(fn.body, childScope, prefix, env.wide);
    out.childStateRecords.push(...childState.byName.values());
    for (const name of childState.byName.keys()) delete childScope[name];
  }
  // The child's own refs and animated values are runtime bindings too (see compileSourceImpl).
  for (const name of [...childAnims.keys(), ...childRefs.keys()])
    delete childScope[name];
  out.childRefs.push(...childRefs.values());
  out.childAnims.push(...childAnims.values());

  const childEnv = {
    ...env,
    consts: childScope,
    locals: childLocals,
    children,
    fnProps,
    state: childState.byName,
    anims: childAnims,
    refs: childRefs,
    pans: childPans,
    callbacks: childCallbacks,
    helpers: collectHelpers(fn.body, out.program),
    cbPrefix: prefix,
  };

  // Resolve the child's memos in declaration order (fold to a const, else inline as a local C expr), then
  // run its mount-once useEffect(fn, []) bodies — both compiled in the child's own env/state.
  for (const [name, expr] of childMemos) {
    try {
      childScope[name] = evalStatic(expr, childScope);
    } catch {
      const e = emitExprWide(expr, childEnv);
      childLocals.set(name, {
        code: `(${e.code})`,
        cType: e.cType,
        isBool: e.isBool,
      });
      delete childScope[name]; // a runtime binding beats a module const of its name in every fold
    }
  }
  for (const eff of collectEffects(fn.body)) {
    compileEffect(eff, childEnv, childState, out);
  }

  return emitNode(
    componentReturnJSX(fn, childScope),
    childScope,
    out,
    childEnv,
    childState,
    opts,
  );
}

/** Emits an element / component child and appends it to the parent. opts.displayCode toggles its show. */
function emitElementInto(node, parentVar, scope, out, env, state, opts) {
  if (node.type !== 'JSXElement')
    throw new Error(`AOT: expected a JSX element here, got ${node.type}`);
  const cv = emitNode(node, scope, out, env, state, opts);
  out.build.push(`    er_tree_append_child(${parentVar}, ${cv});`);
}

/** Unrolls `arr.map((item, i) => <JSX/>)` over a COMPILE-TIME-CONSTANT array. */
function emitMap(call, parentVar, scope, out, env, state) {
  let arr;
  try {
    arr = evalStatic(call.callee.object, scope);
  } catch {
    throw new Error(
      'AOT: .map target must be a compile-time-constant array (dynamic lists not yet supported)',
    );
  }
  if (!Array.isArray(arr))
    throw new Error('AOT: .map target did not resolve to an array');
  const cb = call.arguments[0];
  if (!isFn(cb))
    throw new Error('AOT: .map argument must be an inline function');
  const itemName = cb.params[0]?.name;
  const idxName = cb.params[1]?.name;
  const retJSX = componentReturnJSX(cb);
  arr.forEach((item, i) => {
    const iterScope = {...scope};
    if (itemName) iterScope[itemName] = item;
    if (idxName) iterScope[idxName] = i;
    emitElementInto(
      retJSX,
      parentVar,
      iterScope,
      out,
      rowEnv(env, [itemName, idxName], iterScope),
      state,
    );
  });
}

/**
 * The env for one `.map` row. Its callback params (item, index) shadow every outer binding of the same
 * name — a JS arrow parameter always does — so those names leave every name-keyed map (state, locals,
 * callbacks, helpers, …) and the row's own binding wins: in `consts`, or in `ownLocals` for a pooled
 * row's struct item.
 */
function rowEnv(env, params, consts, ownLocals = null) {
  const names = params.filter(Boolean);
  const drop = m => {
    if (!m || !names.some(n => m.has(n))) return m;
    const c = new Map(m);
    for (const n of names) c.delete(n);
    return c;
  };
  return {
    ...env,
    consts,
    state: drop(env.state),
    refs: drop(env.refs),
    anims: drop(env.anims),
    locals: ownLocals ?? drop(env.locals),
    callbacks: drop(env.callbacks),
    fnProps: drop(env.fnProps),
    pans: drop(env.pans),
    helpers: drop(env.helpers),
    svgImports: drop(env.svgImports),
    children: names.includes(env.children?.ref?.name) ? null : env.children,
  };
}

/**
 * `{listState.map((item, i) => <Row/>)}` over a STATE array of variable length. Pre-allocates a fixed
 * pool of `cap` rows (no runtime malloc); each row k binds `item` to `s_<name>[k]` (a struct local) and
 * is shown only while `k < count` (display toggle). app_update then drives every row's content and show.
 */
function emitDynamicMap(call, rec, parentVar, scope, out, env, state) {
  const cb = call.arguments[0];
  if (!isFn(cb))
    throw new Error('AOT: .map argument must be an inline function');
  const itemName = cb.params[0]?.name;
  const idxName = cb.params[1]?.name;
  const retJSX = componentReturnJSX(cb);
  for (let k = 0; k < rec.cap; k++) {
    const iterScope = {...scope};
    // The item is a runtime local (a struct slot), so a same-named module const must not shadow it
    // in any fold inside the row — see the dynamic-prop note in emitComponent.
    if (itemName) delete iterScope[itemName];
    if (idxName) iterScope[idxName] = k; // the index is a compile-time literal per pooled row
    const locals = new Map(env.locals);
    if (idxName) locals.delete(idxName); // the index is a per-row literal in iterScope
    if (itemName)
      locals.set(itemName, {
        code: `${rec.arrayName}[${k}]`,
        struct: rec.struct,
      });
    emitElementInto(
      retJSX,
      parentVar,
      iterScope,
      out,
      rowEnv(env, [itemName, idxName], iterScope, locals),
      state,
      {
        displayCode: `(${k} < ${rec.countMember})`,
      },
    );
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
        emitChildren(
          env.children.nodes,
          parentVar,
          env.children.scope,
          out,
          env.children.env,
          state,
        );
        continue;
      }
      if (expr.type === 'LogicalExpression' && expr.operator === '&&') {
        // `{cond && <X/>}`. Static cond → include/omit at compile time. Dynamic (state) cond → always
        // build X but toggle its display (none/flex) in app_update — show/hide without node churn.
        let cond;
        try {
          cond = evalStatic(expr.left, scope);
          if (cond)
            emitElementInto(expr.right, parentVar, scope, out, env, state);
        } catch {
          const code = asCond(emitExpr(expr.left, env));
          emitElementInto(expr.right, parentVar, scope, out, env, state, {
            displayCode: code,
          });
        }
      } else if (
        expr.type === 'ConditionalExpression' &&
        (expr.consequent.type === 'JSXElement' ||
          expr.alternate.type === 'JSXElement')
      ) {
        // `{cond ? <A/> : <B/>}`. Static cond picks a branch; dynamic cond builds both and toggles each.
        let test;
        try {
          test = evalStatic(expr.test, scope);
          emitElementInto(
            test ? expr.consequent : expr.alternate,
            parentVar,
            scope,
            out,
            env,
            state,
          );
        } catch {
          const code = asCond(emitExpr(expr.test, env));
          if (expr.consequent.type === 'JSXElement')
            emitElementInto(
              expr.consequent,
              parentVar,
              scope,
              out,
              env,
              state,
              {displayCode: code},
            );
          if (expr.alternate.type === 'JSXElement')
            emitElementInto(expr.alternate, parentVar, scope, out, env, state, {
              displayCode: `!(${code})`,
            });
        }
      } else if (
        expr.type === 'CallExpression' &&
        expr.callee.type === 'MemberExpression' &&
        expr.callee.property.name === 'map'
      ) {
        const obj = expr.callee.object;
        const rec = obj.type === 'Identifier' ? env.state.get(obj.name) : null;
        if (rec?.kind === 'list')
          emitDynamicMap(expr, rec, parentVar, scope, out, env, state);
        else emitMap(expr, parentVar, scope, out, env, state);
      } else {
        // A constant that renders nothing (false/null/'') is fine; anything else is unsupported.
        let v;
        try {
          v = evalStatic(expr, scope);
        } catch {
          const e = aotError(
            `AOT: unsupported expression child "${expr.type}" in a container`,
            "a child expression must be a JSX element, `cond && <El/>` / a ternary of elements, or `list.map(item => <El/>)`. A bare variable holding JSX isn't inlined — write the element directly. (If this is a responsive Flow-A-only branch, compile at the target board size via ER_AOT_SCREEN_W/H so the AOT folds the supported branch.)",
          );
          if (expr.loc) e.aotLoc = expr.loc.start;
          throw e;
        }
        if (v !== false && v != null && v !== '') {
          const e = aotError(
            `AOT: a non-element expression child (${JSON.stringify(v)}) cannot render here`,
            'only JSX elements render as children; wrap text in a <Text>{…}</Text>.',
          );
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

/**
 * Yields an <Svg> subtree's shape children in source order, inlining any `<>…</>` in place: a fragment
 * carries no paint and no transform, so it is transparent to the tape (the Flow A walk in flattenSvg
 * treats it the same way). Whitespace between elements and JSX comments are skipped; anything else
 * that has no shape to contribute throws, rather than vanishing from the generated C.
 */
function* svgShapeChildren(children) {
  for (const c of children) {
    if (c.type === 'JSXElement') yield c;
    else if (c.type === 'JSXFragment') yield* svgShapeChildren(c.children);
    else if (c.type === 'JSXText') {
      if (c.value.trim())
        throw new Error(
          `AOT: <Svg> cannot draw text — remove ${JSON.stringify(c.value.trim())} from the subtree`,
        );
    } else if (c.type === 'JSXExpressionContainer') {
      if (c.expression.type !== 'JSXEmptyExpression')
        throw new Error(
          'AOT: dynamic <Svg> children ({…}) not yet supported — use literal shape elements',
        );
    } else {
      throw new Error(`AOT: unsupported <Svg> child (${c.type})`);
    }
  }
}

/** Converts an SVG JSX element (Svg/Circle/Path/Rect/Line/Arc/G/…) to flattenSvg's `{type, props}` shape,
 *  statically evaluating every attribute. Dynamic attrs/children throw (a state-driven Svg is not supported). */
function jsxToSvgElement(node, scope) {
  if (node.type !== 'JSXElement') return null;
  const type = node.openingElement.name.name;
  const props = {};
  for (const attr of node.openingElement.attributes) {
    if (attr.type !== 'JSXAttribute')
      throw new Error(
        'AOT: spread attributes on an <Svg> element not supported',
      );
    const name = attr.name.name;
    if (name === 'ref' || name === 'key') continue; // not geometry/paint
    if (attr.value == null) props[name] = true;
    else if (attr.value.type === 'StringLiteral')
      props[name] = attr.value.value;
    else if (attr.value.type === 'JSXExpressionContainer')
      props[name] = evalStatic(attr.value.expression, scope);
    else
      throw new Error(
        `AOT: unsupported <${type}> attribute value for "${name}"`,
      );
  }
  const children = [];
  for (const c of svgShapeChildren(node.children))
    children.push(jsxToSvgElement(c, scope));
  if (children.length) props.children = children;
  return {type, props};
}

/** Emits one ERVectorPaint initializer from a flattenSvg paint record
 *  [fill,stroke,w,miter,cap,join,rule, fill_grad, stroke_grad]. fill_grad/stroke_grad (1-based gradient-table
 *  indices, 0 = solid) are absent on inline-<Svg> records (7-wide) → zero, and set on baked <Svg source>. */
function emitVectorPaint(p) {
  return `{ .fill = ${p[0] >>> 0}u, .stroke = ${p[1] >>> 0}u, .stroke_w = ${floatLit(p[2])}, .miter = ${floatLit(p[3])}, .cap = ${p[4] | 0}, .join = ${p[5] | 0}, .fill_rule = ${p[6] | 0}, .fill_grad = ${(p[7] || 0) | 0}, .stroke_grad = ${(p[8] || 0) | 0} }`;
}

/** Emits one ERVectorGradient initializer from a baked artifact gradient
 *  { type, stops:[{color, offset}], ax, ay, bx, by, r }. Stops fill the C ERGradientStop[] positionally
 *  ({color, position}); the engine zero-inits the rest of stops[ER_VGRAD_MAX_STOPS]. Geometry meaning per
 *  type: linear axis (ax,ay)->(bx,by); radial centre (ax,ay)+radius r; conic centre (ax,ay)+start angle r. */
function emitVectorGradient(g) {
  const inStops = g.stops || [];
  const capped = inStops.length > 8 ? inStops.slice(0, 8) : inStops;
  const stops = capped
    .map(s => `{ ${s.color >>> 0}u, ${floatLit(s.offset)} }`)
    .join(', ');
  return (
    `{ .type = ${g.type | 0}, .stop_count = ${capped.length}, .stops = { ${stops} }, ` +
    `.ax = ${floatLit(g.ax || 0)}, .ay = ${floatLit(g.ay || 0)}, .bx = ${floatLit(g.bx || 0)}, ` +
    `.by = ${floatLit(g.by || 0)}, .r = ${floatLit(g.r || 0)} }`
  );
}

/** Static numeric coercion for an SVG attribute value (mirrors svg-ops `num`). */
const svgNum = (v, d = 0) => {
  const n = typeof v === 'number' ? v : parseFloat(v);
  return Number.isNaN(n) ? d : n;
};
/** True if an attribute value is a state-driven C expression (vs a static number/string). */
const isDyn = v => v != null && typeof v === 'object' && 'dyn' in v;
/** Lowers an SVG coordinate attr to a C float expression (literal when static, cast expr when dynamic). */
const cf = (v, d = 0) =>
  isDyn(v) ? `(float)(${v.dyn})` : floatLit(svgNum(v, d));

/** Reads an SVG element's attributes → { name: number|string|true | {dyn: cExpr} } (state attrs → {dyn}). */
function svgAttrs(openingElement, scope, env) {
  const out = {};
  for (const attr of openingElement.attributes) {
    if (attr.type !== 'JSXAttribute')
      throw new Error(
        'AOT: spread attributes on an <Svg> element not supported',
      );
    const name = attr.name.name;
    if (name === 'ref' || name === 'key') continue; // not geometry/paint
    const vn = attr.value;
    if (name === 'fillGrad' || name === 'strokeGrad') {
      if (vn == null || vn.type !== 'JSXExpressionContainer')
        throw new Error(`AOT: "${name}" must be an object expression`);
      out[name] = {gradNode: vn.expression};
      continue;
    }
    if (vn == null) out[name] = true;
    else if (vn.type === 'StringLiteral') out[name] = vn.value;
    else if (vn.type === 'JSXExpressionContainer') {
      try {
        out[name] = evalStatic(vn.expression, withUndefined(scope));
      } catch {
        // A state-driven `d` is unsupported whatever it is built from, and pathEntries carries the
        // diagnostic that names the fix (use Arc/Circle/Rect/Line). Emitting it first would replace that
        // with whatever generic reason the expression itself fails for.
        if (name === 'd') {
          out[name] = {dyn: null, node: vn.expression};
          continue;
        }
        // Keep the raw expression node too: color paint attrs (fill/stroke) lower via emitColorExpr (→ ARGB),
        // not the generic numeric `dyn` code, so a dynamic color resolves to a uint, not a char*.
        out[name] = {
          dyn: emitExpr(vn.expression, env).code,
          node: vn.expression,
        };
      }
    } else
      throw new Error(`AOT: unsupported SVG attribute value for "${name}"`);
  }
  return out;
}

/**
 * Lowers a `{ type, ax, ay, bx, by, r, stops: [{ color, offset }] }` gradient descriptor — the SAME shape
 * Flow A's svg-ops.js takes — to C-expression fields. Every geometry field and every stop may be static
 * or state-driven; `type` and the stop COUNT must be static, since they decide the emitted table's shape.
 *
 * Conic gradients carry the sweep's start angle in `r` (radians, clockwise from the top), so a dial whose
 * ramp must follow a setpoint drives `r` from state — which is exactly why the table can't be const.
 */
function gradSpec(node, scope, env, what) {
  if (!node || node.type !== 'ObjectExpression')
    throw new Error(`AOT: "${what}" must be an object literal`);
  const props = {};
  for (const pr of node.properties) {
    if (pr.type !== 'ObjectProperty' || pr.computed)
      throw new Error(`AOT: "${what}" takes plain key: value pairs only`);
    props[pr.key.name ?? pr.key.value] = pr.value;
  }
  let anyDynamic = false;
  // A numeric field: fold when it can be, otherwise emit the state-driven expression.
  const numf = (v, dflt) => {
    if (v == null) return floatLit(dflt);
    try {
      return floatLit(evalStatic(v, scope));
    } catch {
      anyDynamic = true;
      return `(float)(${emitExpr(v, env).code})`;
    }
  };
  let type;
  try {
    type = evalStatic(props.type, scope) | 0;
  } catch {
    throw new Error(
      `AOT: "${what}.type" must be a compile-time constant (1 linear, 2 radial, 3 conic)`,
    );
  }
  if (type < 1 || type > 3)
    throw new Error(
      `AOT: "${what}.type" must be 1 (linear), 2 (radial) or 3 (conic)`,
    );
  const stopsNode = props.stops;
  if (!stopsNode || stopsNode.type !== 'ArrayExpression')
    throw new Error(`AOT: "${what}.stops" must be an array literal`);
  if (
    stopsNode.elements.length < 2 ||
    stopsNode.elements.length > GRAD_MAX_STOPS
  )
    throw new Error(
      `AOT: "${what}.stops" needs 2..${GRAD_MAX_STOPS} entries (got ${stopsNode.elements.length})`,
    );
  const stops = stopsNode.elements.map(e => {
    if (!e || e.type !== 'ObjectExpression')
      throw new Error(
        `AOT: each "${what}.stops" entry must be an object literal`,
      );
    const sp = {};
    for (const pr of e.properties) sp[pr.key.name ?? pr.key.value] = pr.value;
    let color;
    try {
      color = `${parseColor(evalStatic(sp.color, scope)) >>> 0}u`;
    } catch {
      anyDynamic = true;
      color = emitColorExpr(sp.color, env);
    }
    return {color, offset: numf(sp.offset, 0)};
  });
  return {
    type,
    stops,
    ax: numf(props.ax, 0),
    ay: numf(props.ay, 0),
    bx: numf(props.bx, 0),
    by: numf(props.by, 0),
    r: numf(props.r, 0),
    anyDynamic,
  };
}

/**
 * Unwraps a gradient attribute, which may be a bare object literal or a CONDITIONAL one:
 * `cond ? {…} : null` (either way round) or `cond && {…}`. The gradient table entry is emitted either
 * way; what the condition drives is the PAINT'S INDEX, which becomes a runtime ternary of N or 0.
 *
 * Without this a gradient applied to a shape that is only sometimes gradient-filled leaks into every
 * other state — the index is a compile-time constant, so "no gradient here" is not expressible by
 * omission. The thermostat hit exactly that: its Auto ramp painted over Cool and Heat as well.
 */
function gradAttr(v, scope, env, what) {
  if (!v) return null;
  let node = v.gradNode;
  let cond = null;
  const nullish = n =>
    n.type === 'NullLiteral' ||
    (n.type === 'Identifier' && n.name === 'undefined');
  if (node.type === 'ConditionalExpression') {
    if (
      node.consequent.type === 'ObjectExpression' &&
      nullish(node.alternate)
    ) {
      cond = asCond(emitExpr(node.test, env));
      node = node.consequent;
    } else if (
      node.alternate.type === 'ObjectExpression' &&
      nullish(node.consequent)
    ) {
      cond = `!(${asCond(emitExpr(node.test, env))})`;
      node = node.alternate;
    } else
      throw new Error(
        `AOT: a conditional "${what}" must be \`cond ? { … } : null\` (one branch an object literal, the other null)`,
      );
  } else if (
    node.type === 'LogicalExpression' &&
    node.operator === '&&' &&
    node.right.type === 'ObjectExpression'
  ) {
    cond = asCond(emitExpr(node.left, env));
    node = node.right;
  }
  const spec = gradSpec(node, scope, env, what);
  spec.cond = cond;
  return spec;
}

/** A `{ .type = …, .stops = { … }, … }` ERVectorGradient initializer from a gradSpec (const tables). */
function gradInitFromSpec(g) {
  const stops = g.stops.map(st => `{ ${st.color}, ${st.offset} }`).join(', ');
  return (
    `{ .type = ${g.type}, .stop_count = ${g.stops.length}, .stops = { ${stops} }, ` +
    `.ax = ${g.ax}, .ay = ${g.ay}, .bx = ${g.bx}, .by = ${g.by}, .r = ${g.r} }`
  );
}

/** Per-field assignments rebuilding one mutable gradient-table entry from state, for build_svgN(). */
function gradAssigns(tab, i, g) {
  const out = [
    `    ${tab}[${i}].type = ${g.type};`,
    `    ${tab}[${i}].stop_count = ${g.stops.length};`,
  ];
  g.stops.forEach((st, si) => {
    out.push(`    ${tab}[${i}].stops[${si}].color = ${st.color};`);
    out.push(`    ${tab}[${i}].stops[${si}].position = ${st.offset};`);
  });
  for (const f of ['ax', 'ay', 'bx', 'by', 'r'])
    out.push(`    ${tab}[${i}].${f} = ${g[f]};`);
  return out;
}

const CAP_MAP = {butt: 0, round: 1, square: 2};
const JOIN_MAP = {miter: 0, round: 1, bevel: 2};

/** The ERVectorPaint members, in op-tape paint order [fill,stroke,stroke_w,miter,cap,join,fill_rule]. */
const PAINT_FIELDS = [
  'fill',
  'stroke',
  'stroke_w',
  'miter',
  'cap',
  'join',
  'fill_rule',
  'fill_grad',
  'stroke_grad',
];

/**
 * A shape's paint as 7 C-expr fields (matching PAINT_FIELDS) + whether any is state-driven.
 *   - fill / stroke may be DYNAMIC → lowered via emitColorExpr to an ARGB uint expr (a color string, a
 *     ternary of them, or a folded theme token); static → a baked `0xAARRGGBBu` literal.
 *   - strokeWidth may be DYNAMIC (numeric C expr); static → a float literal.
 *   - cap / join / miterlimit / fillRule must be STATIC (a dynamic one throws clear).
 */
function paintSpec(a, env, scope) {
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
  for (const k of [
    'strokeLinecap',
    'strokeLinejoin',
    'strokeMiterlimit',
    'fillRule',
  ])
    if (isDyn(a[k]))
      throw new Error(
        `AOT: a state-driven <Svg> "${k}" is not supported (only fill / stroke / strokeWidth can be state-driven)`,
      );
  const fields = [
    color(a.fill, 'black'),
    color(a.stroke, 'none'),
    strokeW,
    floatLit(svgNum(a.strokeMiterlimit, 4)),
    String(CAP_MAP[a.strokeLinecap] ?? 0),
    String(JOIN_MAP[a.strokeLinejoin] ?? 0),
    String(a.fillRule === 'evenodd' ? 1 : 0),
  ];
  // Gradient table indices are appended by the caller, which is the only place that knows the table
  // layout for the whole <Svg>. Placeholders keep `fields` aligned with PAINT_FIELDS until then.
  fields.push('0', '0');
  const fillGrad = gradAttr(a.fillGrad, scope, env, 'fillGrad');
  const strokeGrad = gradAttr(a.strokeGrad, scope, env, 'strokeGrad');
  // A conditional gradient makes the INDEX state-driven, so the paint table has to be mutable too.
  if (
    fillGrad?.anyDynamic ||
    strokeGrad?.anyDynamic ||
    fillGrad?.cond ||
    strokeGrad?.cond
  )
    anyDynamic = true;
  return {fields, anyDynamic, fillGrad, strokeGrad};
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
  return ['ER_VOP_ARC', cx, cy, r, a0, a1, '0.0f'];
};
const circleEntriesC = (cx, cy, r) => [
  'ER_VOP_MOVE',
  `(${cx} + ${r})`,
  cy,
  'ER_VOP_ARC',
  cx,
  cy,
  r,
  '0.0f',
  '(2.0f * (float)M_PI)',
  '0.0f',
  'ER_VOP_CLOSE',
];
/** The numeric value of a C float literal (as produced by floatLit/cf, or the `(float)(N)` cast the
 *  imperative updateVector path emits), or null for a state-driven expr. */
const C_NUM = '-?(?:\\d+\\.?\\d*|\\.\\d+)(?:[eE][-+]?\\d+)?';
const cLit = e => {
  const t = String(e).trim();
  const cast = new RegExp(`^\\(float\\)\\((${C_NUM})\\)$`).exec(t);
  if (cast) return parseFloat(cast[1]);
  return new RegExp(`^${C_NUM}f$`).test(t) ? parseFloat(t) : null;
};

/** Normalizes a shape's geometry result. Most shapes are just an op-tape; a rounded <Rect> also returns
 *  the `const float` locals the caller must declare ahead of that tape, in the same C block. */
const geometryOf = g => (Array.isArray(g) ? {entries: g, locals: []} : g);

/** A corner radius clamped to half its side, mirroring svg-ops rectRadii. Folded to a literal when both
 *  the radius and the side are static — the usual case, since only x/y/width tend to be state-driven. */
const clampRadiusC = (r, side) => {
  const lr = cLit(r);
  if (lr != null && lr <= 0) return '0.0f';
  const ls = cLit(side);
  if (lr != null && ls != null)
    return floatLit(Math.max(0, Math.min(lr, ls / 2)));
  return `fminf(fmaxf(${r}, 0.0f), (${side}) * 0.5f)`;
};

const sharpRectEntriesC = (x, y, w, h) => [
  'ER_VOP_MOVE',
  x,
  y,
  'ER_VOP_LINE',
  `(${x} + ${w})`,
  y,
  'ER_VOP_LINE',
  `(${x} + ${w})`,
  `(${y} + ${h})`,
  'ER_VOP_LINE',
  x,
  `(${y} + ${h})`,
  'ER_VOP_CLOSE',
];

// rx/ry are null when the rect has no corner radius at all. Corners are cubics, not ER_VOP_ARC, for the
// same reason svg-ops uses them: a corner must start at EXACTLY the preceding line's endpoint. Returns
// { entries, locals }; `tag` names the locals and must be unique within the caller's C block.
const rectEntriesC = (x, y, w, h, rx = null, ry = null, tag = '') => {
  // A negative radius is invalid, and invalid means `auto` — so it falls back to the other radius
  // rather than squaring the corners (svg-ops rectRadii, and what browsers render). Only a literal can
  // be resolved here; a state-driven radius is assumed non-negative and merely clamped at runtime.
  const isNeg = e => {
    const l = cLit(e);
    return l != null && l < 0;
  };
  if (isNeg(rx)) rx = null;
  if (isNeg(ry)) ry = null;
  if (rx == null && ry == null)
    return {entries: sharpRectEntriesC(x, y, w, h), locals: []};
  let cx = clampRadiusC(rx ?? ry, w);
  let cy = clampRadiusC(ry ?? rx, h);
  if (cLit(cx) === 0 || cLit(cy) === 0)
    return {entries: sharpRectEntriesC(x, y, w, h), locals: []};
  // A clamp that did not fold is a runtime fminf/fmaxf pair the tape below would otherwise repeat ten
  // times over — and with a state-driven side, each copy drags the whole side expression with it. Bind
  // it once instead; a folded radius stays inline, so an all-static rect emits exactly what it used to.
  const locals = [];
  if (cLit(cx) == null) {
    locals.push(`const float rx_${tag} = ${cx};`);
    cx = `rx_${tag}`;
  }
  if (cLit(cy) == null) {
    locals.push(`const float ry_${tag} = ${cy};`);
    cy = `ry_${tag}`;
  }
  const k = floatLit(KAPPA);
  const kx = `(${cx} * ${k})`;
  const ky = `(${cy} * ${k})`;
  const x1 = `(${x} + ${cx})`;
  const x2 = `(${x} + ${w} - ${cx})`;
  const y1 = `(${y} + ${cy})`;
  const y2 = `(${y} + ${h} - ${cy})`;
  const r = `(${x} + ${w})`;
  const b = `(${y} + ${h})`;
  const entries = [
    'ER_VOP_MOVE',
    x1,
    y,
    'ER_VOP_LINE',
    x2,
    y,
    'ER_VOP_CUBIC',
    `(${x2} + ${kx})`,
    y,
    r,
    `(${y1} - ${ky})`,
    r,
    y1,
    'ER_VOP_LINE',
    r,
    y2,
    'ER_VOP_CUBIC',
    r,
    `(${y2} + ${ky})`,
    `(${x2} + ${kx})`,
    b,
    x2,
    b,
    'ER_VOP_LINE',
    x1,
    b,
    'ER_VOP_CUBIC',
    `(${x1} - ${kx})`,
    b,
    x,
    `(${y2} + ${ky})`,
    x,
    y2,
    'ER_VOP_LINE',
    x,
    y1,
    'ER_VOP_CUBIC',
    x,
    `(${y1} - ${ky})`,
    `(${x1} - ${kx})`,
    y,
    x1,
    y,
    'ER_VOP_CLOSE',
  ];
  return {entries, locals};
};
const lineEntriesC = (x1, y1, x2, y2) => [
  'ER_VOP_MOVE',
  x1,
  y1,
  'ER_VOP_LINE',
  x2,
  y2,
];

// JSX-attribute wrappers (6b): resolve each attr to a C float via cf().
const arcEntries = a =>
  arcEntriesC(cf(a.cx), cf(a.cy), cf(a.r), cf(a.startAngle), cf(a.endAngle));
const circleEntries = a => circleEntriesC(cf(a.cx), cf(a.cy), cf(a.r));
const rectEntries = (a, tag) =>
  rectEntriesC(
    cf(a.x),
    cf(a.y),
    cf(a.width),
    cf(a.height),
    a.rx == null ? null : cf(a.rx),
    a.ry == null ? null : cf(a.ry),
    tag,
  );
const lineEntries = a => lineEntriesC(cf(a.x1), cf(a.y1), cf(a.x2), cf(a.y2));
const pathEntries = a => {
  if (a.d == null) return [];
  if (isDyn(a.d))
    throw new Error(
      'AOT: a state-driven <Path d=…> is not yet supported (use Arc/Circle/Rect/Line for dynamic shapes)',
    );
  return parsePath(String(a.d)).map(floatLit); // opcodes are encoded as float values 0..6, like coords
};
const SHAPE_ENTRIES = {
  Arc: arcEntries,
  Circle: circleEntries,
  Rect: rectEntries,
  Line: lineEntries,
  Path: pathEntries,
};

/** True if any attribute anywhere in the <Svg> subtree references state (→ the state-driven path). */
function svgHasDynamic(el, scope) {
  let dyn = false;
  const walk = node => {
    // A fragment holds no attributes of its own but its shapes' attributes still count.
    if (node.type === 'JSXFragment') {
      for (const c of node.children) walk(c);
      return;
    }
    if (node.type !== 'JSXElement') return;
    for (const attr of node.openingElement.attributes) {
      if (
        attr.type === 'JSXAttribute' &&
        attr.name.name !== 'ref' &&
        attr.name.name !== 'key' &&
        attr.value?.type === 'JSXExpressionContainer'
      ) {
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
  const {staticAssigns} = collectStyleAssigns(openingElement, scope, env);
  out.build.push(
    `    ${v} = er_node_create(ER_NODE_VECTOR);`,
    `    er_props_default(&p);`,
  );
  if (typeof width === 'number')
    out.build.push(`    p.width = (int16_t)${Math.round(width)};`);
  if (typeof height === 'number')
    out.build.push(`    p.height = (int16_t)${Math.round(height)};`);
  for (const a of staticAssigns)
    out.build.push(`    p.${a.field} = ${a.expr};`);
  out.build.push(`    er_node_set_props(${v}, &p);`);
  emitRefBind(v, openingElement, out, env);
}

/** <Svg> → ER_NODE_VECTOR. Static subtree → a baked const op-tape; any state-driven attr → a symbolic
 *  op-tape rebuilt by a generated build_svgN() at build time and on every app_update. */
function emitSvg(el, scope, out, env, state, opts) {
  if (opts.displayCode)
    throw new Error(
      'AOT: an <Svg> inside a dynamic conditional is not yet supported',
    );
  // The vector emitter builds its box from static style assigns only, so it cannot honour `visible`
  // (nor a state-driven style `display`). Say so rather than dropping the prop — a silently inert
  // hide prop is exactly the trap this alias exists to remove.
  if (
    el.openingElement.attributes.some(
      a => a.type === 'JSXAttribute' && a.name && a.name.name === 'visible',
    )
  )
    throw aotError(
      'AOT: `visible` on an <Svg> is not supported',
      'wrap it: <View visible={…}><Svg …/></View> — the View carries the hide and the whole subtree goes with it.',
    );
  const sourceAttr = el.openingElement.attributes.find(
    a => a.type === 'JSXAttribute' && a.name && a.name.name === 'source',
  );
  if (sourceAttr) return emitSvgSource(el, sourceAttr, scope, out, env);
  return svgHasDynamic(el, scope)
    ? emitSvgDynamic(el, scope, out, env, state)
    : emitSvgStatic(el, scope, out, env);
}

/** Static <Svg>: reuse flattenSvg (full feature set: viewBox, <G>, Path) and bake const arrays. */
function emitSvgStatic(el, scope, out, env) {
  const svgEl = jsxToSvgElement(el, scope);
  const {ops, paints} = flattenSvg(svgEl.props);
  const v = `n${out.n++}`;
  const id = out.svgN++;
  const nPaints = paints.length / PAINT_STRIDE;
  if (ops.length) {
    out.vectorData.push(
      `static const float s_svg${id}_ops[] = {\n    ${Array.from(ops, floatLit).join(', ')}\n};`,
    );
    out.vectorData.push(
      `static const ERVectorPaint s_svg${id}_paints[] = {\n${Array.from({length: nPaints}, (_, i) => '    ' + emitVectorPaint(paints.slice(i * PAINT_STRIDE, i * PAINT_STRIDE + PAINT_STRIDE))).join(',\n')}\n};`,
    );
  }
  emitSvgBox(
    v,
    svgEl.props.width,
    svgEl.props.height,
    el.openingElement,
    scope,
    out,
    env,
  );
  if (ops.length)
    out.build.push(
      `    er_node_set_vector_ops(${v}, s_svg${id}_ops, ${ops.length}, s_svg${id}_paints, ${nPaints}, NULL, 0);`,
    );
  return v;
}

/** Baked <Svg source={importedSvg}>: the .svg is pre-baked to a vector artifact (ops/paints/GRADIENTS) by the
 *  CLI (bakeSvgArtifacts → opts.svgArtifacts) since compileSource is I/O-free. Scaled to the static width/height
 *  at compile time, then emitted as const tables. This is the path that carries gradients into Flow B. */
function emitSvgSource(el, sourceAttr, scope, out, env) {
  const expr =
    sourceAttr.value && sourceAttr.value.type === 'JSXExpressionContainer'
      ? sourceAttr.value.expression
      : null;
  if (!expr || expr.type !== 'Identifier')
    throw new Error(
      'AOT: <Svg source> must reference an imported .svg (source={importedSvg})',
    );
  const imp = env.svgImports.get(expr.name);
  if (!imp)
    throw new Error(
      `AOT: <Svg source={${expr.name}}> — no matching \`import ${expr.name} from '...svg'\``,
    );
  const art = env.svgArtifacts[imp.name];
  if (!art)
    throw new Error(
      `AOT: vector artifact for "${imp.name}" was not baked (internal: opts.svgArtifacts missing it)`,
    );

  // Static width/height (props or {expr} that folds) → scale the artifact at compile time; default = intrinsic.
  const numAttr = (name, dflt) => {
    const at = el.openingElement.attributes.find(
      a => a.type === 'JSXAttribute' && a.name && a.name.name === name,
    );
    if (!at || at.value == null) return dflt;
    if (at.value.type === 'StringLiteral') return svgNum(at.value.value, dflt);
    if (at.value.type === 'JSXExpressionContainer') {
      try {
        return svgNum(evalStatic(at.value.expression, scope), dflt);
      } catch {
        throw new Error(
          'AOT: <Svg source> width/height must be a static number',
        );
      }
    }
    return dflt;
  };
  const w = numAttr('width', art.width);
  const h = numAttr('height', art.height);

  // Raster fallback: the .svg used unsupported features and was rasterized to a PNG at bake time
  // (bakeSvgArtifacts). Emit it as an Image node sized to the box, and register the PNG so it bakes into
  // assets.generated.c (the CLI resolves importPath; an absolute temp path passes through resolve() as-is).
  if (art.kind === 'raster') {
    if (art.png) out.images.set(art.name, art.png);
    const vimg = `n${out.n++}`;
    const {staticAssigns} = collectStyleAssigns(el.openingElement, scope, env);
    out.build.push(
      `    ${vimg} = er_node_create(${NODE_TYPES.Image});`,
      `    er_props_default(&p);`,
    );
    if (typeof w === 'number')
      out.build.push(`    p.width = (int16_t)${Math.round(w)};`);
    if (typeof h === 'number')
      out.build.push(`    p.height = (int16_t)${Math.round(h)};`);
    for (const a of staticAssigns)
      out.build.push(`    p.${a.field} = ${a.expr};`);
    out.build.push(
      `    snprintf(p.image_name, sizeof(p.image_name), "%s", ${cstr(art.name)});`,
    );
    out.build.push(`    er_node_set_props(${vimg}, &p);`);
    emitRefBind(vimg, el.openingElement, out, env);
    return vimg;
  }

  const scaled = scaleVectorArtifact(art, w, h);
  const ops = scaled.ops;
  const paints = scaled.paints;
  const gradients = scaled.gradients || [];

  const v = `n${out.n++}`;
  const id = out.svgN++;
  const nPaints = paints.length / PAINT_STRIDE;
  if (ops.length) {
    out.vectorData.push(
      `static const float s_svg${id}_ops[] = {\n    ${Array.from(ops, floatLit).join(', ')}\n};`,
    );
    out.vectorData.push(
      `static const ERVectorPaint s_svg${id}_paints[] = {\n${Array.from({length: nPaints}, (_, i) => '    ' + emitVectorPaint(paints.slice(i * PAINT_STRIDE, i * PAINT_STRIDE + PAINT_STRIDE))).join(',\n')}\n};`,
    );
    if (gradients.length)
      out.vectorData.push(
        `static const ERVectorGradient s_svg${id}_grads[] = {\n${gradients.map(g => '    ' + emitVectorGradient(g)).join(',\n')}\n};`,
      );
  }
  emitSvgBox(v, w, h, el.openingElement, scope, out, env);
  if (ops.length) {
    const gradsRef = gradients.length ? `s_svg${id}_grads` : 'NULL';
    out.build.push(
      `    er_node_set_vector_ops(${v}, s_svg${id}_ops, ${ops.length}, s_svg${id}_paints, ${nPaints}, ${gradsRef}, ${gradients.length});`,
    );
  }
  return v;
}

/** State-driven <Svg> (flat Arc/Circle/Rect/Line/static-Path; no viewBox/<G>): emit a mutable op-tape
 *  + build_svgN() that recomputes it from state, called at build and re-called on each app_update. */
function emitSvgDynamic(el, scope, out, env, state) {
  const svgA = svgAttrs(el.openingElement, scope, env);
  if (svgA.viewBox != null)
    throw new Error(
      'AOT: a viewBox on a state-driven <Svg> is not yet supported — size shapes in the width/height space',
    );
  const entries = [];
  const decls = []; // C locals the shapes need declared ahead of the tape (rounded-rect radii)
  const specs = [];
  for (const c of svgShapeChildren(el.children)) {
    const type = c.openingElement.name.name;
    const fn = SHAPE_ENTRIES[type];
    if (!fn)
      throw new Error(
        `AOT: <${type}> is not a supported shape in a state-driven <Svg> (no <G>/viewBox yet)`,
      );
    const a = svgAttrs(c.openingElement, scope, env);
    // build_svgN() is this <Svg>'s own function, so the shape index alone keeps a local's name unique.
    const shape = geometryOf(fn(a, `s${specs.length}`));
    if (!shape.entries.length) continue;
    decls.push(...shape.locals);
    entries.push('ER_VOP_SHAPE', floatLit(specs.length), ...shape.entries);
    specs.push(paintSpec(a, env, scope));
  }
  const v = `n${out.n++}`;
  const id = out.svgN++;
  const len = entries.length;
  const nPaints = specs.length;
  const dynPaint = specs.some(p => p.anyDynamic);
  out.needsMath = true; // build_svg uses cosf/sinf/M_PI for arcs

  // Gradients referenced by these shapes, flattened into ONE table per <Svg> (the engine indexes it
  // 1-based off each paint). Done here rather than in paintSpec because the layout is per-<Svg>, not
  // per-shape, so a shape cannot know its own index.
  const grads = [];
  for (const ps of specs) {
    const idx = g => {
      if (!g) return '0';
      const n = grads.push(g); // push returns the new length = the 1-based index
      return g.cond ? `((${g.cond}) ? ${n} : 0)` : String(n);
    };
    ps.fields[7] = idx(ps.fillGrad);
    ps.fields[8] = idx(ps.strokeGrad);
  }
  const dynGrad = grads.some(g => g.anyDynamic);

  out.vectorData.push(`static float s_svg${id}_ops[${len}];`);
  if (grads.length) {
    // Mutable when any field is state-driven — a conic ramp anchored to a setpoint rewrites its start
    // angle every update, exactly like the op-tape above it.
    if (dynGrad)
      out.vectorData.push(
        `static ERVectorGradient s_svg${id}_grads[${grads.length}];`,
      );
    else
      out.vectorData.push(
        `static const ERVectorGradient s_svg${id}_grads[] = {\n${grads.map(g => '    ' + gradInitFromSpec(g)).join(',\n')}\n};`,
      );
  }
  // Dynamic paint → a MUTABLE paint table (re)filled by build_svg from state each update; else a const table.
  if (dynPaint)
    out.vectorData.push(`static ERVectorPaint s_svg${id}_paints[${nPaints}];`);
  else
    out.vectorData.push(
      `static const ERVectorPaint s_svg${id}_paints[] = {\n${specs.map(p => '    ' + paintInitFromSpec(p)).join(',\n')}\n};`,
    );
  const builderLines = [
    ...decls.map(d => `    ${d}`),
    ...entries.map((e, i) => `    s_svg${id}_ops[${i}] = ${e};`),
  ];
  if (dynPaint)
    specs.forEach((ps, pi) =>
      ps.fields.forEach((f, fi) =>
        builderLines.push(
          `    s_svg${id}_paints[${pi}].${PAINT_FIELDS[fi]} = ${f};`,
        ),
      ),
    );
  if (dynGrad)
    grads.forEach((g, gi) =>
      builderLines.push(...gradAssigns(`s_svg${id}_grads`, gi, g)),
    );
  out.vectorBuilders.push(
    `static void build_svg${id}(void)\n{\n${builderLines.join('\n')}\n}`,
  );

  emitSvgBox(v, svgA.width, svgA.height, el.openingElement, scope, out, env);
  out.build.push(
    `    build_svg${id}();`,
    `    er_node_set_vector_ops(${v}, s_svg${id}_ops, ${len}, s_svg${id}_paints, ${nPaints}, ${grads.length ? `s_svg${id}_grads` : 'NULL'}, ${grads.length});`,
    `    s_${v} = ${v};`,
  );
  out.handles.push(v);
  out.svgUpdates.push({
    id,
    len,
    nPaints,
    nGrads: grads.length,
    nodeVar: `s_${v}`,
  });
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
    const e =
      attr.value?.type === 'JSXExpressionContainer'
        ? attr.value.expression
        : null;
    if (e?.type === 'Identifier' && env.refs?.get(e.name)?.kind === 'node') {
      const r = env.refs.get(e.name);
      r.used = true;
      out.build.push(`    ${r.cVar} = ${v};`);
    } else
      throw new Error(
        'AOT: ref={…} must reference a node ref declared with useRef()',
      );
  }
}

/** Compiles a value-callback (e.g. Switch onValueChange) — binds its first param to `valueCode`, not an event. */
function compileValueHandler(
  fnNode,
  valueCode,
  env,
  state,
  out,
  cType = 'int',
  valueCode2 = null,
  isBool = false,
) {
  const param =
    fnNode.params[0]?.type === 'Identifier' ? fnNode.params[0].name : null;
  const locals = new Map(env.locals);
  // A <Switch> hands its callback a boolean; carry that so `'on=' + v` prints true/false, not 1/0.
  if (param) locals.set(param, {code: valueCode, cType, isBool});
  // A second parameter (a RANGE <Dial>'s low end) binds the same way, so `onChange={(hi, lo) => …}`
  // lowers to data->value / data->value_start with no object allocated on device.
  const param2 =
    fnNode.params[1]?.type === 'Identifier' ? fnNode.params[1].name : null;
  if (param2 && valueCode2) locals.set(param2, {code: valueCode2, cType});
  const ctx = {stateChanged: false, animIdx: 0, out};
  const body = fnNode.body;
  const list =
    body.type === 'BlockStatement'
      ? body.body
      : [{type: 'ExpressionStatement', expression: body}];
  const stmts = compileStmts(list, {...env, locals}, state, ctx, '    ');
  if (ctx.stateChanged) stmts.push('    app_update();');
  return stmts;
}

/** <Dial> prop → ERProps field tables (see emitDial). */
const DIAL_FLOAT_PROPS = {
  value: 'arc_value',
  valueStart: 'arc_value_start',
  minSpan: 'arc_min_span',
  min: 'arc_min',
  max: 'arc_max',
  startAngle: 'arc_start_angle',
  sweepAngle: 'arc_sweep_angle',
  step: 'arc_step',
  gapAngle: 'arc_gap_angle',
};
const DIAL_INT_PROPS = {
  thickness: 'arc_width',
  bandThickness: 'arc_band_width',
  knobSize: 'arc_knob_size',
  knobBorderWidth: 'arc_knob_border_width',
  segments: 'arc_segments',
};
const DIAL_COLOR_PROPS = {
  trackColor: 'arc_track_color',
  indicatorColor: 'arc_indicator_color',
  bandColor: 'arc_band_color',
  knobColor: 'arc_knob_color',
  knobBorderColor: 'arc_knob_border_color',
};
const DIAL_ENUM_PROPS = {
  cap: {
    field: 'arc_cap',
    table: {butt: 'ER_ARC_CAP_BUTT', round: 'ER_ARC_CAP_ROUND'},
  },
  knob: {
    field: 'arc_knob',
    table: {
      none: 'ER_ARC_KNOB_NONE',
      circle: 'ER_ARC_KNOB_CIRCLE',
      image: 'ER_ARC_KNOB_IMAGE',
      child: 'ER_ARC_KNOB_CHILD',
    },
  },
};

/**
 * <Dial value={v} min max startAngle sweepAngle step thickness bandThickness trackColor indicatorColor
 *       indicatorGradient bandColor cap segments gapAngle knob knobSize knobColor knobBorderColor
 *       knobBorderWidth knobImage adjustable onChange={(v) => setV(v)} style=… />
 * → ER_NODE_ARC, the engine's native arc widget. Numbers and colours are static literals or state-driven
 * (recomputed in app_update); `value` may also be a useAnimatedValue handle, which binds ER_PROP_ARC_VALUE
 * natively so a ramp costs no app_update at all. onChange lowers to ER_EVENT_VALUE_CHANGE with its param
 * bound to data->value (the quantized value the built-in drag produced). Default 120x120 box.
 */
function emitDial(el, scope, out, env, state) {
  const v = `n${out.n++}`;
  const {staticAssigns, dynAssigns, binds} = collectStyleAssigns(
    el.openingElement,
    scope,
    env,
  );
  const hasField = f => styleWrites(staticAssigns, dynAssigns, f);
  if (!hasField('width')) staticAssigns.push({field: 'width', expr: '120'});
  if (!hasField('height')) staticAssigns.push({field: 'height', expr: '120'});

  const SUPPORTED =
    'supported props: value, min, max, startAngle, sweepAngle, step, thickness, bandThickness, ' +
    'trackColor, indicatorColor, indicatorGradient, bandColor, cap, segments, gapAngle, knob, knobSize, ' +
    'minSpan, ' +
    'knobColor, knobBorderColor, knobBorderWidth, knobImage, adjustable, range, valueStart, onChange, style.';
  const numeric = (field, node, isFloat) => {
    try {
      const n = evalStatic(node, scope);
      if (typeof n !== 'number') throw new Error('not a number');
      staticAssigns.push({
        field,
        expr: isFloat ? floatLit(n) : String(Math.round(n)),
      });
    } catch {
      const e = emitExpr(node, env);
      dynAssigns.push({
        field,
        code: isFloat ? `(float)(${e.code})` : `app_round_dim(${e.code})`,
      });
    }
  };
  const colour = (field, node) => {
    try {
      const c = evalStatic(node, scope);
      if (typeof c !== 'string') throw new Error('not a colour');
      staticAssigns.push({field, expr: colorLiteral(c)});
    } catch {
      dynAssigns.push({field, code: emitColorExpr(node, env)});
    }
  };

  let onChangeFn = null;
  let knobImage = null; // static asset name
  for (const attr of el.openingElement.attributes) {
    if (attr.type !== 'JSXAttribute')
      throw aotError('AOT: spread props on <Dial> are not supported');
    const name = attr.name.name;
    if (name === 'style' || name === 'ref' || name === 'key') continue;
    const node = attrExpr(attr);
    if (
      (name === 'value' || name === 'valueStart') &&
      node?.type === 'Identifier' &&
      env.anims?.has(node.name)
    ) {
      binds.push({
        cVar: env.anims.get(node.name).cVar,
        prop:
          name === 'value' ? 'ER_PROP_ARC_VALUE' : 'ER_PROP_ARC_VALUE_START',
      });
    } else if (DIAL_FLOAT_PROPS[name])
      numeric(DIAL_FLOAT_PROPS[name], node, true);
    else if (DIAL_INT_PROPS[name]) numeric(DIAL_INT_PROPS[name], node, false);
    else if (DIAL_COLOR_PROPS[name]) colour(DIAL_COLOR_PROPS[name], node);
    else if (DIAL_ENUM_PROPS[name]) {
      const {field, table} = DIAL_ENUM_PROPS[name];
      let tok = null;
      try {
        tok = evalStatic(node, scope);
      } catch {
        /* state-driven — handled below */
      }
      if (typeof tok === 'string') {
        if (!table[tok])
          throw aotError(
            `AOT: unsupported <Dial ${name}> "${tok}"`,
            `${name} must be one of: ${Object.keys(table).join(' / ')}.`,
          );
        staticAssigns.push({field, expr: table[tok]});
      } else {
        dynAssigns.push({
          field,
          code: `(uint8_t)(${emitEnumExpr(node, table, env)})`,
        });
      }
    } else if (name === 'adjustable' || name === 'range') {
      const field = name === 'range' ? 'arc_range' : 'arc_adjustable';
      try {
        staticAssigns.push({field, expr: evalStatic(node, scope) ? '1' : '0'});
      } catch {
        dynAssigns.push({
          field,
          code: `(uint8_t)((${emitExpr(node, env).code}) ? 1 : 0)`,
        });
      }
    } else if (name === 'knobImage') {
      knobImage = imageNameFromSource(node, env);
      if (knobImage == null)
        throw aotError(
          'AOT: <Dial knobImage> must resolve to a static asset name',
          "use an imported image (`import knob from './knob.png'` → knobImage={knob}) or a string asset name.",
        );
      const path = env.imageNames?.get(knobImage);
      if (path) out.images.set(knobImage, path);
    } else if (name === 'indicatorGradient') {
      // A gradient is usually applied only in SOME state (a thermostat ramps its band in AUTO and paints
      // it solid otherwise), so `cond ? {…} : null` is the common shape. The stops themselves are still
      // constant — only WHETHER they apply varies — so bake them and switch the stop COUNT at runtime:
      // the engine ignores a gradient with fewer than two stops and falls back to indicatorColor.
      let gradNode = node;
      let gradCond = null;
      if (node?.type === 'ConditionalExpression') {
        const nullish = n =>
          n?.type === 'NullLiteral' ||
          (n?.type === 'Identifier' && n.name === 'undefined');
        if (nullish(node.alternate)) {
          gradNode = node.consequent;
          gradCond = emitExpr(node.test, env).code;
        } else if (nullish(node.consequent)) {
          gradNode = node.alternate;
          gradCond = `!(${emitExpr(node.test, env).code})`;
        }
      }
      const g = evalStaticOrThrow(
        gradNode,
        scope,
        'AOT: <Dial indicatorGradient> must be a static object (optionally behind a ternary against null)',
        "indicatorGradient={{ type: 'conic', stops: [{ color: '#00f' }, { color: '#f00' }] }}, or " +
          'indicatorGradient={on ? {…} : null}',
      );
      const stops = Array.isArray(g?.stops) ? g.stops.slice(0, 4) : [];
      if (stops.length < 2)
        throw aotError(
          'AOT: <Dial indicatorGradient> needs at least 2 stops (max 4)',
        );
      staticAssigns.push({
        field: 'gradient_type',
        expr: g.type === 'radial' ? 'ER_GRADIENT_RADIAL' : 'ER_GRADIENT_CONIC',
      });
      if (gradCond)
        dynAssigns.push({
          field: 'gradient_stop_count',
          code: `(uint8_t)((${gradCond}) ? ${stops.length} : 0)`,
        });
      else
        staticAssigns.push({
          field: 'gradient_stop_count',
          expr: String(stops.length),
        });
      stops.forEach((st, i) => {
        const off =
          typeof st.offset === 'number' ? st.offset : i / (stops.length - 1);
        staticAssigns.push({
          field: `gradient_stops[${i}].color`,
          expr: colorLiteral(String(st.color)),
        });
        staticAssigns.push({
          field: `gradient_stops[${i}].position`,
          expr: floatLit(off),
        });
      });
    } else if (name === 'onChange') onChangeFn = node;
    else
      throw aotError(`AOT: <Dial> prop "${name}" is not supported`, SUPPORTED);
  }

  const isDynamic = dynAssigns.length > 0;
  out.build.push(`    ${v} = er_node_create(ER_NODE_ARC);`);
  if (isDynamic) {
    out.build.push(`    s_${v} = ${v};`);
    out.handles.push(v);
    out.updates.push({
      v,
      styleAssigns: staticAssigns,
      text: null,
      dynAssigns,
      imageName: knobImage != null ? cstr(knobImage) : null,
    });
  } else {
    out.build.push(`    er_props_default(&p);`);
    for (const a of staticAssigns)
      out.build.push(`    p.${a.field} = ${a.expr};`);
    if (knobImage != null)
      out.build.push(
        `    snprintf(p.image_name, sizeof(p.image_name), "%s", ${cstr(knobImage)});`,
      );
    out.build.push(`    er_node_set_props(${v}, &p);`);
  }

  // Animated bindings: style props plus an animated `value` (ER_PROP_ARC_VALUE).
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

  if (onChangeFn) {
    // A useCallback identifier resolves to its arrow, the same way the generic host-node event path does —
    // a dial's change handler is exactly the kind of thing an app wraps in useCallback.
    if (onChangeFn.type === 'Identifier' && env.callbacks?.has(onChangeFn.name))
      onChangeFn = env.callbacks.get(onChangeFn.name);
    if (!isFn(onChangeFn))
      throw aotError(
        'AOT: <Dial onChange> must be an inline function or a useCallback',
        'onChange={(v) => setValue(v)}',
      );
    const handlerName = `er_handler_${out.handlers.length}`;
    out.handlers.push({
      name: handlerName,
      body: compileValueHandler(
        onChangeFn,
        'data->value',
        env,
        state,
        out,
        'float',
        'data->value_start',
      ),
    });
    out.build.push(
      `    er_event_set(${v}, ER_EVENT_VALUE_CHANGE, ${handlerName}, NULL);`,
    );
  }
  emitRefBind(v, el.openingElement, out, env);
  // Children lay out inside the dial's box like any View's (a centre readout), and with knob="child" the
  // engine moves the first one onto the value point.
  emitChildren(el.children, v, scope, out, env, state);
  return v;
}

/**
 * <Switch value={on} onValueChange={(v) => setOn(v)} trackColor={{false,true}} thumbColor=… style=… />
 * → ER_NODE_SWITCH. The engine flips its own value on press (+ animates the thumb) then fires ER_EVENT_PRESS,
 * so onValueChange maps to PRESS and its `v` param is the TOGGLED value (!value). `value` drives switch_value
 * (state → dynamic). Default RN 51×31 box (the renderer scales the track/thumb to it); style can override.
 */
function emitSwitch(el, scope, out, env, state) {
  const v = `n${out.n++}`;
  const {staticAssigns, dynAssigns} = collectStyleAssigns(
    el.openingElement,
    scope,
    env,
  );
  const hasField = f => styleWrites(staticAssigns, dynAssigns, f);
  if (!hasField('width')) staticAssigns.push({field: 'width', expr: '51'});
  if (!hasField('height')) staticAssigns.push({field: 'height', expr: '31'});

  let valueNode = null;
  let onChangeFn = null;
  for (const attr of el.openingElement.attributes) {
    if (attr.type !== 'JSXAttribute')
      throw aotError('AOT: spread props on <Switch> are not supported');
    const name = attr.name.name;
    if (name === 'style' || name === 'ref' || name === 'key') continue;
    const node = attrExpr(attr);
    if (name === 'value') valueNode = node;
    else if (name === 'onValueChange') onChangeFn = node;
    else if (name === 'thumbColor')
      staticAssigns.push({
        field: 'thumb_color',
        expr: colorLiteral(String(evalStatic(node, scope))),
      });
    else if (name === 'trackColor') {
      const tc = evalStatic(node, scope);
      if (tc?.false != null)
        staticAssigns.push({
          field: 'track_color_false',
          expr: colorLiteral(String(tc.false)),
        });
      if (tc?.true != null)
        staticAssigns.push({
          field: 'track_color_true',
          expr: colorLiteral(String(tc.true)),
        });
    } else if (name === 'disabled') {
      /* accepted; the AOT has no disabled-visual yet, so it is a no-op */
    } else
      throw aotError(
        `AOT: <Switch> prop "${name}" is not supported`,
        'supported props: value, onValueChange, trackColor, thumbColor, style.',
      );
  }

  // value → switch_value (static or, when state-driven, recomputed in app_update).
  if (valueNode) {
    try {
      staticAssigns.push({
        field: 'switch_value',
        expr: evalStatic(valueNode, scope) ? '1' : '0',
      });
    } catch {
      dynAssigns.push({
        field: 'switch_value',
        code: `(uint8_t)((${asCond(emitExpr(valueNode, env))}) ? 1 : 0)`,
      });
    }
  }

  const isDynamic = dynAssigns.length > 0;
  out.build.push(`    ${v} = er_node_create(ER_NODE_SWITCH);`);
  if (isDynamic) {
    out.build.push(`    s_${v} = ${v};`);
    out.handles.push(v);
    out.updates.push({v, styleAssigns: staticAssigns, text: null, dynAssigns});
  } else {
    out.build.push(`    er_props_default(&p);`);
    for (const a of staticAssigns)
      out.build.push(`    p.${a.field} = ${a.expr};`);
    out.build.push(`    er_node_set_props(${v}, &p);`);
  }

  if (onChangeFn) {
    if (!isFn(onChangeFn))
      throw aotError(
        'AOT: onValueChange must be an inline function',
        'onValueChange={(v) => setX(v)}',
      );
    if (!valueNode)
      throw aotError(
        'AOT: a <Switch> with onValueChange needs a value prop',
        'controlled switch: <Switch value={on} onValueChange={(v) => setOn(v)} />',
      );
    const handlerName = `er_handler_${out.handlers.length}`;
    const toggled = `(!(${asCond(emitExpr(valueNode, env))}))`; // the engine toggles on press → param is !value
    out.handlers.push({
      name: handlerName,
      body: compileValueHandler(
        onChangeFn,
        toggled,
        env,
        state,
        out,
        'int',
        null,
        true,
      ),
    });
    out.build.push(
      `    er_event_set(${v}, ER_EVENT_PRESS, ${handlerName}, NULL);`,
    );
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
  const {staticAssigns, dynAssigns} = collectStyleAssigns(
    el.openingElement,
    scope,
    env,
  );
  let valueNode = null;
  let onChangeFn = null;
  let placeholder = null;
  for (const attr of el.openingElement.attributes) {
    if (attr.type !== 'JSXAttribute')
      throw aotError('AOT: spread props on <TextInput> are not supported');
    const name = attr.name.name;
    if (name === 'style' || name === 'ref' || name === 'key') continue;
    const node = attrExpr(attr);
    if (name === 'value' || name === 'defaultValue') valueNode = node;
    else if (name === 'onChangeText') onChangeFn = node;
    else if (name === 'placeholder')
      placeholder = String(evalStatic(node, scope));
    else if (name === 'placeholderTextColor')
      staticAssigns.push({
        field: 'placeholder_color',
        expr: colorLiteral(String(evalStatic(node, scope))),
      });
    else if (name === 'cursorColor')
      staticAssigns.push({
        field: 'cursor_color',
        expr: colorLiteral(String(evalStatic(node, scope))),
      });
    else if (name === 'editable') {
      try {
        staticAssigns.push({
          field: 'editable',
          expr: evalStatic(node, scope) ? '1' : '0',
        });
      } catch {
        dynAssigns.push({
          field: 'editable',
          code: `(uint8_t)((${emitExpr(node, env).code}) ? 1 : 0)`,
        });
      }
    } else if (
      [
        'autoFocus',
        'keyboardType',
        'secureTextEntry',
        'maxLength',
        'multiline',
        'autoCapitalize',
        'autoCorrect',
        'returnKeyType',
        'onSubmitEditing',
        'onFocus',
        'onBlur',
      ].includes(name)
    ) {
      /* accepted but not yet lowered (no on-screen keyboard / submit wiring in the AOT path) */
    } else
      throw aotError(
        `AOT: <TextInput> prop "${name}" is not supported`,
        'supported props: value, onChangeText, placeholder, placeholderTextColor, cursorColor, editable, style.',
      );
  }

  // value → the input's text buffer (er_node_set_props → er_text_input_set_text): static literal, or a
  // state-driven value re-synced each app_update.
  let text = null;
  if (valueNode) {
    const f = emitFormat(valueNode, env, scope);
    text = {dynamic: f.args.length > 0, format: f.format, args: f.args};
  }

  const isDynamic = dynAssigns.length > 0 || (text && text.dynamic);
  out.build.push(`    ${v} = er_node_create(ER_NODE_TEXT_INPUT);`);
  if (isDynamic) {
    out.build.push(`    s_${v} = ${v};`);
    out.handles.push(v);
    out.updates.push({
      v,
      styleAssigns: staticAssigns,
      text,
      dynAssigns,
      placeholder,
    });
  } else {
    out.build.push(`    er_props_default(&p);`);
    for (const a of staticAssigns)
      out.build.push(`    p.${a.field} = ${a.expr};`);
    if (placeholder != null)
      out.build.push(
        `    snprintf(p.placeholder, sizeof(p.placeholder), "%s", ${cstr(placeholder)});`,
      );
    if (text)
      out.build.push(
        `    snprintf(p.text, sizeof(p.text), "%s", ${cstr(text.format.replace(/%%/g, '%'))});`,
      );
    out.build.push(`    er_node_set_props(${v}, &p);`);
  }

  if (onChangeFn) {
    if (!isFn(onChangeFn))
      throw aotError(
        'AOT: onChangeText must be an inline function',
        'onChangeText={(t) => setText(t)}',
      );
    const handlerName = `er_handler_${out.handlers.length}`;
    out.handlers.push({
      name: handlerName,
      body: compileValueHandler(
        onChangeFn,
        'data->changed_text',
        env,
        state,
        out,
        'string',
      ),
    });
    out.build.push(
      `    er_event_set(${v}, ER_EVENT_CHANGE_TEXT, ${handlerName}, NULL);`,
    );
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
  const {staticAssigns, dynAssigns} = collectStyleAssigns(
    el.openingElement,
    scope,
    env,
  );
  const hasField = f => styleWrites(staticAssigns, dynAssigns, f);
  let size = 36;
  for (const attr of el.openingElement.attributes) {
    if (attr.type !== 'JSXAttribute')
      throw aotError(
        'AOT: spread props on <ActivityIndicator> are not supported',
      );
    const name = attr.name.name;
    if (name === 'style' || name === 'ref' || name === 'key') continue;
    const node = attrExpr(attr);
    if (name === 'color') {
      try {
        staticAssigns.push({
          field: 'indicator_color',
          expr: colorLiteral(String(evalStatic(node, scope))),
        });
      } catch {
        dynAssigns.push({
          field: 'indicator_color',
          code: emitColorExpr(node, env),
        });
      }
    } else if (name === 'size') {
      const sv = evalStatic(node, scope);
      size = sv === 'small' ? 20 : sv === 'large' ? 36 : Number(sv) || 36;
    } else if (name === 'animating') {
      try {
        staticAssigns.push({
          field: 'animating',
          expr: evalStatic(node, scope) ? '1' : '0',
        });
      } catch {
        dynAssigns.push({
          field: 'animating',
          code: `(uint8_t)((${emitExpr(node, env).code}) ? 1 : 0)`,
        });
      }
    } else
      throw aotError(
        `AOT: <ActivityIndicator> prop "${name}" is not supported`,
        'supported props: color, size, animating, style.',
      );
  }
  if (!hasField('width'))
    staticAssigns.push({field: 'width', expr: String(size)});
  if (!hasField('height'))
    staticAssigns.push({field: 'height', expr: String(size)});

  const isDynamic = dynAssigns.length > 0;
  out.build.push(`    ${v} = er_node_create(ER_NODE_ACTIVITY_INDICATOR);`);
  if (isDynamic) {
    out.build.push(`    s_${v} = ${v};`);
    out.handles.push(v);
    out.updates.push({v, styleAssigns: staticAssigns, text: null, dynAssigns});
  } else {
    out.build.push(`    er_props_default(&p);`);
    for (const a of staticAssigns)
      out.build.push(`    p.${a.field} = ${a.expr};`);
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
  const {staticAssigns, dynAssigns} = collectStyleAssigns(
    el.openingElement,
    scope,
    env,
  );
  const hasField = f => styleWrites(staticAssigns, dynAssigns, f);
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
  for (const [f, expr] of DEFAULTS)
    if (!hasField(f)) staticAssigns.push({field: f, expr});

  let visibleNode = null;
  for (const attr of el.openingElement.attributes) {
    if (attr.type !== 'JSXAttribute')
      throw aotError('AOT: spread props on <Modal> are not supported');
    const name = attr.name.name;
    if (name === 'style' || name === 'ref' || name === 'key') continue;
    const node = attrExpr(attr);
    if (name === 'visible') visibleNode = node;
    else if (name === 'backdropColor')
      staticAssigns.push({
        field: 'backdrop_color',
        expr: colorLiteral(String(evalStatic(node, scope))),
      });
    else if (
      name === 'transparent' ||
      name === 'animationType' ||
      name === 'onRequestClose' ||
      name === 'statusBarTranslucent'
    ) {
      /* accepted for RN compatibility; no-op in the AOT today */
    } else
      throw aotError(
        `AOT: <Modal> prop "${name}" is not supported`,
        'supported: visible, backdropColor, style, children (transparent / animationType / onRequestClose are accepted but no-ops).',
      );
  }
  if (!visibleNode)
    throw aotError(
      'AOT: a <Modal> needs a visible prop',
      '<Modal visible={show}>…</Modal>',
    );
  try {
    staticAssigns.push({
      field: 'modal_visible',
      expr: evalStatic(visibleNode, scope) ? '1' : '0',
    });
  } catch {
    dynAssigns.push({
      field: 'modal_visible',
      code: `(uint8_t)((${emitExpr(visibleNode, env).code}) ? 1 : 0)`,
    });
  }

  const isDynamic = dynAssigns.length > 0;
  out.build.push(`    ${v} = er_node_create(ER_NODE_MODAL);`);
  if (isDynamic) {
    out.build.push(`    s_${v} = ${v};`);
    out.handles.push(v);
    out.updates.push({v, styleAssigns: staticAssigns, text: null, dynAssigns});
  } else {
    out.build.push(`    er_props_default(&p);`);
    for (const a of staticAssigns)
      out.build.push(`    p.${a.field} = ${a.expr};`);
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
    if (attr.type !== 'JSXAttribute')
      throw aotError('AOT: spread props on <FlatList> are not supported');
    const name = attr.name.name;
    if (name === 'data') dataNode = attrExpr(attr);
    else if (name === 'renderItem') renderItem = attrExpr(attr);
    else if (name === 'style') styleAttr = attr;
    else if (name === 'keyExtractor' || name === 'ref' || name === 'key') {
      /* ignored — the AOT unrolls at compile time, so React keys are irrelevant */
    } else
      throw aotError(
        `AOT: <FlatList> prop "${name}" is not supported`,
        'supported: data, renderItem, keyExtractor, style. For headers/footers/horizontal/onEndReached etc., use <ScrollView> + .map directly.',
      );
  }
  if (!dataNode)
    throw aotError(
      'AOT: <FlatList> needs a data prop',
      '<FlatList data={items} renderItem={({ item }) => <Row item={item} />} />',
    );
  if (!renderItem || !isFn(renderItem))
    throw aotError(
      'AOT: <FlatList> needs a renderItem function',
      'renderItem={({ item, index }) => <Row item={item} />}',
    );
  const param = renderItem.params[0];
  if (!param || param.type !== 'ObjectPattern')
    throw aotError(
      'AOT: FlatList renderItem must destructure ({ item, index })',
      'renderItem={({ item }) => <Row item={item} />}',
    );
  let itemName = null;
  let indexName = null;
  for (const prop of param.properties) {
    if (prop.type !== 'ObjectProperty' || prop.value.type !== 'Identifier')
      throw aotError(
        'AOT: FlatList renderItem may destructure only item / index (to plain names)',
      );
    if (prop.key.name === 'item') itemName = prop.value.name;
    else if (prop.key.name === 'index') indexName = prop.value.name;
    else
      throw aotError(
        `AOT: FlatList renderItem cannot destructure "${prop.key.name}" (only item / index)`,
      );
  }
  if (!itemName)
    throw aotError(
      'AOT: FlatList renderItem must destructure item',
      'renderItem={({ item }) => …}',
    );

  // Rewrite renderItem `({ item, index }) => BODY` → a positional `.map` callback `(item, index) => BODY`.
  const cbParams = [{type: 'Identifier', name: itemName}];
  if (indexName) cbParams.push({type: 'Identifier', name: indexName});
  const cb = {
    type: 'ArrowFunctionExpression',
    params: cbParams,
    body: renderItem.body,
    async: false,
    expression: renderItem.body.type !== 'BlockStatement',
  };
  const mapCall = {
    type: 'CallExpression',
    callee: {
      type: 'MemberExpression',
      object: dataNode,
      property: {type: 'Identifier', name: 'map'},
      computed: false,
    },
    arguments: [cb],
  };
  const scrollView = {
    type: 'JSXElement',
    openingElement: {
      type: 'JSXOpeningElement',
      name: {type: 'JSXIdentifier', name: 'ScrollView'},
      attributes: styleAttr ? [styleAttr] : [],
      selfClosing: false,
    },
    closingElement: {
      type: 'JSXClosingElement',
      name: {type: 'JSXIdentifier', name: 'ScrollView'},
    },
    children: [{type: 'JSXExpressionContainer', expression: mapCall}],
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

/** Resolves an <Image source>/imageName expression to its baked asset NAME (a string) if it folds at compile
 *  time, else null (a runtime/dynamic source — the caller emits a dynamic image_name). Image imports live in
 *  the const scope as their asset-name string, so this folds `wxSun`, `item.icon` (unrolled map), and static
 *  ternaries; `{ uri }` is the explicit remote-shape escape. */
function imageNameFromSource(expr, env) {
  if (!expr) return null;
  try {
    const v = evalStatic(expr, env.consts ?? {});
    if (typeof v === 'string') return v;
  } catch {
    /* not a compile-time constant — fall through to {uri}, else dynamic */
  }
  if (expr.type === 'ObjectExpression') {
    // source={{ uri: 'wx_sun' }} — RN's remote-image shape; here the uri IS the baked asset name.
    const uri = expr.properties.find(
      p =>
        p.type === 'ObjectProperty' &&
        (p.key.name === 'uri' || p.key.value === 'uri'),
    );
    if (uri?.value?.type === 'StringLiteral') return uri.value.value;
  }
  return null;
}

/** Resolves an <Image>'s source/imageName/resizeMode/tintColor for the node props (static name, a dynamic
 *  name expr, resize mode, tint). Records baking intent in `out`: a static name that matches an import is
 *  marked used (out.images); a dynamic source flips out.bakeAllImages (its asset can't be enumerated). So
 *  only REACHED images are baked — an import used solely in a folded-away branch costs no flash. */
function resolveImageAttrs(el, env, out) {
  const attrs = el.openingElement.attributes;
  const find = n =>
    attrs.find(a => a.type === 'JSXAttribute' && a.name.name === n);
  const imAttr = find('imageName');
  const srcAttr = find('source');
  const srcExpr = imAttr
    ? attrExpr(imAttr)
    : srcAttr
      ? attrExpr(srcAttr)
      : null;
  let imageName = null; // static asset name (a literal), OR …
  let imageNameDyn = null; // … a runtime C string expr (a list-item field / state) set in app_update.
  if (srcExpr) {
    imageName = imageNameFromSource(srcExpr, env);
    if (imageName != null) {
      const path = env.imageNames?.get(imageName); // a baked import (vs a bare {uri} name the app supplies)
      if (path) out.images.set(imageName, path);
    } else {
      // Dynamic source: emit it as a runtime string. The engine resolves it against the image registry by
      // name each frame, so the candidate assets must be baked — and they can't be enumerated, so bake them
      // ALL (out.bakeAllImages). Only triggered when a dynamic source is actually REACHED.
      out.bakeAllImages = true;
      const e = emitExpr(srcExpr, env);
      if (e.cType !== 'string') {
        const err = aotError(
          'AOT: an <Image source> must resolve to an asset NAME (a string)',
          "use an imported image (`import logo from './logo.png'` → source={logo}), a string asset name, source={{ uri: 'name' }}, or a string-valued state / list-item field for a dynamic source.",
        );
        if (srcExpr.loc) err.aotLoc = srcExpr.loc.start;
        throw err;
      }
      imageNameDyn = e.code;
    }
  }
  let resizeMode = null;
  const rmAttr = find('resizeMode');
  if (rmAttr) {
    const rm = evalStaticOr(attrExpr(rmAttr), env, null);
    resizeMode = RESIZE_MODES[rm];
    if (!resizeMode) {
      const e = aotError(
        `AOT: unsupported <Image resizeMode> "${rm}"`,
        `resizeMode must be one of: ${Object.keys(RESIZE_MODES).join(' / ')}.`,
      );
      if (rmAttr.loc) e.aotLoc = rmAttr.loc.start;
      throw e;
    }
  }
  let tintColor = null;
  const tcAttr = find('tintColor');
  if (tcAttr) {
    const tc = evalStaticOr(attrExpr(tcAttr), env, null);
    if (typeof tc === 'string' || typeof tc === 'number')
      tintColor = argbLiteral(tc);
  }
  return {imageName, imageNameDyn, resizeMode, tintColor};
}

// ---------------------------------------------------------------------------------------------------
// Node emitter — the element dispatcher + the generic host node. emitNodeImpl is the entry point for
// every JSX element: it routes Svg / typed components / FlatList / components to their emitters above,
// and handles the generic host nodes (View / Text / Pressable / TouchableOpacity / Image / ScrollView)
// itself — style,
// text, events, refs, children. emitNode wraps it with withLoc so a thrown AOT error gets a location.
// ---------------------------------------------------------------------------------------------------

// The JS-only RN wrappers the package exports that this compiler cannot lower yet. Each is a fixed
// rewrite over primitives the AOT DOES understand, so the fix is always to write that tree out — the
// message says which one, because a stock "unknown element" here is actively misleading (the import
// resolved, the simulator renders it, and the name is spelled right).
const FLOW_A_ONLY_COMPONENTS = {
  Button:
    '<Pressable style={…} onPress={…}><Text style={…}>title</Text></Pressable>.',
  ImageBackground:
    "<View style={…}><Image source={…} style={{position: 'absolute', top: 0, left: 0, right: 0, bottom: 0}} />…children…</View>.",
  SectionList:
    '<ScrollView> with the header element and a data.map(…) per section (a section is not one .map, which is why it has no lowering).',
};

// <TouchableOpacity> is a Pressable node plus one synthetic animated value bound to its opacity:
// press-in snaps it to activeOpacity, press-out fades it back to whatever the style asks for. Flow A
// builds the same thing out of hooks (TouchableOpacity.js); lowering it here means the feedback runs on
// the engine's native driver with no JS on the device at all.
const TOUCHABLE_ACTIVE_OPACITY = 0.2; // RN's default
const TOUCHABLE_RESTORE_MS = 250; // RN's fade-back; the dim itself lands with no ramp

/** The press events a <TouchableOpacity disabled> drops — RN presses none of them, and dims for none. */
const PRESS_EVENTS = new Set([
  'onPress',
  'onLongPress',
  'onPressIn',
  'onPressOut',
]);

/** A JSX element's named attribute, or undefined. */
function namedAttr(openingElement, name) {
  return openingElement.attributes.find(
    a => a.type === 'JSXAttribute' && a.name && a.name.name === name,
  );
}

/** True for `<TouchableOpacity disabled>` / `disabled={SOME_CONST}`. Must fold: `disabled` decides what
 *  the build EMITS — the press handlers and the dim binding — so there is nothing left to decide at runtime. */
function touchableIsDisabled(el, scope) {
  const attr = namedAttr(el.openingElement, 'disabled');
  if (!attr) return false;
  return !!evalStaticOrThrow(
    attrExpr(attr),
    scope,
    'AOT: <TouchableOpacity disabled> must be a compile-time constant',
    'a disabled one still renders — it lowers to a plain <Pressable>, children, layout and style intact — but its press handlers and its dim are not emitted at all, so the AOT has to know at build time. To gate on state, leave it enabled and return early in the handler (`onPress={() => { if (!ready) return; … }}`).',
  );
}

/** A folded `activeOpacity` as a number in 0..1. Everything else is an error HERE, where the source line
 *  is still in reach: it would otherwise reach floatLit and land in the generated C as a literal that
 *  either does not compile (`NaNf`) or quietly means something the app never asked for — a bare
 *  `activeOpacity` is `true`, so `1.0f`, which is no dim at all; `{false}` is an invisible one. */
function opacityLiteralOrThrow(value, attr) {
  if (
    typeof value !== 'number' ||
    !Number.isFinite(value) ||
    value < 0 ||
    value > 1
  ) {
    // JSON.stringify turns NaN into the STRING "null", which reads as a different mistake entirely.
    const shown =
      typeof value === 'number'
        ? String(value)
        : (JSON.stringify(value) ?? String(value));
    const e = aotError(
      `AOT: <TouchableOpacity activeOpacity> must be a number between 0 and 1 (got ${shown})`,
      'it is the opacity the node dims to while held — 0 is invisible, 1 is no dim at all. RN defaults it to 0.2.',
    );
    if (attr.loc) e.aotLoc = attr.loc.start;
    throw e;
  }
  return value;
}

/**
 * Creates a <TouchableOpacity>'s animated value and binds it to the node's opacity. Returns the two
 * fades as handler-body lines, for the event loop to fold into its press handlers.
 */
function touchablePressFades(
  el,
  v,
  staticAssigns,
  dynAssigns,
  binds,
  out,
  scope,
) {
  const animatedOpacity = binds.some(b => b.prop === 'ER_PROP_OPACITY');
  if (animatedOpacity || dynAssigns.some(a => a.field === 'opacity'))
    throw aotError(
      `AOT: <TouchableOpacity> cannot take ${animatedOpacity ? 'an Animated' : 'a state-driven'} opacity`,
      "the press feedback owns this node's opacity — a second writer does not blend with it, it races it. To animate opacity yourself, use <Pressable>: the dim is just onPressIn/onPressOut driving an Animated.Value (see TouchableOpacity.js for the whole of it).",
    );
  // lowerStyle has already scaled a static opacity to 0–255, and that byte IS the resting value. It is
  // NaN for a style opacity that was not a number — which lowerStyle emits as a `p.opacity = NaN` the C
  // compiler rejects on its own, so leave that error to it rather than adding a second bad literal here.
  const restingByte = staticAssigns.find(a => a.field === 'opacity');
  const restingRaw = restingByte ? Number(restingByte.expr) / 255 : 1;
  const resting = Number.isFinite(restingRaw) ? restingRaw : 1;

  const activeAttr = namedAttr(el.openingElement, 'activeOpacity');
  const active = activeAttr
    ? opacityLiteralOrThrow(
        evalStaticOrThrow(
          attrExpr(activeAttr),
          scope,
          'AOT: <TouchableOpacity activeOpacity> must be a compile-time constant',
          'the dim target is baked into the generated handler, so it cannot come from state.',
        ),
        activeAttr,
      )
    : TOUCHABLE_ACTIVE_OPACITY;

  const cVar = `s_av_press_${v}`;
  out.childAnims.push({cVar, initCode: floatLit(resting)});
  out.build.push(`    er_anim_value_bind(${cVar}, ${v}, ER_PROP_OPACITY);`);

  const fade = (to, ms) => [
    '    {',
    '        ERAnimConfig cfg;',
    '        memset(&cfg, 0, sizeof(cfg));',
    '        cfg.type = ER_ANIM_TIMING;',
    `        cfg.duration_ms = ${ms};`,
    '        cfg.easing = ER_EASE_QUAD_IN_OUT;',
    `        er_anim_value_animate(${cVar}, ${floatLit(to)}, &cfg);`,
    '    }',
  ];
  return {
    onPressIn: fade(active, 0),
    onPressOut: fade(resting, TOUCHABLE_RESTORE_MS),
  };
}

function emitNodeImpl(el, scope, out, env, state, opts = {}) {
  el = omitUndefinedAttrs(el, scope, env);
  const tag = resolveTag(el.openingElement);
  if (tag === 'Svg') return emitSvg(el, scope, out, env, state, opts);
  if (tag === 'Switch') return emitSwitch(el, scope, out, env, state);
  if (tag === 'Dial') return emitDial(el, scope, out, env, state);
  if (tag === 'TextInput') return emitTextInput(el, scope, out, env, state);
  if (tag === 'ActivityIndicator')
    return emitActivityIndicator(el, scope, out, env);
  if (tag === 'Modal') return emitModal(el, scope, out, env, state);
  if (tag === 'FlatList') return emitFlatList(el, scope, out, env, state, opts);
  const nodeType = NODE_TYPES[tag];
  if (!nodeType) {
    if (out.components.has(tag))
      return emitComponent(el, scope, out, env, state, opts);
    // Exported by the package and rendering fine in the simulator, but with no lowering here yet — so
    // say that, instead of "unknown element", which reads like a typo and sends people hunting for one.
    const flowAOnly = FLOW_A_ONLY_COMPONENTS[tag];
    if (flowAOnly)
      throw aotError(
        `AOT: <${tag}> is not supported in Flow B yet`,
        `it renders in Flow A (the simulator and the QuickJS runtime) but the AOT has no lowering for it. Write it out by hand for the device build: ${flowAOnly}`,
      );
    throw aotError(
      `AOT: unknown element <${tag}> (not a built-in or a component in this file)`,
      `<${tag}> must be a built-in (View / Text / Pressable / TouchableOpacity / Image / ScrollView / Svg + shapes / Animated.*) or a function component defined in THIS file. Check the import/spelling, or define the component here.`,
    );
  }

  // A <TouchableOpacity disabled> lowers to a plain Pressable node — children, layout and style all
  // intact — with no press handlers and no animated value to dim it with. That is RN's `disabled`, and
  // it is the whole of it here: the engine has no such node flag, so a node left holding onPress would
  // still fire, and one left holding the binding would still dim.
  const touchableOff =
    tag === 'TouchableOpacity' && touchableIsDisabled(el, scope);

  // Spread attributes on a host element (`<View {...props} />`) aren't lowered — the style/event loops
  // below only read named JSXAttributes, so a spread would be SILENTLY dropped. The one exception is
  // `{...pan.panHandlers}`, which the AOT understands as a whole (see emitPanResponder); everything else
  // is rejected explicitly (the typed components — Switch/TextInput/Modal/… — already throw on spreads).
  // Pin the location to the spread itself, not the whole element, for a precise code-frame.
  const panSpreads = [];
  for (const spread of el.openingElement.attributes) {
    if (spread.type !== 'JSXSpreadAttribute') continue;
    let pan;
    try {
      pan = panSpreadTarget(spread.argument, env);
    } catch (e) {
      if (spread.loc && !e.aotLoc) e.aotLoc = spread.loc.start;
      throw e;
    }
    if (pan) {
      panSpreads.push(pan);
      continue;
    }
    const e = aotError(
      `AOT: a spread {...} on <${tag}> is not supported`,
      `list each prop explicitly (e.g. style={…} onPress={…}). The only spread a host element understands is a PanResponder's ({...pan.panHandlers}); otherwise spread props are supported on a function component instance whose spread object folds to a compile-time constant.`,
    );
    if (spread.loc) e.aotLoc = spread.loc.start;
    throw e;
  }

  const v = `n${out.n++}`;
  const {staticAssigns, dynAssigns, binds} = collectStyleAssigns(
    el.openingElement,
    scope,
    env,
  );
  // A <Text> with a nested <Text> becomes inline SPANS; otherwise a single (possibly dynamic) string.
  const spans =
    tag === 'Text' ? collectTextSpans(el.children, scope, env) : null;
  const text =
    tag === 'Text' && !spans ? buildText(el.children, scope, env) : null;
  // An <Image>'s baked-asset name + resize/tint. resize/tint are static → fold into staticAssigns so both
  // the static and (deferred) dynamic paths apply them; the asset name is a char buffer (set via snprintf),
  // either a compile-time literal (image.imageName) or a runtime string expr (image.imageNameDyn).
  const image = tag === 'Image' ? resolveImageAttrs(el, env, out) : null;
  if (image?.resizeMode)
    staticAssigns.push({field: 'resize_mode', expr: image.resizeMode});
  if (image?.tintColor)
    staticAssigns.push({field: 'tint_color', expr: image.tintColor});

  // `visible` is the prop spelling of `display` (props.js does the same mapping in Flow A). An
  // explicit style `display` WINS, exactly as it does there, so the two spellings cannot disagree.
  // This lives here rather than in collectStyleAssigns because that helper is shared with <Modal> —
  // whose `visible` is its own show/hide prop, lowered to modal_visible by emitModal — and with the
  // typed components, which reject any prop they do not whitelist. All of those dispatch before this
  // point, so they are structurally out of reach.
  const visibleAttr = el.openingElement.attributes.find(
    a => a.type === 'JSXAttribute' && a.name && a.name.name === 'visible',
  );
  if (
    visibleAttr &&
    !staticAssigns.some(a => a.field === 'display') &&
    !dynAssigns.some(a => a.field === 'display')
  ) {
    const expr = visibleAttr.value == null ? null : attrExpr(visibleAttr); // bare `visible` === true
    if (expr == null) {
      staticAssigns.push({field: 'display', expr: 'ER_DISPLAY_FLEX'});
    } else {
      try {
        staticAssigns.push({
          field: 'display',
          expr: evalStatic(expr, scope) ? 'ER_DISPLAY_FLEX' : 'ER_DISPLAY_NONE',
        });
      } catch {
        dynAssigns.push({
          field: 'display',
          code: `((${emitExpr(expr, env).code}) ? ER_DISPLAY_FLEX : ER_DISPLAY_NONE)`,
        });
      }
    }
  }

  // `delayLongPress` is the hold time before onLongPress fires (RN's prop, same 500 ms default). It has
  // to fold: the value is baked into the node's props at build time. 0 is the engine's "no preference"
  // sentinel, so an app asking for 0 gets 1 ms — the next tick, which is what RN's setTimeout(0) means.
  const delayAttr = namedAttr(el.openingElement, 'delayLongPress');
  if (delayAttr) {
    const bad = aotError(
      `AOT: <${tag} delayLongPress> must fold to a number`,
      'pass a literal or a module-level constant in milliseconds, e.g. delayLongPress={800}.',
    );
    if (delayAttr.value == null) throw bad;
    let ms;
    try {
      ms = Number(evalStatic(attrExpr(delayAttr), scope));
    } catch {
      throw bad;
    }
    if (!Number.isFinite(ms)) throw bad;
    staticAssigns.push({
      field: 'long_press_ms',
      expr: String(Math.min(65535, Math.max(1, Math.round(ms)))),
    });
  }

  // `displayCode` toggles show/hide for a state-driven conditional: the node is always built, its
  // `display` flips between flex and none in app_update (joining any state-driven style assigns).
  // Pushed last so an enclosing conditional beats the element's own visible/display — app_update
  // applies staticAssigns before dynAssigns, so a dynamic toggle also beats a static one.
  if (opts.displayCode)
    dynAssigns.push({
      field: 'display',
      code: `((${opts.displayCode}) ? ER_DISPLAY_FLEX : ER_DISPLAY_NONE)`,
    });

  const isDynamic =
    !!text?.dynamic || dynAssigns.length > 0 || !!image?.imageNameDyn;

  out.build.push(`    ${v} = er_node_create(${nodeType});`);
  if (isDynamic) {
    // Props are (re)applied in app_update(); here just create the node and remember its handle. A dynamic
    // <Image> carries its runtime asset name (imageName) so app_update re-snprintf's p.image_name each pass.
    out.build.push(`    s_${v} = ${v};`);
    out.handles.push(v);
    out.updates.push({
      v,
      styleAssigns: staticAssigns,
      text,
      dynAssigns,
      imageName: image?.imageNameDyn,
    });
  } else {
    out.build.push(`    er_props_default(&p);`);
    for (const a of staticAssigns)
      out.build.push(`    p.${a.field} = ${a.expr};`);
    if (text)
      out.build.push(
        `    snprintf(p.text, sizeof(p.text), "%s", ${cstr(text.format.replace(/%%/g, '%'))});`,
      );
    if (image?.imageName != null)
      out.build.push(
        `    snprintf(p.image_name, sizeof(p.image_name), "%s", ${cstr(image.imageName)});`,
      );
    out.build.push(`    er_node_set_props(${v}, &p);`);
  }

  // Inline text spans (a <Text> with nested <Text>): set once; each span inherits the node's base style
  // unless it overrides (the local array is copied by er_node_set_text_spans).
  if (spans) {
    out.build.push(`    {`, `        static const ERTextSpan spans_${v}[] = {`);
    for (const s of spans)
      out.build.push(
        `            { ${s.text}, ${s.color}, ${s.font_size}, ${s.font_weight}, ${s.font_style}, ${s.text_decoration}, ${s.letter_spacing} },`,
      );
    out.build.push(
      `        };`,
      `        er_node_set_text_spans(${v}, spans_${v}, ${spans.length});`,
      `    }`,
    );
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

  // <TouchableOpacity>'s own opacity binding, on top of any the style asked for.
  const pressFades =
    tag === 'TouchableOpacity' && !touchableOff
      ? touchablePressFades(el, v, staticAssigns, dynAssigns, binds, out, scope)
      : null;

  emitRefBind(v, el.openingElement, out, env);

  const fadesWired = new Set();
  for (const attr of el.openingElement.attributes) {
    if (attr.type !== 'JSXAttribute') continue;
    const evt = EVENT_TYPES[attr.name.name];
    if (!evt) continue;
    if (touchableOff && PRESS_EVENTS.has(attr.name.name)) continue;
    const fn = attrExpr(attr);
    let handlerName;
    if (fn.type === 'Identifier' && env.callbacks?.has(fn.name)) {
      // onPress={fn} where fn is a useCallback → emit one shared handler, reused across elements. The
      // cbPrefix namespaces it per child instance (so two instances' same-named callbacks stay distinct,
      // each compiled in its own env/state); '' for the App — unchanged.
      const key = `${env.cbPrefix ?? ''}${fn.name}`;
      handlerName = out.cbEmitted.get(key);
      if (!handlerName) {
        handlerName = `er_cb_${key}`;
        out.cbEmitted.set(key, handlerName);
        out.handlers.push({
          name: handlerName,
          body: compileHandler(env.callbacks.get(fn.name), env, state, out),
        });
      }
    } else if (fn.type === 'Identifier' && env.fnProps?.has(fn.name)) {
      // Callback prop: <Child onTap={() => …}/> where Child does onPress={onTap}. Inline the CALLER's
      // function as this handler, compiled in the caller's env/state so its setters/locals resolve there.
      const fp = env.fnProps.get(fn.name);
      handlerName = `er_handler_${out.handlers.length}`;
      out.handlers.push({
        name: handlerName,
        body: compileHandler(fp.node, fp.env, fp.state, out),
      });
    } else if (isFn(fn)) {
      handlerName = `er_handler_${out.handlers.length}`;
      out.handlers.push({
        name: handlerName,
        body: compileHandler(fn, env, state, out),
      });
    } else {
      throw aotError(
        `AOT: ${attr.name.name} must be an inline function, a useCallback, or a callback prop`,
        `pass an inline arrow (onPress={() => setX(…)}), a useCallback identifier, or a function prop received by this component.`,
      );
    }
    // The app's own press-in/out on a <TouchableOpacity>: wrap it rather than prepend the fade to its
    // body, which for a shared useCallback handler would dim every OTHER element using it too.
    const fade = pressFades?.[attr.name.name];
    if (fade) {
      const wrapped = `er_handler_${out.handlers.length}`;
      out.handlers.push({
        name: wrapped,
        body: [...fade, `    ${handlerName}(node, data, user_data);`],
      });
      handlerName = wrapped;
      fadesWired.add(attr.name.name);
    }
    out.build.push(`    er_event_set(${v}, ${evt}, ${handlerName}, NULL);`);
  }

  // Whichever end of the press the app did not handle itself is the fade on its own.
  if (pressFades) {
    for (const key of ['onPressIn', 'onPressOut']) {
      if (fadesWired.has(key)) continue;
      const name = `er_handler_${out.handlers.length}`;
      out.handlers.push({name, body: pressFades[key]});
      out.build.push(
        `    er_event_set(${v}, ${EVENT_TYPES[key]}, ${name}, NULL);`,
      );
    }
  }

  for (const pan of panSpreads) emitPanResponder(pan, v, out, env, state);

  if (tag !== 'Text') emitChildren(el.children, v, scope, out, env, state);
  return v;
}
/**
 * `el` without the attributes whose value folds to `undefined` in this scope — typically a prop a child
 * component was never given. Flow A omits an undefined prop, so every reader has to see it as absent;
 * read as a value, `visible` hid the node and `placeholder` printed the word "undefined". This is the one
 * place that rule lives: every element reaches its reader through here. A copy is returned only when
 * something is dropped, because the same JSX node is emitted once per scope (each .map row, each
 * component instance) and one scope's answer must not leak into another's.
 */
function omitUndefinedAttrs(el, scope, env) {
  const attrs = el.openingElement.attributes;
  // Built on first use: the runtime-aware scope, plus the global `undefined` (a reserved name — see
  // normalizeUndefined), so `SHOW ? true : undefined` with SHOW false folds too.
  let fold = null;
  const kept = attrs.filter(a => {
    if (a.type !== 'JSXAttribute' || a.value?.type !== 'JSXExpressionContainer')
      return true;
    fold ??= Object.assign(Object.create(foldScope(env, scope)), {undefined});
    try {
      return evalStatic(a.value.expression, fold) !== undefined;
    } catch {
      return true; // not a compile-time value — its reader decides
    }
  });
  return kept.length === attrs.length
    ? el
    : {...el, openingElement: {...el.openingElement, attributes: kept}};
}

const emitNode = withLoc(emitNodeImpl);

// ---------------------------------------------------------------------------------------------------
// On-screen keyboard config — lower a module-level setKeyboardConfig({...}) call to static C tables
// (ERKeyboardKey/Row/Layer + ERKeyboardConfig) that er_app_build hands to er_keyboard_set_config.
// ---------------------------------------------------------------------------------------------------

/** A keyboard-config color → C ARGB literal; null/undefined → "0" (the engine's "use default" sentinel). */
function kbdColor(v) {
  return v == null ? '0' : argbLiteral(v);
}

/** One JS keyboard key object → a C ERKeyboardKey initializer. `li` is its layer index (for shift highlight).
 *  Shapes: { char } (types it), { char:' ', span } (space), { label, layer } (switch), { label, backspace },
 *  { label, done }; optional span / highlight. */
function kbdKeyToC(k, li) {
  if (k == null || typeof k !== 'object')
    throw aotError(
      'AOT: each setKeyboardConfig key must be an object',
      'e.g. { char: "q" } or { label: "shift", layer: 1, highlight: true }',
    );
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
    throw aotError(
      'AOT: a setKeyboardConfig key needs one of char / layer / backspace / done',
    );
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
    if (
      stmt.type === 'ExpressionStatement' &&
      stmt.expression.type === 'CallExpression' &&
      stmt.expression.callee.type === 'Identifier' &&
      stmt.expression.callee.name === 'setKeyboardConfig'
    ) {
      arg = stmt.expression.arguments[0];
      break;
    }
  }
  if (!arg) return;
  let cfg;
  try {
    cfg = evalStatic(arg, {});
  } catch {
    throw aotError(
      'AOT: setKeyboardConfig(...) needs a statically-foldable config object',
      'pass an object literal of colours/sizes (+ an optional `layers` array) — no state or runtime values.',
    );
  }
  if (cfg == null || typeof cfg !== 'object')
    throw aotError('AOT: setKeyboardConfig(...) needs a config object');

  const data = [];
  let layersExpr = 'NULL';
  let layerCount = 0;
  if (Array.isArray(cfg.layers)) {
    const layerVars = [];
    cfg.layers.forEach((layer, li) => {
      if (!Array.isArray(layer))
        throw aotError(
          'AOT: setKeyboardConfig `layers[i]` must be an array of rows',
        );
      const rowVars = [];
      layer.forEach((row, ri) => {
        if (!Array.isArray(row) || !row.length)
          throw aotError(
            'AOT: each keyboard row must be a non-empty array of keys',
          );
        data.push(
          `static const ERKeyboardKey kbd_l${li}r${ri}[] = { ${row.map(k => kbdKeyToC(k, li)).join(', ')} };`,
        );
        rowVars.push(`{ kbd_l${li}r${ri}, ${row.length} }`);
      });
      data.push(
        `static const ERKeyboardRow kbd_l${li}rows[] = { ${rowVars.join(', ')} };`,
      );
      layerVars.push(`{ kbd_l${li}rows, ${layer.length} }`);
    });
    data.push(
      `static const ERKeyboardLayer kbd_layers[] = { ${layerVars.join(', ')} };`,
    );
    layersExpr = 'kbd_layers';
    layerCount = cfg.layers.length;
  }
  const num = v => (v == null ? 0 : Math.round(Number(v)));
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

// --- TypeScript support (Flow B) ---------------------------------------------------------------------
// The compiler walks a plain JS+JSX AST. TypeScript apps (App.tsx) parse with the `typescript` plugin and
// are then scrubbed of all type-only syntax IN PLACE before the walker runs: the runtime nodes keep their
// original source locations, so code-frame errors still point at the user's real source. This is a faithful
// type strip — the generated C for an App.tsx is identical to its untyped App.jsx twin.

/** Is this app a TypeScript entry? Driven by the filename extension, or an explicit opts.ts (for tests). */
const isTsEntry = opts => opts.ts ?? /\.[cm]?tsx$/.test(opts.filename || '');

const parserPlugins = ts => (ts ? ['jsx', 'typescript'] : ['jsx']);
// Type-only expression wrappers: `x as T`, `x satisfies T`, `x!`, `<T>x`, `f<T>` — unwrap to the inner expr.
const TS_EXPR_WRAPPERS = new Set([
  'TSAsExpression',
  'TSSatisfiesExpression',
  'TSNonNullExpression',
  'TSTypeAssertion',
  'TSInstantiationExpression',
]);
// Type-only declarations (no runtime presence) — dropped from any statement/body list.
const TS_TYPE_DECLS = new Set([
  'TSInterfaceDeclaration',
  'TSTypeAliasDeclaration',
  'TSDeclareFunction',
]);
// Type-only fields hung off otherwise-runtime nodes — deleted so nothing downstream traverses into them.
const TS_TYPE_FIELDS = [
  'typeAnnotation',
  'returnType',
  'typeParameters',
  'typeArguments',
  'accessibility',
  'definite',
  'declare',
  'readonly',
  'override',
  'abstract',
];
// Keys that never hold child AST nodes — skip them so the scrub stays cheap and never mangles metadata.
const TS_SKIP_KEYS = new Set([
  'loc',
  'start',
  'end',
  'range',
  'leadingComments',
  'trailingComments',
  'innerComments',
  'comments',
  'extra',
  'tokens',
]);

/** True for nodes that carry no runtime meaning and must be removed from a statement/specifier list. */
const isTypeOnly = node =>
  !!node &&
  (TS_TYPE_DECLS.has(node.type) ||
    // `import type ... ` / `export type ...`, and per-specifier `import { type X }`.
    ((node.type === 'ImportDeclaration' || node.type === 'ImportSpecifier') &&
      node.importKind === 'type') ||
    ((node.type === 'ExportNamedDeclaration' ||
      node.type === 'ExportSpecifier') &&
      node.exportKind === 'type'));

/** Follow a chain of type-only expression wrappers to the runtime expression underneath. */
const unwrapTs = node => {
  while (node && TS_EXPR_WRAPPERS.has(node.type)) node = node.expression;
  return node;
};

/**
 * Strip every TypeScript-only construct from a parsed AST, in place: drop type declarations and type
 * imports, unwrap `as`/`!`/`<T>` expression wrappers, and delete type-annotation fields. Remaining nodes
 * keep their .loc, so the compiler's error code-frames stay accurate.
 */
function stripTypeScript(root) {
  const visit = node => {
    if (!node || typeof node !== 'object') return;
    for (const f of TS_TYPE_FIELDS) if (f in node) delete node[f];
    for (const key of Object.keys(node)) {
      if (TS_SKIP_KEYS.has(key)) continue;
      let val = node[key];
      if (Array.isArray(val)) {
        const kept = [];
        for (let el of val) {
          if (isTypeOnly(el)) continue;
          if (el && TS_EXPR_WRAPPERS.has(el.type)) el = unwrapTs(el);
          kept.push(el);
          visit(el);
        }
        node[key] = kept;
      } else if (val && typeof val.type === 'string') {
        if (TS_EXPR_WRAPPERS.has(val.type)) {
          val = unwrapTs(val);
          node[key] = val;
        }
        visit(val);
      } else if (val && typeof val === 'object') {
        visit(val);
      }
    }
  };
  visit(root);
  return root;
}

/**
 * One pass over the parsed program that fixes what `undefined` means before anything is compiled.
 *
 * - A JSX attribute whose value is `{undefined}` is DROPPED. Flow A's buildProps omits an undefined prop
 *   (`props[k] !== undefined`), so the node keeps its default — `visible={undefined}` stays visible. Doing
 *   it here gives the twenty-odd prop readers that view at once, instead of each deciding what an
 *   undefined value means (one hid the node, one crashed on Object.entries(undefined)).
 *   A value that only becomes undefined in some scope — a prop a child was never given — is dropped per
 *   emission by omitUndefinedAttrs. This literal pass still matters for <Svg> shapes, which bypass it.
 * - A BINDING named `undefined` is refused. JS allows shadowing the global, but Flow B gives `undefined`
 *   a meaning of its own (an omitted prop, empty text), and a shadowing local would silently lose to it.
 */
function normalizeUndefined(ast) {
  const reserved = id => {
    if (id?.type !== 'Identifier' || id.name !== 'undefined') return;
    const e = aotError(
      'AOT: `undefined` cannot be used as a name',
      'Flow B reads `undefined` as the global everywhere — an omitted prop, or empty text — so a binding with that name would never be read. Rename it.',
    );
    if (id.loc) e.aotLoc = id.loc.start;
    throw e;
  };
  const pattern = p => {
    if (!p) return;
    if (p.type === 'Identifier') reserved(p);
    else if (p.type === 'ArrayPattern') p.elements.forEach(pattern);
    else if (p.type === 'ObjectPattern')
      p.properties.forEach(q =>
        pattern(q.type === 'RestElement' ? q.argument : q.value),
      );
    else if (p.type === 'AssignmentPattern') pattern(p.left);
    else if (p.type === 'RestElement') pattern(p.argument);
  };
  const isUndefinedAttr = a =>
    a.type === 'JSXAttribute' &&
    a.value?.type === 'JSXExpressionContainer' &&
    a.value.expression.type === 'Identifier' &&
    a.value.expression.name === 'undefined';
  const SKIP = new Set([
    'loc',
    'start',
    'end',
    'extra',
    'leadingComments',
    'trailingComments',
    'innerComments',
  ]);
  const visit = node => {
    if (!node || typeof node.type !== 'string') return;
    switch (node.type) {
      case 'VariableDeclarator':
        pattern(node.id);
        break;
      case 'FunctionDeclaration':
      case 'FunctionExpression':
      case 'ArrowFunctionExpression':
      case 'ObjectMethod':
      case 'ClassMethod':
        if (node.id) reserved(node.id);
        node.params.forEach(pattern);
        break;
      case 'CatchClause':
        pattern(node.param);
        break;
      case 'ClassDeclaration':
      case 'ClassExpression':
        if (node.id) reserved(node.id);
        break;
      case 'ImportSpecifier':
      case 'ImportDefaultSpecifier':
      case 'ImportNamespaceSpecifier':
        reserved(node.local);
        break;
      case 'JSXOpeningElement':
        node.attributes = node.attributes.filter(a => !isUndefinedAttr(a));
        break;
    }
    for (const key of Object.keys(node)) {
      if (SKIP.has(key)) continue;
      const v = node[key];
      if (Array.isArray(v)) v.forEach(visit);
      else if (v && typeof v.type === 'string') visit(v);
    }
  };
  visit(ast);
}

/** Parse an app entry to a JS+JSX AST, transparently stripping TypeScript when the entry is .ts/.tsx. */
function parseApp(src, opts = {}) {
  const ts = isTsEntry(opts);
  const ast = parse(src, {sourceType: 'module', plugins: parserPlugins(ts)});
  if (ts) stripTypeScript(ast);
  return ast;
}

/**
 * Compiles a Flow B app's JSX (or TSX) source to C.
 * @param {string} src   The App.jsx/App.tsx source text.
 * @param {string} demo  Demo name (only used in the generated-by header comment).
 * @param {object} opts  compileSource's options.
 * @param {Set<string>} wide   State/ref slots to declare 64-bit (see compileWidened).
 * @param {Set<string>} found  Collects the int slots a 64-bit timestamp was stored in.
 * @returns {{c: string, h: string, nodes: number, state: number, handlers: number, updates: number}}
 */
function compileSourceImpl(src, demo, opts, wide, found) {
  const ast = parseApp(src, opts);
  normalizeUndefined(ast);
  const shadowedClock = findShadowedClockCalls(ast);

  const screen = opts.screen ?? {width: SCREEN_W, height: SCREEN_H};
  // Image imports first, so their asset-name strings seed the module scope BEFORE its consts fold (a const
  // array of `{ icon: wxSun }` needs wxSun resolvable). An image import is just its baked-name string.
  const imageImports = collectImageImports(ast.program);
  const svgImports = collectSvgImports(ast.program); // <Svg source> imports → artifacts baked by the CLI (opts.svgArtifacts)
  const imageSeed = Object.fromEntries(
    [...imageImports].map(([local, imp]) => [local, imp.name]),
  );
  const scope = moduleScope(ast.program, screen, imageSeed);
  // Module bindings ALONE, before App's own consts are folded in below. A child component is declared at
  // module level, so its body closes over these — not over whatever App happens to have shadowed.
  const moduleConsts = {...scope};
  const component = findComponent(ast.program);
  // Fold statically-derived component-local consts (e.g. `const compact = screen.width < 400`) into the
  // const scope, so responsive `if` branches and styles can switch on them at compile time. Dynamic consts
  // (state-derived, useMemo, etc.) throw here and are skipped — they're handled later by memos/emitExpr.
  // Every name the body declares — a hook's destructured state included — shadows the module binding
  // before any local folds, or `const copy = r` would capture a module `r` that state `r` hides.
  for (const name of declaredNames(component.body.body)) delete scope[name];
  for (const stmt of component.body.body) {
    if (stmt.type !== 'VariableDeclaration' || stmt.kind !== 'const') continue;
    for (const decl of stmt.declarations) {
      if (decl.id.type !== 'Identifier' || !decl.init) continue;
      try {
        scope[decl.id.name] = evalStatic(decl.init, withUndefined(scope));
      } catch {
        // Dynamic (state-derived, useMemo, …). A memo is re-bound below and a plain derived const is not
        // supported — either way a same-named module const must not stay visible to the folds, or the
        // JSX quietly renders the module value. Mirrors the child-component path.
        delete scope[decl.id.name];
      }
    }
  }
  const state = collectState(component.body, scope, '', wide);
  for (const name of state.byName.keys()) delete scope[name];
  const rootJSX = findReturnJSX(component.body, scope);

  const anims = collectAnims(component.body, scope);
  const refs = collectRefs(component.body, scope, '', wide);
  for (const name of [...anims.keys(), ...refs.keys()]) delete scope[name];
  const pans = collectPanResponders(component.body);
  const callbacks = collectCallbacks(component.body);
  const memos = collectMemos(component.body);
  const helpers = collectHelpers(component.body, ast.program);
  const imageNames = new Map(
    [...imageImports].map(([, imp]) => [imp.name, imp.importPath]),
  ); // asset name → path
  const env = {
    state: state.byName,
    locals: new Map(),
    consts: scope,
    moduleConsts, // what an inlined child component sees (see emitComponent)
    anims,
    refs,
    pans,
    callbacks,
    helpers,
    imageNames,
    svgImports,
    svgArtifacts: opts.svgArtifacts || {},
    wide,
    found,
    shadowedClock,
  };
  // Resolve memos in declaration order: constant-fold into the const scope when possible, else register a
  // derived C expression in locals so each reference inlines it (the AOT has no per-render cache — the dep
  // tracking re-applies dependent nodes anyway). Done before emit so references resolve.
  for (const [name, expr] of memos) {
    try {
      scope[name] = evalStatic(expr, scope);
    } catch {
      const e = emitExprWide(expr, env);
      env.locals.set(name, {
        code: `(${e.code})`,
        cType: e.cType,
        isBool: e.isBool,
      });
      delete scope[name]; // a runtime binding beats a module const of its name in every fold
    }
  }
  const out = {
    n: 0,
    build: [],
    handlers: [],
    updates: [],
    handles: [],
    components: collectComponents(ast.program),
    cbEmitted: new Map(),
    vectorData: [],
    vectorBuilders: [],
    svgUpdates: [],
    svgN: 0,
    needsMath: false,
    timerFns: [],
    usesTimers: false,
    mountEffects: [],
    animCbs: [],
    seqN: 0,
    kbdData: '',
    kbdSetup: '',
    images: new Map(),
    bakeAllImages: false,
    instN: 0,
    childStateRecords: [],
    childRefs: [],
    childAnims: [],
    program: ast.program,
    effN: 0,
    effectFns: [],
    effectDecls: [],
    depEffects: [],
    panN: 0,
    panDecls: [],
    queries: [],
  };
  compileKeyboardConfig(ast.program, out); // module-level setKeyboardConfig({...}) → static ERKeyboardConfig
  const appTop = emitNode(rootJSX, scope, out, env, state);

  // Image baking: a REACHED static source registered its import in out.images during emit. If any reached
  // source is DYNAMIC (resolved by name at runtime), we can't enumerate it — fall back to baking every import.
  // Either way, an image used only in a folded-away branch (never emitted) costs no flash.
  if (out.bakeAllImages)
    for (const [, imp] of imageImports)
      out.images.set(imp.name, imp.importPath);

  // Effects: `useEffect(fn, [])` runs once at mount; `useEffect(fn, [dep…])` re-runs from app_update when a
  // dep changes (see compileEffect). Compiled after emit so out.timerFns/usesTimers reflect handler timers too.
  for (const eff of collectEffects(component.body)) {
    compileEffect(eff, env, state, out);
  }

  const nodeDecls = Array.from({length: out.n}, (_, i) => `n${i}`);
  // App state + every inlined child instance's per-instance state (each already namespaced via cField).
  const stateRecords = [...state.byName.values(), ...out.childStateRecords];
  const scalarRecords = stateRecords.filter(s => s.kind === 'scalar');
  const listRecords = stateRecords.filter(s => s.kind === 'list');

  // Scalar state → one ErAppState struct. List state → a fixed-capacity struct array + a count each.
  const fieldCDecl = f =>
    f.kind === 'string'
      ? `    char ${f.key}[${LIST_STR_CAP}];`
      : `    ${f.kind} ${f.key};`;
  const itemInit = (item, struct) =>
    `{ ${struct.fields.map(f => (f.kind === 'string' ? cstr(String(item[f.key] ?? '')) : f.kind === 'float' ? `${Number(item[f.key]) || 0}f` : String(Math.round(Number(item[f.key]) || 0)))).join(', ')} }`;
  const listBlocks = listRecords
    .map(
      s =>
        `typedef struct\n{\n${s.struct.fields.map(fieldCDecl).join('\n')}\n} ${s.cTypeName};\n\n` +
        `static ${s.cTypeName} ${s.arrayName}[${s.cap}] = {${s.items.map(it => '\n    ' + itemInit(it, s.struct)).join(',')}\n};\n` +
        `static int ${s.countMember} = ${s.items.length};\n`,
    )
    .join('\n');
  const scalarFieldDecl = s =>
    s.cType === 'string'
      ? `    char ${s.cField}[${LIST_STR_CAP}];`
      : `    ${cScalarType(s.cType)} ${s.cField};`;
  const scalarBlock = scalarRecords.length
    ? `typedef struct\n{\n${scalarRecords.map(scalarFieldDecl).join('\n')}\n} ErAppState;\n\nstatic ErAppState s_state = {${scalarRecords.map(s => ` .${s.cField} = ${s.initCode}`).join(',')} };\n`
    : '';
  const stateBlock = [scalarBlock, listBlocks].filter(Boolean).join('\n');

  const handleDecls = out.handles.map(v => `static ERNode* s_${v};`).join('\n');

  // Value refs — a plain mutable static each (escape-hatch state that does not trigger a re-render).
  // Refs the emitted C never touches (e.g. a `useRef(null)` that only holds a JS value) declare nothing,
  // so consumer builds don't warn about an unused static.
  const refDecls = [...refs.values(), ...out.childRefs]
    .filter(r => r.used)
    .map(
      r =>
        `static ${r.kind === 'value' ? cScalarType(r.cType) : r.cType} ${r.cVar} = ${r.initCode};`,
    )
    .join('\n');

  // Baked vector op-tapes + paint tables (static <Svg> geometry), emitted at file scope.
  const vectorBlock = out.vectorData.join('\n\n');
  // build_svgN() recompute functions (state-driven Svgs) — declared before app_update, which calls them.
  const vectorBuilderBlock = out.vectorBuilders.join('\n\n');

  // Animated values — one engine-side handle each, created at the top of er_app_build (binds reference them).
  const animList = [...anims.values(), ...out.childAnims];
  const animDecls = animList
    .map(a => `static ERAnimValueHandle ${a.cVar};`)
    .join('\n');
  const animCreate = animList
    .map(a => `    ${a.cVar} = er_anim_value_create(${a.initCode});`)
    .join('\n');

  const hasUpdate =
    out.updates.length > 0 ||
    out.svgUpdates.length > 0 ||
    out.depEffects.length > 0;
  const updateBlock = (() => {
    if (!hasUpdate) return '';
    const lines = ['static void app_update(void)', '{'];
    if (out.updates.length) lines.push('    ERProps p;');
    for (const u of out.updates) {
      lines.push(`    er_props_default(&p);`);
      for (const a of u.styleAssigns)
        lines.push(`    p.${a.field} = ${a.expr};`);
      for (const a of u.dynAssigns) lines.push(`    p.${a.field} = ${a.code};`);
      if (u.placeholder != null)
        lines.push(
          `    snprintf(p.placeholder, sizeof(p.placeholder), "%s", ${cstr(u.placeholder)});`,
        );
      if (u.imageName != null)
        lines.push(
          `    snprintf(p.image_name, sizeof(p.image_name), "%s", ${u.imageName});`,
        );
      if (u.text) {
        if (u.text.args.length)
          lines.push(
            `    snprintf(p.text, sizeof(p.text), ${cstr(u.text.format)}, ${u.text.args.join(', ')});`,
          );
        else
          lines.push(
            `    snprintf(p.text, sizeof(p.text), "%s", ${cstr(u.text.format.replace(/%%/g, '%'))});`,
          );
      }
      lines.push(`    er_node_set_props(s_${u.v}, &p);`);
    }
    // State-driven Svgs: recompute the op-tape from state and re-upload.
    for (const s of out.svgUpdates) {
      lines.push(`    build_svg${s.id}();`);
      lines.push(
        `    er_node_set_vector_ops(${s.nodeVar}, s_svg${s.id}_ops, ${s.len}, s_svg${s.id}_paints, ${s.nPaints}, ${s.nGrads ? `s_svg${s.id}_grads` : 'NULL'}, ${s.nGrads || 0});`,
      );
    }
    // Dep-driven useEffect: run each effect whose dependency value changed since the last app_update.
    for (const block of out.depEffects) lines.push(block);
    lines.push('}');
    return lines.join('\n');
  })();

  // PanResponder gesture state (one small block per responder) + the should-set predicates, which are
  // bool-returning callbacks the engine calls during negotiation rather than ordinary event handlers.
  const panDeclBlock = out.panDecls.join('\n');
  const queryDefs = out.queries
    .map(
      q =>
        `static bool ${q.name}(ERNode* node, const EREventData* data, void* user_data)\n{\n    (void)node;\n    (void)data;\n    (void)user_data;\n    return ${q.expr};\n}`,
    )
    .join('\n\n');

  const handlerDefs = out.handlers
    .map(
      h =>
        `static void ${h.name}(ERNode* node, const EREventData* data, void* user_data)\n{\n    (void)node;\n    (void)data;\n    (void)user_data;\n${h.body.join('\n')}\n}`,
    )
    .join('\n\n');

  // Dep-driven useEffect: a static "previous value" per dependency, a forward decl (app_update calls each
  // er_effect_N before it's defined), and the effect body as a parameterless C function.
  const effectDeclsBlock = out.effectDecls.join('\n');
  const effectFwdDecls = out.effectFns
    .map(f => `static void ${f.name}(void);`)
    .join('\n');
  const effectFnDefs = out.effectFns
    .map(f => `static void ${f.name}(void)\n{\n${f.body.join('\n')}\n}`)
    .join('\n\n');

  // setInterval/setTimeout → a small fixed timer table advanced by er_app_tick(dt) (the host calls it each
  // frame). The table + er_timer_add are emitted only when timers are used (er_timer_clear only when
  // something calls it — see timerClearBlock); er_app_tick is always defined (a no-op otherwise) so a host
  // can call it unconditionally. Timer callbacks become parameterless C functions.
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
    .map(t => `static void ${t.name}(void)\n{\n${t.body.join('\n')}\n}`)
    .join('\n\n');

  // Timer callbacks are defined last, but a handler or a dep-driven effect fn can register one
  // (er_timer_add(…, er_timer_fn_N)) and those defs come earlier — so forward-declare every timer fn.
  const timerFnFwdDecls = out.timerFns
    .map(t => `static void ${t.name}(void);`)
    .join('\n');

  // Animated.sequence on_complete callbacks: each starts the next step when the previous finishes. Forward-
  // declared (the handler that starts step 0 references the first callback, and each callback the next).
  const animCbDecls = out.animCbs
    .map(cb => `static void ${cb.name}(bool finished, void* user_data);`)
    .join('\n');
  const animCbDefs = out.animCbs
    .map(
      cb =>
        `static void ${cb.name}(bool finished, void* user_data)\n{\n    (void)finished;\n    (void)user_data;\n${cb.body.join('\n')}\n}`,
    )
    .join('\n\n');

  // A wall-clock change is applied here, on the app loop, rather than in the setter: the host may set the
  // time before er_app_build(), when there is nothing to update yet.
  const wallClockRefresh = hasUpdate
    ? '    if (s_wall_clock_changed)\n    {\n        s_wall_clock_changed = 0;\n        app_update();\n    }\n'
    : '';
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

  // <math.h> when any libm symbol appears (Svg arc trig, or Math.* in expressions/handlers/timer callbacks).
  const usesMath =
    out.needsMath ||
    /\b(sinf|cosf|tanf|sqrtf|fabsf|roundf|floorf|ceilf|fminf|fmaxf|atan2f|powf|M_PI)\b/.test(
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
  // Host-fed values (useHostValue) → a public setter each: write the s_state field, then re-apply
  // dependent nodes via app_update() (which the host also gets for free — it is the same refresh a JS
  // setter would trigger). The host calls e.g. er_app_set_steps(count) once per frame.
  const hostRecords = scalarRecords.filter(s => s.host);
  if (hostRecords.some(s => s.name === 'wall_clock'))
    throw aotError(
      'AOT: useHostValue("wall_clock") would collide with er_app_set_wall_clock()',
      'rename the host value — every app exports er_app_set_wall_clock() for Date.now().',
    );
  const hostSettersBlock = hostRecords
    .map(
      s =>
        `void er_app_set_${s.name}(${cScalarType(s.cType)} v)\n{\n    ${s.cMember} = v;\n${hasUpdate ? '    app_update();\n' : ''}}`,
    )
    .join('\n\n');
  const hostSetterProtos = hostRecords
    .map(
      s =>
        `/** @brief Host-fed input '${s.name}' (useHostValue): set its value and refresh the display. */\nvoid er_app_set_${s.name}(${cScalarType(s.cType)} v);`,
    )
    .join('\n\n');

  // Date.now() / performance.now() read the engine clock as 64-bit whole milliseconds. The offset and its
  // setter are always emitted, so a host can set the time whether or not the app reads it; the readers and
  // the 64-bit Math helpers only when something calls them.
  const CLOCK_HELPERS = [
    [
      'app_perf_now',
      'static int64_t app_perf_now(void)\n{\n    return (int64_t)er_now_ms64();\n}',
    ],
    [
      'app_date_now',
      'static int64_t app_date_now(void)\n{\n    return s_wall_offset_ms + (int64_t)er_now_ms64();\n}',
    ],
    [
      'app_floordiv64',
      "/* Math.floor(a / b) in whole numbers: C's `/` rounds toward zero where floor rounds down. b is a\n" +
        '   nonzero constant; the compiler refuses any other divisor. */\n' +
        'static int64_t app_floordiv64(int64_t a, int64_t b)\n{\n' +
        '    const int64_t q = a / b;\n    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;\n}',
    ],
    [
      'app_abs64',
      'static int64_t app_abs64(int64_t v)\n{\n    return v < 0 ? -v : v;\n}',
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
        'static int app_delay_ms64(int64_t v)\n{\n    const uint32_t u = (uint32_t)v;\n    return u > 0x7FFFFFFFu ? 0 : (int)u;\n}',
    ],
  ];
  // The generated code that can call a file-local helper, with literals and comments blanked so a
  // `<Text>app_date_now(</Text>` is not a call. Each static helper is emitted only when this calls it, since
  // an unused static function warns under -Wall.
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
  const clockBlock = [
    '/* Date.now() is the engine clock plus this offset, so it reads as uptime until the host calls\n' +
      '   er_app_set_wall_clock(). performance.now() is the engine clock alone. */\n' +
      'static int64_t s_wall_offset_ms;' +
      (hasUpdate
        ? '\n/* Set by er_app_set_wall_clock(); the next er_app_tick() re-applies what reads the clock. */\n' +
          'static int s_wall_clock_changed;'
        : ''),
    ...CLOCK_HELPERS.filter(([name]) => helperCallers.includes(`${name}(`)).map(
      ([, def]) => def,
    ),
  ].join('\n\n');
  // A mount effect's cleanup is dropped (the app never unmounts), so an app whose only clearInterval lives
  // there calls er_timer_clear from nowhere.
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
               "embedded-react version mismatch: app.gen.c was generated by ${PKG_VERSION} but the engine header (er_version.h) is a different major.minor. Regenerate the app with 'npm run aot', or align the engine and npm versions.");
${usesMath ? '#include <math.h>\n/* M_PI is not in ISO C99 <math.h> (only POSIX/GNU); define a fallback so the generated app compiles under -std=c99 / MSVC. */\n#ifndef M_PI\n#define M_PI 3.14159265358979323846\n#endif\n' : ''}${dimRoundBlock}\n${clockBlock}\n${stateBlock ? '\n' + stateBlock : ''}${refDecls ? '\n' + refDecls + '\n' : ''}${panDeclBlock ? '\n' + panDeclBlock + '\n' : ''}${effectDeclsBlock ? '\n' + effectDeclsBlock + '\n' : ''}${vectorBlock ? '\n' + vectorBlock + '\n' : ''}${vectorBuilderBlock ? '\n' + vectorBuilderBlock + '\n' : ''}${animDecls ? '\n' + animDecls + '\n' : ''}${handleDecls ? '\n' + handleDecls + '\n' : ''}${timerTableBlock ? '\n' + timerTableBlock + '\n' : ''}${timerClearBlock ? '\n' + timerClearBlock + '\n' : ''}${effectFwdDecls ? '\n' + effectFwdDecls + '\n' : ''}${updateBlock ? '\n' + updateBlock + '\n' : ''}${timerFnFwdDecls ? '\n' + timerFnFwdDecls + '\n' : ''}${animCbDecls ? '\n' + animCbDecls + '\n' : ''}${handlerDefs ? '\n' + handlerDefs + '\n' : ''}${queryDefs ? '\n' + queryDefs + '\n' : ''}${effectFnDefs ? '\n' + effectFnDefs + '\n' : ''}${animCbDefs ? '\n' + animCbDefs + '\n' : ''}${timerFnDefs ? '\n' + timerFnDefs + '\n' : ''}${out.kbdData ? '\n' + out.kbdData + '\n' : ''}
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
    er_tree_append_child(root, ${appTop});
    er_tree_set_root(root);
${out.kbdSetup ? out.kbdSetup + ' /* app-supplied on-screen keyboard layout/appearance */\n' : ''}${hasUpdate ? '\n    app_update(); /* apply initial state-dependent props */\n' : ''}${mountEffectsBlock}}
`;

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

  // images: the baked-image imports the app actually references (name + source-relative path) — the CLI
  // resolves each path against the demo dir and bakes them into assets.generated.c (er_register_assets).
  const images = [...out.images.entries()].map(([name, importPath]) => ({
    name,
    importPath,
  }));
  return {
    c: body,
    h: header,
    nodes: out.n,
    state: stateRecords.length,
    handlers: out.handlers.length,
    updates: out.updates.length,
    images,
  };
}

/**
 * A state or ref slot is typed from its initial value, so `useState(0)` is an int until a setter stores
 * Date.now() in it. A compile reports such slots (env.found); this compiles again with them widened to
 * int64_t until none are new, so every read of a slot sees its final type.
 */
function compileWidened(src, demo, opts) {
  const wide = new Set();
  for (;;) {
    const found = new Set();
    let result;
    let error;
    try {
      result = compileSourceImpl(src, demo, opts, wide, found);
    } catch (e) {
      error = e;
    }
    const fresh = [...found].filter(key => !wide.has(key));
    // A failed pass that still had slots to widen may have failed on the narrow type, so retry it first.
    if (!fresh.length) {
      if (error) throw error;
      return result;
    }
    for (const key of fresh) wide.add(key);
  }
}

/** Public entry: compile JSX source → { c, h, ... }. On an AOT error, annotate it with file:line:col + a
 *  source code-frame (+ hint) so the failure points at the exact unsupported construct. */
export function compileSource(src, demo = 'app', opts = {}) {
  try {
    return compileWidened(src, demo, opts);
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
if (
  process.argv[1] &&
  resolve(process.argv[1]) === resolve(fileURLToPath(import.meta.url))
) {
  const demo = process.argv[2] || process.env.DEMO || 'thermostat';
  const appPath = resolve(demosDir, demo, 'App.jsx');
  const avail = existsSync(demosDir)
    ? readdirSync(demosDir, {withFileTypes: true})
        .filter(d => d.isDirectory())
        .map(d => d.name)
    : [];
  if (!existsSync(appPath)) {
    console.error(
      `AOT: demo "${demo}" not found (expected ${appPath}). Available: ${avail.join(', ') || '(none)'}`,
    );
    process.exit(1);
  }
  const src = readFileSync(appPath, 'utf8');
  let result;
  try {
    // Bake any <Svg source> .svg imports to vector artifacts first (I/O), then compile (pure) with them in hand.
    const svgArtifacts = await bakeSvgArtifacts(src, resolve(demosDir, demo));
    result = compileSource(src, demo, {
      filename: resolve(demosDir, demo, 'App.jsx'),
      svgArtifacts,
    });
  } catch (e) {
    // A located AOT error already reads as "<reason>\n  at file:line:col\n\n<frame>\n\nhint: ..."; print it
    // cleanly (no JS stack) so the developer sees exactly the unsupported construct.
    console.error(e && e.aotLoc ? e.message : e?.message || String(e));
    process.exit(1);
  }
  mkdirSync(distDir, {recursive: true});
  writeFileSync(resolve(distDir, 'app.gen.c'), result.c);
  writeFileSync(resolve(distDir, 'app.gen.h'), result.h);

  // Bake the images the app imports into dist/assets.generated.{c,h} (er_register_assets) — the SAME baker
  // Flow A uses. Always written (even with 0 images → a no-op register fn) so the AOT host can always
  // compile + call it. Each importPath is source-relative to the demo's App.jsx.
  const imageJobs = result.images.map(im => ({
    name: im.name,
    path: resolve(demosDir, demo, im.importPath),
  }));
  for (const j of imageJobs) {
    if (!existsSync(j.path)) {
      console.error(`AOT: <Image> asset "${j.name}" not found at ${j.path}`);
      process.exit(1);
    }
  }
  // Flow B has no font imports — every string renders in the engine's built-in font, so check the
  // app's text and its font sizes against that coverage.
  warnMissingGlyphs({source: src, jsx: true});
  warnFontSizes({used: analyzeFontSizes(src)});
  const baked = bakeAssets({images: imageJobs, fonts: [], outDir: distDir});
  console.log(
    `AOT: compiled demo "${demo}" -> dist/app.gen.c (${result.nodes} nodes, ${result.state} state, ` +
      `${result.handlers} handler(s), ${result.updates} dynamic) + ${baked.images} image(s) -> dist/assets.generated.c`,
  );
}
