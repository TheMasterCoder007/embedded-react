// `npm run aot [demo]` — the Flow B ahead-of-time compiler (vertical slice).
//
// Compiles a demo's JSX straight to C against er_scene.h: no QuickJS, no JS at runtime. The generated
// app.gen.c builds the engine node tree directly, so it fits an MCU with only internal RAM. This is
// the static-tree slice — it lowers the component's returned JSX (View/Text/Pressable, StyleSheet +
// inline styles, literal/interpolated text) to er_node_create / er_node_set_props / er_tree_* calls.
// State + events come next; today useState initial values render as constants.
//
//   npm run aot                      # default demo (thermostat) — but use a minimal demo for the slice
//   npm run aot -- music-player      # a specific demo by folder name
//
// Unsupported syntax throws with a clear "AOT: ..." message rather than emitting wrong code — the
// supported subset grows demo by demo (see /PLAN.md Flow B).
import { parse } from '@babel/parser';
import { readFileSync, writeFileSync, mkdirSync, existsSync, readdirSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { lowerStyle, NODE_TYPES } from './style-map.mjs';

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
// Static expression evaluation — resolves the constant subset (literals, objects/arrays, identifiers
// in scope, member access, StyleSheet.create). Anything dynamic throws; the slice only needs constants.
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
      // The only call we statically fold is StyleSheet.create(obj) — an identity pass-through.
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
// AST helpers
// ---------------------------------------------------------------------------------------------------
const isFn = (n) => n && (n.type === 'FunctionDeclaration' || n.type === 'FunctionExpression' || n.type === 'ArrowFunctionExpression');

/** Finds the app component: a function named `App` (declaration or const = arrow/function). */
function findComponent(program) {
  for (const stmt of program.body) {
    const d = stmt.type === 'ExportNamedDeclaration' ? stmt.declaration : stmt;
    if (!d) continue;
    if (d.type === 'FunctionDeclaration' && d.id?.name === 'App') return d;
    if (d.type === 'VariableDeclaration') {
      for (const decl of d.declarations) if (decl.id?.name === 'App' && isFn(decl.init)) return decl.init;
    }
  }
  throw new Error('AOT: no `App` component found (expected `export function App() { ... }`)');
}

/** Builds the module-level scope: StyleSheet objects and simple const values. */
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
        /* not a static const (e.g. a component) — skip; resolved on demand if ever referenced */
      }
    }
  }
  return scope;
}

/** Collects useState initial values from the component body into scope (state renders as its initial). */
function collectHooks(fnBody, scope) {
  for (const stmt of fnBody.body) {
    if (stmt.type !== 'VariableDeclaration') continue;
    for (const decl of stmt.declarations) {
      const init = decl.init;
      if (init?.type === 'CallExpression' && init.callee.name === 'useState' && decl.id.type === 'ArrayPattern') {
        const stateName = decl.id.elements[0]?.name;
        if (stateName) scope[stateName] = init.arguments[0] ? evalStatic(init.arguments[0], scope) : undefined;
      }
    }
  }
}

/** Returns the JSX element a component function returns (unwrapping a parenthesized/single return). */
function findReturnJSX(fnBody) {
  for (const stmt of fnBody.body) {
    if (stmt.type === 'ReturnStatement' && stmt.argument) {
      let a = stmt.argument;
      if (a.type === 'JSXElement') return a;
      throw new Error(`AOT: the component must return a single JSX element (got ${a.type})`);
    }
  }
  throw new Error('AOT: component has no return statement');
}

// ---------------------------------------------------------------------------------------------------
// JSX → flattened style + text
// ---------------------------------------------------------------------------------------------------
function attrValue(attr) {
  const v = attr.value;
  if (!v) return true; // bare boolean attribute
  if (v.type === 'StringLiteral') return v;
  if (v.type === 'JSXExpressionContainer') return v.expression;
  return v;
}

/** Merges an element's `style` attribute (styles.x ref / inline object / array of those) → flat object. */
function collectStyle(openingElement, scope) {
  let merged = {};
  for (const attr of openingElement.attributes) {
    if (attr.type !== 'JSXAttribute' || attr.name.name !== 'style') continue;
    const expr = attrValue(attr);
    const val = evalStatic(expr, scope);
    const parts = Array.isArray(val) ? val : [val];
    for (const p of parts) if (p) merged = { ...merged, ...p };
  }
  return merged;
}

/** Builds a Text node's string from its JSX children (literal segments + {expression} interpolations). */
function collectText(children, scope) {
  let out = '';
  for (const child of children) {
    if (child.type === 'JSXText') {
      // JSX whitespace: collapse runs containing a newline to nothing/space; keep simple inline text.
      out += /\n/.test(child.value) ? child.value.replace(/\s+/g, ' ').trim() : child.value;
    } else if (child.type === 'JSXExpressionContainer') {
      if (child.expression.type === 'JSXEmptyExpression') continue;
      const v = evalStatic(child.expression, scope);
      out += v === undefined || v === null ? '' : String(v);
    } else if (child.type === 'JSXElement') {
      throw new Error('AOT: nested <Text> / element children inside <Text> not yet supported (spans)');
    }
  }
  return out;
}

const cstr = (s) => `"${s.replace(/\\/g, '\\\\').replace(/"/g, '\\"').replace(/\n/g, '\\n').replace(/\t/g, '\\t')}"`;

// ---------------------------------------------------------------------------------------------------
// Emit C
// ---------------------------------------------------------------------------------------------------
function emitNode(el, scope, lines, ctx) {
  const tag = el.openingElement.name.name;
  const nodeType = NODE_TYPES[tag];
  if (!nodeType) throw new Error(`AOT: unsupported element <${tag}> (custom components not yet supported)`);

  const v = `n${ctx.n++}`;
  const style = collectStyle(el.openingElement, scope);
  const assigns = lowerStyle(style);

  lines.push(`    ERNode* ${v} = er_node_create(${nodeType});`);
  lines.push(`    er_props_default(&p);`);
  for (const a of assigns) lines.push(`    p.${a.field} = ${a.expr};`);
  if (tag === 'Text') {
    const text = collectText(el.children, scope);
    lines.push(`    snprintf(p.text, sizeof(p.text), "%s", ${cstr(text)});`);
  }
  lines.push(`    er_node_set_props(${v}, &p);`);

  if (tag !== 'Text') {
    for (const child of el.children) {
      if (child.type !== 'JSXElement') continue;
      const cv = emitNode(child, scope, lines, ctx);
      lines.push(`    er_tree_append_child(${v}, ${cv});`);
    }
  }
  return v;
}

// ---------------------------------------------------------------------------------------------------
// Compile
// ---------------------------------------------------------------------------------------------------
const src = readFileSync(appPath, 'utf8');
const ast = parse(src, { sourceType: 'module', plugins: ['jsx'] });

const scope = moduleScope(ast.program);
const component = findComponent(ast.program);
collectHooks(component.body, scope);
const rootJSX = findReturnJSX(component.body);

const lines = [];
const ctx = { n: 0 };
const appTop = emitNode(rootJSX, scope, lines, ctx);

const body = `/*
 * Generated by the embedded-react Flow B AOT compiler (npm run aot -- ${demo}). DO NOT EDIT.
 * Builds the app's scene graph directly against er_scene.h — no QuickJS, no JS at runtime.
 */
#include "app.gen.h"

#include "er_scene.h"

#include <stdio.h>

void er_app_build(int screen_w, int screen_h)
{
    ERProps p;

    /* A screen-sized root the app tree fills (mirrors AppRegistry mounting into a screen-sized host). */
    ERNode* root = er_node_create(ER_NODE_VIEW);
    er_props_default(&p);
    p.width = (int16_t)screen_w;
    p.height = (int16_t)screen_h;
    er_node_set_props(root, &p);

${lines.join('\n')}
    er_tree_append_child(root, ${appTop});
    er_tree_set_root(root);
}
`;

const header = `/* Generated by the embedded-react Flow B AOT compiler. DO NOT EDIT. */
#ifndef ER_APP_GEN_H
#define ER_APP_GEN_H

/** @brief Builds the AOT-compiled app's scene graph into the engine (call once after backend init). */
void er_app_build(int screen_w, int screen_h);

#endif
`;

mkdirSync(distDir, { recursive: true });
writeFileSync(resolve(distDir, 'app.gen.c'), body);
writeFileSync(resolve(distDir, 'app.gen.h'), header);
console.log(`AOT: compiled demo "${demo}" -> dist/app.gen.c (${ctx.n} nodes)`);
