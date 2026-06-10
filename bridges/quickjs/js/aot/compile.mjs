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

const here = dirname(fileURLToPath(import.meta.url)); // bridges/quickjs/js/aot
const repoRoot = resolve(here, '../../../..');
const demosDir = resolve(repoRoot, 'demos');
const distDir = resolve(here, '..', 'dist');

const demo = process.argv[2] || process.env.DEMO || 'thermostat';
const appPath = resolve(demosDir, demo, 'App.jsx');
if (!existsSync(appPath)) {
  const avail = existsSync(demosDir) ? readdirSync(demosDir, { withFileTypes: true }).filter((d) => d.isDirectory()).map((d) => d.name) : [];
  console.error(`AOT: demo "${demo}" not found (expected ${appPath}). Available: ${avail.join(', ') || '(none)'}`);
  process.exit(1);
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
      const initVal = init.arguments[0] ? evalStatic(init.arguments[0], scope) : 0;
      const cType = cTypeOfValue(initVal);
      if (cType === 'string') throw new Error(`AOT: string useState ("${name}") not yet supported`);
      const rec = { name, setter, cType, cMember: `s_state.${name}`, initCode: cType === 'float' ? `${initVal}f` : String(Number(initVal)) };
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

/** True if a function component declares any useState (per-instance child state — not yet supported). */
function usesState(fn) {
  if (fn.body.type !== 'BlockStatement') return false;
  return fn.body.body.some((s) => s.type === 'VariableDeclaration' && s.declarations.some((d) => d.init?.type === 'CallExpression' && d.init.callee.name === 'useState'));
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
  const apply = (expr) => {
    if (expr.type === 'ArrayExpression') {
      for (const e of expr.elements) if (e) apply(e);
      return;
    }
    if (expr.type === 'ObjectExpression') {
      for (const prop of expr.properties) {
        if (prop.type !== 'ObjectProperty') throw new Error('AOT: spread/method in an inline style object not supported');
        const key = prop.computed ? evalStatic(prop.key, scope) : prop.key.name ?? prop.key.value;
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
  return { staticAssigns, dynAssigns };
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
function compileHandler(fnNode, env, state) {
  const stmts = [];
  const body = fnNode.body;
  const list = body.type === 'BlockStatement' ? body.body : [{ type: 'ExpressionStatement', expression: body }];
  for (const st of list) {
    if (st.type !== 'ExpressionStatement') throw new Error(`AOT: unsupported statement "${st.type}" in event handler`);
    const expr = st.expression;
    if (expr.type !== 'CallExpression' || expr.callee.type !== 'Identifier') throw new Error('AOT: event handler may only call a state setter (for now)');
    const rec = state.bySetter.get(expr.callee.name);
    if (!rec) throw new Error(`AOT: "${expr.callee.name}" is not a known state setter`);
    const arg = expr.arguments[0];
    if (arg && (arg.type === 'ArrowFunctionExpression' || arg.type === 'FunctionExpression')) {
      // setState(prev => expr): bind the param to the current value, assign the result.
      const param = arg.params[0]?.name;
      const locals = new Map(env.locals);
      if (param) locals.set(param, { code: rec.cMember, cType: rec.cType });
      if (arg.body.type === 'BlockStatement') throw new Error('AOT: updater function must be a single expression (for now)');
      const e = emitExpr(arg.body, { ...env, locals });
      stmts.push(`    ${rec.cMember} = ${e.code};`);
    } else {
      const e = emitExpr(arg, env);
      stmts.push(`    ${rec.cMember} = ${e.code};`);
    }
  }
  stmts.push('    app_update();');
  return stmts;
}

// ---------------------------------------------------------------------------------------------------
// Emit — control flow (components / conditionals / lists) all unroll at COMPILE TIME into fixed nodes.
// Runtime-dynamic conditionals/lists (where the node COUNT changes with state) are not yet supported
// and throw a clear "AOT: ..." — see /PLAN.md Flow B.
// ---------------------------------------------------------------------------------------------------

/** Reads a component instance's props (attributes) as static values; dynamic props throw (for now). */
function extractProps(openingElement, scope) {
  const props = {};
  for (const attr of openingElement.attributes) {
    if (attr.type === 'JSXSpreadAttribute') throw new Error('AOT: spread props {...x} to a component not yet supported');
    if (attr.type !== 'JSXAttribute' || attr.name.name === 'key') continue;
    try {
      props[attr.name.name] = evalStatic(attrExpr(attr), scope);
    } catch {
      throw new Error(`AOT: dynamic prop "${attr.name.name}" to a component not yet supported (static props only for now)`);
    }
  }
  return props;
}

/** Binds a component's parameter (destructured `{a,b}` or whole `props`) to the passed prop values. */
function bindParams(fn, props) {
  const out = {};
  const param = fn.params[0];
  if (!param) return out;
  if (param.type === 'Identifier') {
    out[param.name] = props;
  } else if (param.type === 'ObjectPattern') {
    for (const p of param.properties) {
      if (p.type === 'RestElement') throw new Error('AOT: rest props (...rest) in a component param not supported');
      const key = p.key.name ?? p.key.value;
      let val = props[key];
      if (val === undefined && p.value?.type === 'AssignmentPattern') val = evalStatic(p.value.right, {});
      out[key] = val;
    }
  } else {
    throw new Error('AOT: unsupported component parameter pattern');
  }
  return out;
}

/** Inlines a function component instance: bind props into scope, emit its returned JSX in place. */
function emitComponent(el, scope, out, env, state, opts) {
  const tag = el.openingElement.name.name;
  const fn = out.components.get(tag);
  if (usesState(fn)) throw new Error(`AOT: component <${tag}> uses useState — per-instance child state not yet supported`);
  if (el.children.some((c) => c.type === 'JSXElement')) throw new Error(`AOT: passing children to <${tag}> (props.children) not yet supported`);
  const bindings = bindParams(fn, extractProps(el.openingElement, scope));
  const childScope = { ...scope, ...bindings };
  return emitNode(componentReturnJSX(fn), childScope, out, { ...env, consts: childScope }, state, opts);
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
        emitMap(expr, parentVar, scope, out, env, state);
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

function emitNode(el, scope, out, env, state, opts = {}) {
  const tag = el.openingElement.name.name;
  const nodeType = NODE_TYPES[tag];
  if (!nodeType) {
    if (out.components.has(tag)) return emitComponent(el, scope, out, env, state, opts);
    throw new Error(`AOT: unknown element <${tag}> (not a built-in or a component in this file)`);
  }

  const v = `n${out.n++}`;
  const { staticAssigns, dynAssigns } = collectStyleAssigns(el.openingElement, scope, env);
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

  for (const attr of el.openingElement.attributes) {
    if (attr.type !== 'JSXAttribute') continue;
    const evt = EVENT_TYPES[attr.name.name];
    if (!evt) continue;
    const fn = attrExpr(attr);
    if (!isFn(fn)) throw new Error(`AOT: ${attr.name.name} must be an inline function`);
    const hid = out.handlers.length;
    out.handlers.push({ name: `er_handler_${hid}`, body: compileHandler(fn, env, state) });
    out.build.push(`    er_event_set(${v}, ${evt}, er_handler_${hid}, NULL);`);
  }

  if (tag !== 'Text') emitChildren(el.children, v, scope, out, env, state);
  return v;
}

// ---------------------------------------------------------------------------------------------------
// Compile
// ---------------------------------------------------------------------------------------------------
const src = readFileSync(appPath, 'utf8');
const ast = parse(src, { sourceType: 'module', plugins: ['jsx'] });

const scope = moduleScope(ast.program);
const component = findComponent(ast.program);
const state = collectState(component.body, scope);
const rootJSX = findReturnJSX(component.body);

const env = { state: state.byName, locals: new Map(), consts: scope };
const out = { n: 0, build: [], handlers: [], updates: [], handles: [], components: collectComponents(ast.program) };
const appTop = emitNode(rootJSX, scope, out, env, state);

const nodeDecls = Array.from({ length: out.n }, (_, i) => `n${i}`);
const stateRecords = [...state.byName.values()];

const stateBlock = stateRecords.length
  ? `typedef struct\n{\n${stateRecords.map((s) => `    ${s.cType === 'float' ? 'float' : 'int'} ${s.name};`).join('\n')}\n} ErAppState;\n\nstatic ErAppState s_state = {${stateRecords.map((s) => ` .${s.name} = ${s.initCode}`).join(',')} };\n`
  : '';

const handleDecls = out.handles.map((v) => `static ERNode* s_${v};`).join('\n');

const updateBlock = (() => {
  if (!out.updates.length) return '';
  const lines = ['static void app_update(void)', '{', '    ERProps p;'];
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
  lines.push('}');
  return lines.join('\n');
})();

const handlerDefs = out.handlers
  .map((h) => `static void ${h.name}(ERNode* node, const EREventData* data, void* user_data)\n{\n    (void)node;\n    (void)data;\n    (void)user_data;\n${h.body.join('\n')}\n}`)
  .join('\n\n');

const hasUpdate = out.updates.length > 0;
const body = `/*
 * Generated by the embedded-react Flow B AOT compiler (npm run aot -- ${demo}). DO NOT EDIT.
 * Builds the app's scene graph + state machine directly against er_scene.h — no QuickJS, no JS runtime.
 */
#include "app.gen.h"

#include "er_scene.h"

#include <stdio.h>
${stateBlock ? '\n' + stateBlock : ''}${handleDecls ? '\n' + handleDecls + '\n' : ''}${updateBlock ? '\n' + updateBlock + '\n' : ''}${handlerDefs ? '\n' + handlerDefs + '\n' : ''}
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

mkdirSync(distDir, { recursive: true });
writeFileSync(resolve(distDir, 'app.gen.c'), body);
writeFileSync(resolve(distDir, 'app.gen.h'), header);
console.log(`AOT: compiled demo "${demo}" -> dist/app.gen.c (${out.n} nodes, ${stateRecords.length} state, ${out.handlers.length} handler(s), ${out.updates.length} dynamic)`);
