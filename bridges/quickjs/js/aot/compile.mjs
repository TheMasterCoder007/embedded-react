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
import { readFileSync, writeFileSync, mkdirSync, existsSync, readdirSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { lowerStyle, NODE_TYPES, DYN_FIELDS, colorLiteral } from './style-map.mjs';
import { flattenSvg, parseColor, parsePath } from '../src/embedded-react/svg-ops.js';

const here = dirname(fileURLToPath(import.meta.url)); // bridges/quickjs/js/aot
const repoRoot = resolve(here, '../../../..');
const demosDir = resolve(repoRoot, 'demos');
const distDir = resolve(here, '..', 'dist');

// The core compiler is exported as compileSource(src) so it can be unit-tested on inline JSX. The CLI
// (read a demo's App.jsx, write dist/app.gen.{c,h}) lives in the entry guard at the bottom of this file.

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

function emitExpr(node, env) {
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
      if (node.operator === '-' || node.operator === '+' || node.operator === '!') return { code: `(${node.operator}${a.code})`, cType: node.operator === '!' ? 'int' : a.cType };
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
      throw new Error('AOT: unsupported member expression in a dynamic context');
    }
  }
  throw new Error(`AOT: unsupported expression "${node.type}" in a dynamic context`);
}

const printfSpec = (cType) => (cType === 'string' ? '%s' : cType === 'float' ? '%g' : '%d');
const cTypeOfValue = (v) => (typeof v === 'string' ? 'string' : typeof v === 'number' && !Number.isInteger(v) ? 'float' : 'int');

// ---------------------------------------------------------------------------------------------------
// AST helpers
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

function moduleScope(program) {
  const scope = {};
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

/** Infers a C struct shape from a list-state's initial elements (objects of strings/numbers). */
function inferItemStruct(items, name) {
  if (!Array.isArray(items) || !items.length) throw new Error(`AOT: list state "${name}" needs ≥1 initial element to infer its item shape`);
  const first = items[0];
  if (typeof first !== 'object' || first === null || Array.isArray(first)) throw new Error(`AOT: list state "${name}" elements must be objects`);
  const fields = Object.keys(first).map((key) => {
    const v = first[key];
    if (typeof v === 'string') return { key, kind: 'string' };
    if (typeof v === 'number') return { key, kind: Number.isInteger(v) ? 'int' : 'float' };
    throw new Error(`AOT: list state "${name}" field "${key}" must be a string or number`);
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
        const items = evalStatic(initArg, scope);
        rec = { name, setter, kind: 'list', struct: inferItemStruct(items, name), items, cap: LIST_CAP, cTypeName: `ErItem_${name}`, arrayName: `s_${name}`, countMember: `s_${name}_count` };
      } else {
        const initVal = initArg ? evalStatic(initArg, scope) : 0;
        const cType = cTypeOfValue(initVal);
        // String scalar → a fixed char buffer in ErAppState; setters snprintf into it (see scalarAssign).
        const initCode = cType === 'string' ? cstr(String(initVal)) : cType === 'float' ? `${initVal}f` : String(Number(initVal));
        rec = { name, setter, kind: 'scalar', cType, cMember: `s_state.${name}`, initCode };
      }
      byName.set(name, rec);
      if (setter) bySetter.set(setter, rec);
    }
  }
  return { byName, bySetter };
}

function findReturnJSX(fnBody) {
  for (const stmt of fnBody.body) {
    if (stmt.type === 'ReturnStatement' && stmt.argument) {
      if (stmt.argument.type === 'JSXElement') return stmt.argument;
      throw new Error(`AOT: the component must return a single JSX element (got ${stmt.argument.type})`);
    }
  }
  throw new Error('AOT: component has no return statement');
}

/** Returns the JSX a function component returns (arrow expression body or a block's return). */
function componentReturnJSX(fn) {
  if (fn.body.type === 'JSXElement') return fn.body;
  if (fn.body.type === 'BlockStatement') return findReturnJSX(fn.body);
  throw new Error('AOT: component body must return a JSX element');
}

const fnReturnsJSX = (fn) => fn.body.type === 'JSXElement' || (fn.body.type === 'BlockStatement' && fn.body.body.some((s) => s.type === 'ReturnStatement' && s.argument?.type === 'JSXElement'));

/** Collects top-level function components (name → fn node), excluding the `App` entry component. */
function collectComponents(program) {
  const comps = new Map();
  for (const stmt of program.body) {
    const d = stmt.type === 'ExportNamedDeclaration' ? stmt.declaration : stmt;
    if (!d) continue;
    if (d.type === 'FunctionDeclaration' && d.id && d.id.name !== 'App' && fnReturnsJSX(d)) comps.set(d.id.name, d);
    if (d.type === 'VariableDeclaration')
      for (const decl of d.declarations) if (decl.id?.type === 'Identifier' && decl.id.name !== 'App' && isFn(decl.init) && fnReturnsJSX(decl.init)) comps.set(decl.id.name, decl.init);
  }
  return comps;
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
 * Collects `const r = useRef(initial)` VALUE refs → Map(name → {cVar, cType, initCode}). A value ref is
 * a mutable C slot (like state but it does NOT trigger a re-render — `.current` reads/writes only). Node
 * refs (`useRef()` with no initial, used as `ref={r}` for imperative calls) are deferred to the Vector
 * phase and throw a clear error here.
 */
function collectRefs(fnBody, scope) {
  const refs = new Map();
  if (fnBody.type !== 'BlockStatement') return refs;
  for (const stmt of fnBody.body) {
    if (stmt.type !== 'VariableDeclaration') continue;
    for (const decl of stmt.declarations) {
      const init = decl.init;
      if (init?.type === 'CallExpression' && init.callee.name === 'useRef' && decl.id.type === 'Identifier') {
        if (!init.arguments[0]) throw new Error(`AOT: useRef() with no initial value (node/imperative refs) not yet supported — give "${decl.id.name}" a numeric initial`);
        const v = evalStatic(init.arguments[0], scope);
        if (typeof v !== 'number') throw new Error(`AOT: useRef initial for "${decl.id.name}" must be a number (value refs only, for now)`);
        const cType = Number.isInteger(v) ? 'int' : 'float';
        refs.set(decl.id.name, { cVar: `s_ref_${decl.id.name}`, cType, initCode: cType === 'float' ? `${v}f` : String(v) });
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

/** Maps `Easing.<x>` to an ER_EASE_* constant (a flat subset; defaults to ease-in-out). */
function mapEasing(node) {
  if (node?.type === 'MemberExpression' && node.object.name === 'Easing') {
    const m = { linear: 'ER_EASE_LINEAR', ease: 'ER_EASE_EASE', quad: 'ER_EASE_QUAD_IN_OUT', cubic: 'ER_EASE_CUBIC_IN_OUT', bounce: 'ER_EASE_BOUNCE_OUT', elastic: 'ER_EASE_ELASTIC_OUT' };
    return m[node.property.name] || 'ER_EASE_EASE_IN_OUT';
  }
  return 'ER_EASE_EASE_IN_OUT';
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
  if (node.type === 'Identifier' && node.name in env.consts && typeof env.consts[node.name] === 'string') {
    return colorLiteral(env.consts[node.name]);
  }
  throw new Error('AOT: a dynamic color must be a color string literal or a ternary of them');
}

/** Lowers one dynamic inline-style value to ERProps field assignment(s) (C expressions). */
function lowerDynamicStyleValue(key, valueNode, env) {
  const meta = DYN_FIELDS[key];
  if (!meta) throw new Error(`AOT: a state-driven value for style "${key}" is not supported (static only)`);
  if (meta.kind === 'color') return [{ field: meta.field, code: emitColorExpr(valueNode, env) }];
  if (meta.kind === 'opacity') return [{ field: meta.field, code: `(uint8_t)((${emitExpr(valueNode, env).code}) * 255.0f)` }];
  return [{ field: meta.field, code: `(int16_t)(${emitExpr(valueNode, env).code})` }]; /* num */
}

/**
 * Collects an element's merged style into static field assigns and dynamic (state-driven) field assigns.
 * Inline object values are tried statically first; a value that references state becomes a dynAssign.
 * Later style sources override earlier ones per field (RN merge), kept in `fields` by ERProps field.
 */
function collectStyleAssigns(openingElement, scope, env) {
  const fields = new Map(); // ERProps field -> { dynamic: bool, code: string }
  const binds = []; // [{ cVar, prop }] — animated values bound to node properties (native driver)
  const animRef = (node) => (node?.type === 'Identifier' && env.anims?.has(node.name) ? env.anims.get(node.name).cVar : null);
  const apply = (expr) => {
    if (expr.type === 'ArrayExpression') {
      for (const e of expr.elements) if (e) apply(e);
      return;
    }
    if (expr.type === 'ObjectExpression') {
      for (const prop of expr.properties) {
        if (prop.type !== 'ObjectProperty') throw new Error('AOT: spread/method in an inline style object not supported');
        const key = prop.computed ? evalStatic(prop.key, scope) : prop.key.name ?? prop.key.value;

        // Animated value bound directly to a prop (opacity / backgroundColor / color).
        const av = animRef(prop.value);
        if (av && ANIM_STYLE_PROPS[key]) {
          for (const p of ANIM_STYLE_PROPS[key]) binds.push({ cVar: av, prop: p });
          continue;
        }
        // transform: [{ scale: <anim> }, { translateX: <anim> }, ...] — bind each animated entry.
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

const EVENT_TYPES = { onPress: 'ER_EVENT_PRESS', onLongPress: 'ER_EVENT_LONG_PRESS', onPressIn: 'ER_EVENT_PRESS_IN', onPressOut: 'ER_EVENT_PRESS_OUT' };

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

/** Compiles `Animated.timing|spring(value, cfg).start()` to a scoped ERAnimConfig + er_anim_value_animate. */
function compileAnimateStart(expr, env, idx) {
  const animCall = expr.callee.object; // Animated.timing(value, cfg)
  if (animCall?.type !== 'CallExpression' || animCall.callee.type !== 'MemberExpression' || animCall.callee.object.name !== 'Animated') throw new Error('AOT: .start() must be called on Animated.timing/spring(value, config)');
  const kind = animCall.callee.property.name; // 'timing' | 'spring' | 'decay'
  const valRef = animCall.arguments[0];
  if (valRef?.type !== 'Identifier' || !env.anims?.has(valRef.name)) throw new Error(`AOT: Animated.${kind}() first argument must be a useAnimatedValue`);
  const cVar = env.anims.get(valRef.name).cVar;
  const cfgObj = animCall.arguments[1];
  const get = (k) => cfgObj?.properties?.find((p) => (p.key.name ?? p.key.value) === k)?.value;
  const toNode = get('toValue');
  if (!toNode) throw new Error(`AOT: Animated.${kind}() config needs a toValue`);
  const toCode = emitExpr(toNode, env).code;
  const c = `cfg${idx}`;
  const lines = ['    {', `        ERAnimConfig ${c};`, `        memset(&${c}, 0, sizeof(${c}));`];
  if (kind === 'spring') {
    lines.push(`        ${c}.type = ER_ANIM_SPRING;`);
    lines.push(`        ${c}.stiffness = ${floatLit(evalStaticOr(get('stiffness'), env, 200))};`);
    lines.push(`        ${c}.damping = ${floatLit(evalStaticOr(get('damping'), env, 18))};`);
    lines.push(`        ${c}.mass = ${floatLit(evalStaticOr(get('mass'), env, 1))};`);
  } else {
    lines.push(`        ${c}.type = ER_ANIM_TIMING;`);
    lines.push(`        ${c}.duration_ms = ${Math.round(Number(evalStaticOr(get('duration'), env, 250)))};`);
    lines.push(`        ${c}.easing = ${mapEasing(get('easing'))};`);
  }
  lines.push(`        er_anim_value_animate(${cVar}, (float)(${toCode}), &${c});`, '    }');
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

/** Compiles one handler ExpressionStatement: a state setter, ref mutation, or Animated.*(...).start(). */
function compileHandlerExpr(expr, env, state, ctx, indent) {
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
  // Animated.timing/spring(value, cfg).start() — native-driven; sets no React state, needs no app_update.
  if (expr.type === 'CallExpression' && expr.callee.type === 'MemberExpression' && expr.callee.property.name === 'start') {
    return compileAnimateStart(expr, env, ctx.animIdx++);
  }
  if (expr.type !== 'CallExpression' || expr.callee.type !== 'Identifier') throw new Error('AOT: a handler statement must be a state setter, a ref write, or Animated.timing/spring(...).start()');
  const rec = state.bySetter.get(expr.callee.name);
  if (!rec) throw new Error(`AOT: "${expr.callee.name}" is not a known state setter`);
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
        const e = emitExpr(decl.init, env);
        const cName = `l_${decl.id.name}`;
        lines.push(`${indent}${e.cType === 'float' ? 'float' : 'int'} ${cName} = ${e.code};`);
        env = { ...env, locals: new Map(env.locals).set(decl.id.name, { code: cName, cType: e.cType }) };
      }
      continue;
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
    if (st.type !== 'ExpressionStatement') throw new Error(`AOT: unsupported statement "${st.type}" in event handler`);
    lines.push(...compileHandlerExpr(st.expression, env, state, ctx, indent));
  }
  return lines;
}

function compileHandler(fnNode, env, state) {
  const body = fnNode.body;
  const list = body.type === 'BlockStatement' ? body.body : [{ type: 'ExpressionStatement', expression: body }];
  const ctx = { stateChanged: false, animIdx: 0 };
  const stmts = compileStmts(list, env, state, ctx, '    ');
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
    if (attr.type === 'JSXSpreadAttribute') throw new Error('AOT: spread props {...x} to a component not yet supported');
    if (attr.type !== 'JSXAttribute' || attr.name.name === 'key') continue;
    const node = attrExpr(attr);
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

/** Inlines a function component instance: bind props (static → scope, dynamic → locals), emit its JSX. */
function emitComponent(el, scope, out, env, state, opts) {
  const tag = el.openingElement.name.name;
  const fn = out.components.get(tag);
  if (usesState(fn)) throw new Error(`AOT: component <${tag}> uses useState — per-instance child state not yet supported`);
  if (el.children.some((c) => c.type === 'JSXElement')) throw new Error(`AOT: passing children to <${tag}> (props.children) not yet supported`);
  const childScope = { ...scope };
  const childLocals = new Map(env.locals);
  for (const [name, d] of bindParams(fn, extractProps(el.openingElement, scope, env))) {
    if (d.static) childScope[name] = d.value;
    else childLocals.set(name, { code: d.code, cType: d.cType, struct: d.struct });
  }
  return emitNode(componentReturnJSX(fn), childScope, out, { ...env, consts: childScope, locals: childLocals }, state, opts);
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
          throw new Error(`AOT: unsupported expression child "${expr.type}" in a container`);
        }
        if (v !== false && v != null && v !== '') throw new Error(`AOT: a non-element expression child (${JSON.stringify(v)}) cannot render here`);
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
    const vn = attr.value;
    if (vn == null) out[name] = true;
    else if (vn.type === 'StringLiteral') out[name] = vn.value;
    else if (vn.type === 'JSXExpressionContainer') {
      try {
        out[name] = evalStatic(vn.expression, scope);
      } catch {
        out[name] = { dyn: emitExpr(vn.expression, env).code };
      }
    } else throw new Error(`AOT: unsupported SVG attribute value for "${name}"`);
  }
  return out;
}

const CAP_MAP = { butt: 0, round: 1, square: 2 };
const JOIN_MAP = { miter: 0, round: 1, bevel: 2 };

/** A shape's static 7-number paint record [fill,stroke,w,miter,cap,join,rule]; dynamic paint attrs throw. */
function svgPaintRecord(a) {
  for (const k of ['fill', 'stroke', 'strokeWidth', 'strokeLinecap', 'strokeLinejoin', 'strokeMiterlimit', 'fillRule'])
    if (isDyn(a[k])) throw new Error('AOT: a state-driven <Svg> paint attribute is not yet supported (geometry can be dynamic, paint must be static)');
  return [parseColor(a.fill ?? 'black'), parseColor(a.stroke ?? 'none'), svgNum(a.strokeWidth, 1), svgNum(a.strokeMiterlimit, 4), CAP_MAP[a.strokeLinecap] ?? 0, JOIN_MAP[a.strokeLinejoin] ?? 0, a.fillRule === 'evenodd' ? 1 : 0];
}

// Per-shape op-tape entries (C-expr strings; opcodes as ER_VOP_* macros). Mirror svg-ops circleOps/etc.
const arcEntries = (a) => {
  const cx = cf(a.cx),
    cy = cf(a.cy),
    r = cf(a.r);
  const a0 = `((${cf(a.startAngle)} - 90.0f) * (float)M_PI / 180.0f)`;
  const a1 = `((${cf(a.endAngle)} - 90.0f) * (float)M_PI / 180.0f)`;
  return ['ER_VOP_MOVE', `(${cx} + ${r} * cosf(${a0}))`, `(${cy} + ${r} * sinf(${a0}))`, 'ER_VOP_ARC', cx, cy, r, a0, a1, '0.0f'];
};
const circleEntries = (a) => {
  const cx = cf(a.cx),
    cy = cf(a.cy),
    r = cf(a.r);
  return ['ER_VOP_MOVE', `(${cx} + ${r})`, cy, 'ER_VOP_ARC', cx, cy, r, '0.0f', '(2.0f * (float)M_PI)', '0.0f', 'ER_VOP_CLOSE'];
};
const rectEntries = (a) => {
  const x = cf(a.x),
    y = cf(a.y),
    w = cf(a.width),
    h = cf(a.height);
  return ['ER_VOP_MOVE', x, y, 'ER_VOP_LINE', `(${x} + ${w})`, y, 'ER_VOP_LINE', `(${x} + ${w})`, `(${y} + ${h})`, 'ER_VOP_LINE', x, `(${y} + ${h})`, 'ER_VOP_CLOSE'];
};
const lineEntries = (a) => ['ER_VOP_MOVE', cf(a.x1), cf(a.y1), 'ER_VOP_LINE', cf(a.x2), cf(a.y2)];
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
      if (attr.type === 'JSXAttribute' && attr.value?.type === 'JSXExpressionContainer') {
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
  const paints = [];
  for (const c of el.children) {
    if (c.type !== 'JSXElement') continue;
    const type = c.openingElement.name.name;
    const fn = SHAPE_ENTRIES[type];
    if (!fn) throw new Error(`AOT: <${type}> is not a supported shape in a state-driven <Svg> (no <G>/viewBox yet)`);
    const a = svgAttrs(c.openingElement, scope, env);
    const shape = fn(a);
    if (!shape.length) continue;
    entries.push('ER_VOP_SHAPE', floatLit(paints.length), ...shape);
    paints.push(svgPaintRecord(a));
  }
  const v = `n${out.n++}`;
  const id = out.svgN++;
  const len = entries.length;
  const nPaints = paints.length;
  out.vectorData.push(`static float s_svg${id}_ops[${len}];`);
  out.vectorData.push(`static const ERVectorPaint s_svg${id}_paints[] = {\n${paints.map((p) => '    ' + emitVectorPaint(p)).join(',\n')}\n};`);
  out.vectorBuilders.push(`static void build_svg${id}(void)\n{\n${entries.map((e, i) => `    s_svg${id}_ops[${i}] = ${e};`).join('\n')}\n}`);

  emitSvgBox(v, svgA.width, svgA.height, el.openingElement, scope, out, env);
  out.build.push(`    build_svg${id}();`, `    er_node_set_vector_ops(${v}, s_svg${id}_ops, ${len}, s_svg${id}_paints, ${nPaints});`, `    s_${v} = ${v};`);
  out.handles.push(v);
  out.svgUpdates.push({ id, len, nPaints, nodeVar: `s_${v}` });
  return v;
}

function emitNode(el, scope, out, env, state, opts = {}) {
  const tag = resolveTag(el.openingElement);
  if (tag === 'Svg') return emitSvg(el, scope, out, env, state, opts);
  const nodeType = NODE_TYPES[tag];
  if (!nodeType) {
    if (out.components.has(tag)) return emitComponent(el, scope, out, env, state, opts);
    throw new Error(`AOT: unknown element <${tag}> (not a built-in or a component in this file)`);
  }

  const v = `n${out.n++}`;
  const { staticAssigns, dynAssigns, binds } = collectStyleAssigns(el.openingElement, scope, env);
  const text = tag === 'Text' ? buildText(el.children, scope, env) : null;

  // `displayCode` toggles show/hide for a state-driven conditional: the node is always built, its
  // `display` flips between flex and none in app_update (joining any state-driven style assigns).
  if (opts.displayCode) dynAssigns.push({ field: 'display', code: `((${opts.displayCode}) ? ER_DISPLAY_FLEX : ER_DISPLAY_NONE)` });

  const isDynamic = !!text?.dynamic || dynAssigns.length > 0;

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
    out.build.push(`    er_node_set_props(${v}, &p);`);
  }

  // Animated style props (opacity / transform / color) → bind the node to its animated value. The
  // engine's native driver advances it each tick (no per-frame JS, no app_update for the motion).
  for (const b of binds) out.build.push(`    er_anim_value_bind(${b.cVar}, ${v}, ${b.prop});`);

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
        out.handlers.push({ name: handlerName, body: compileHandler(env.callbacks.get(fn.name), env, state) });
      }
    } else if (isFn(fn)) {
      handlerName = `er_handler_${out.handlers.length}`;
      out.handlers.push({ name: handlerName, body: compileHandler(fn, env, state) });
    } else {
      throw new Error(`AOT: ${attr.name.name} must be an inline function or a useCallback`);
    }
    out.build.push(`    er_event_set(${v}, ${evt}, ${handlerName}, NULL);`);
  }

  if (tag !== 'Text') emitChildren(el.children, v, scope, out, env, state);
  return v;
}

// ---------------------------------------------------------------------------------------------------
// Compile — JSX source string → generated C. Pure (no I/O) so it can be unit-tested directly.
// ---------------------------------------------------------------------------------------------------
/**
 * Compiles a Flow B app's JSX source to C.
 * @param {string} src   The App.jsx source text.
 * @param {string} demo  Demo name (only used in the generated-by header comment).
 * @returns {{c: string, h: string, nodes: number, state: number, handlers: number, updates: number}}
 */
export function compileSource(src, demo = 'app') {
const ast = parse(src, { sourceType: 'module', plugins: ['jsx'] });

const scope = moduleScope(ast.program);
const component = findComponent(ast.program);
const state = collectState(component.body, scope);
const rootJSX = findReturnJSX(component.body);

const anims = collectAnims(component.body, scope);
const refs = collectRefs(component.body, scope);
const callbacks = collectCallbacks(component.body);
const memos = collectMemos(component.body);
const env = { state: state.byName, locals: new Map(), consts: scope, anims, refs, callbacks };
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
const out = { n: 0, build: [], handlers: [], updates: [], handles: [], components: collectComponents(ast.program), cbEmitted: new Map(), vectorData: [], vectorBuilders: [], svgUpdates: [], svgN: 0 };
const appTop = emitNode(rootJSX, scope, out, env, state);

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
const body = `/*
 * Generated by the embedded-react Flow B AOT compiler (npm run aot -- ${demo}). DO NOT EDIT.
 * Builds the app's scene graph + state machine directly against er_scene.h — no QuickJS, no JS runtime.
 */
#include "app.gen.h"

#include "er_scene.h"

#include <stdio.h>
#include <string.h>
${out.vectorBuilders.length ? '#include <math.h>\n' : ''}${stateBlock ? '\n' + stateBlock : ''}${refDecls ? '\n' + refDecls + '\n' : ''}${vectorBlock ? '\n' + vectorBlock + '\n' : ''}${vectorBuilderBlock ? '\n' + vectorBuilderBlock + '\n' : ''}${animDecls ? '\n' + animDecls + '\n' : ''}${handleDecls ? '\n' + handleDecls + '\n' : ''}${updateBlock ? '\n' + updateBlock + '\n' : ''}${handlerDefs ? '\n' + handlerDefs + '\n' : ''}
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
${hasUpdate ? '\n    app_update(); /* apply initial state-dependent props */\n' : ''}}
`;

const header = `/* Generated by the embedded-react Flow B AOT compiler. DO NOT EDIT. */
#ifndef ER_APP_GEN_H
#define ER_APP_GEN_H

/** @brief Builds the AOT-compiled app's scene graph + state machine (call once after backend init). */
void er_app_build(int screen_w, int screen_h);

#endif
`;

  return { c: body, h: header, nodes: out.n, state: stateRecords.length, handlers: out.handlers.length, updates: out.updates.length };
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
  const result = compileSource(readFileSync(appPath, 'utf8'), demo);
  mkdirSync(distDir, { recursive: true });
  writeFileSync(resolve(distDir, 'app.gen.c'), result.c);
  writeFileSync(resolve(distDir, 'app.gen.h'), result.h);
  console.log(`AOT: compiled demo "${demo}" -> dist/app.gen.c (${result.nodes} nodes, ${result.state} state, ${result.handlers} handler(s), ${result.updates} dynamic)`);
}
