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

// Build-time font-size discovery and coverage check. The engine has no runtime rasterizer: a font is
// baked at fixed pixel sizes and font_registry_get() (engine/font/font_registry.c) snaps anything
// else to the nearest baked size, ties going to the larger. So the bake has to know every size the
// app's text uses, and a size it missed is a silent visual bug.
//
// Discovery folds constants, which is what makes a token-driven type scale work:
//   const TYPE = {body: 14, title: 22};   <Text style={{fontSize: TYPE.title}}>
// A literal-only scan bakes nothing for that app and every size collapses onto one. Folding covers
// module-level constants, objects (including spreads), arrays, arithmetic, and both arms of a
// responsive ternary — a `screen.width`-driven scale can't be resolved to one branch at build time,
// so both branches are baked and either panel renders correctly.
//
// What folding can't see is a size that only exists at runtime: a prop, state, a computed key. Those
// are reported rather than guessed at, because the runtime result is a silent snap, not an error.
import {parse} from '@babel/parser';
import {BUILTIN_SIZES} from './bake-font.mjs';
import {BUILTIN_FAMILY} from './glyph-coverage.mjs';

/** Sizes one family may register before the engine starts dropping them (FONT_FAMILY_MAX_SIZES). */
const MAX_BAKED_SIZES = 16;

/** Bounds of the engine's uint8 BitmapFont.pixel_size — a folded value outside them isn't a size. */
const MIN_PX = 1;
const MAX_PX = 255;

/** The range er_text_measure()/er_text_draw() clamp a requested size into before resolving a font. */
const RENDER_MIN_PX = 8;
const RENDER_MAX_PX = 96;

// A responsive scale multiplies out (two ternary arms times two more, …). Both limits keep a
// pathological expression from turning discovery into a combinatorial walk; the cost of clipping is
// an under-bake that the "could not be read" report below already covers.
const MAX_CANDIDATES = 12;
const MAX_DEPTH = 16;

// Whole-word `fontSize: <number>` — the pre-folding scan, kept as the fallback for a source the
// parser chokes on (a future syntax, a mangled bundle). Strictly weaker, never silently preferred.
const LITERAL_SIZE = /\bfontSize\s*:\s*(\d+(?:\.\d+)?)/g;

const PARSER_OPTS = {
  sourceType: 'unambiguous', // an IIFE bundle is a script; an app entry is a module
  errorRecovery: true,
  // Same pair the AOT parser uses: the TS plugin is harmless on plain JS/JSX and lets a .tsx app
  // through unchanged.
  plugins: ['jsx', 'typescript'],
};

/*--- AST walking --------------------------------------------------------------------------------*/

// Keys that hold position/comment bookkeeping rather than child nodes. Skipping them is purely a
// speed matter (they carry no `type`), but on a 300 KB bundle it is most of the walk.
const NON_NODE_KEYS = new Set([
  'loc',
  'start',
  'end',
  'range',
  'extra',
  'comments',
  'leadingComments',
  'trailingComments',
  'innerComments',
]);

/** Depth-first visit of every AST node under `node`, in source order. */
function walk(node, visit) {
  if (Array.isArray(node)) {
    for (const n of node) walk(n, visit);
    return;
  }
  if (!node || typeof node !== 'object' || typeof node.type !== 'string')
    return;
  visit(node);
  for (const key of Object.keys(node)) {
    if (NON_NODE_KEYS.has(key)) continue;
    const child = node[key];
    if (child && typeof child === 'object') walk(child, visit);
  }
}

/** Static name of a non-computed property key, or null when the key isn't a fixed string. */
function keyName(node) {
  if (node.computed) return null;
  const k = node.key;
  if (!k) return null;
  if (k.type === 'Identifier') return k.name;
  if (k.type === 'StringLiteral') return k.value;
  if (k.type === 'NumericLiteral') return String(k.value);
  return null;
}

/*--- Constant folding ---------------------------------------------------------------------------*/

// A folded value is either a number or a Scale: an object/array whose properties are themselves
// candidate lists, so `TYPE.title` is a lookup and a ternary is a union.
const scale = props => ({props});
const isScale = v =>
  v !== null && typeof v === 'object' && v.props instanceof Map;

/** Trims a candidate list to the fan-out budget, dropping duplicates first. */
const cap = values =>
  values.length > MAX_CANDIDATES
    ? [...new Set(values)].slice(0, MAX_CANDIDATES)
    : values;

/** The numbers in a candidate list (a Scale is not a size). */
const numbers = values => values.filter(v => typeof v === 'number');

const ARITHMETIC = {
  '+': (a, b) => a + b,
  '-': (a, b) => a - b,
  '*': (a, b) => a * b,
  '/': (a, b) => a / b,
  '%': (a, b) => a % b,
  '**': (a, b) => a ** b,
};

// Rounding is how a ratio-based scale lands on whole pixels (Math.round(base * 1.25)); min/max is
// how it gets clamped. Anything else on Math is left unresolved rather than guessed.
const MATH_FNS = {
  round: Math.round,
  floor: Math.floor,
  ceil: Math.ceil,
  abs: Math.abs,
  min: Math.min,
  max: Math.max,
};

/**
 * Statically evaluates an expression to the values it can take.
 *
 * @param {object} node     Expression AST node.
 * @param {object} scope    Fold scope from buildScope().
 * @param {number} [depth]  Recursion guard.
 * @returns {any[]} Candidate values (numbers and/or Scales); empty when nothing can be resolved.
 */
function evaluate(node, scope, depth = 0) {
  if (!node || depth > MAX_DEPTH) return [];
  const rec = n => evaluate(n, scope, depth + 1);

  switch (node.type) {
    case 'NumericLiteral':
      return [node.value];

    case 'Identifier':
      return lookup(node.name, scope, depth);

    // A parenthesized expression, a TS cast, a `!` assertion: transparent for our purposes.
    case 'ParenthesizedExpression':
    case 'TSAsExpression':
    case 'TSSatisfiesExpression':
    case 'TSNonNullExpression':
    case 'TSTypeAssertion':
    case 'TypeCastExpression':
      return rec(node.expression);

    case 'UnaryExpression': {
      if (node.operator !== '-' && node.operator !== '+') return [];
      const sign = node.operator === '-' ? -1 : 1;
      return numbers(rec(node.argument)).map(v => sign * v);
    }

    case 'BinaryExpression': {
      const op = ARITHMETIC[node.operator];
      if (!op) return [];
      const left = numbers(rec(node.left));
      const right = numbers(rec(node.right));
      const out = [];
      for (const a of left) for (const b of right) out.push(op(a, b));
      return cap(out);
    }

    // Both arms: which one a responsive app takes depends on the panel it boots on, and the same
    // bundle runs on all of them.
    case 'ConditionalExpression':
      return cap([...rec(node.consequent), ...rec(node.alternate)]);

    // `a ?? b` / `a || b` are the same shape of choice; `&&` can only ever yield its right arm.
    case 'LogicalExpression':
      return node.operator === '&&'
        ? rec(node.right)
        : cap([...rec(node.left), ...rec(node.right)]);

    case 'ObjectExpression': {
      const props = new Map();
      for (const p of node.properties) {
        if (p.type === 'SpreadElement') {
          for (const base of rec(p.argument).filter(isScale))
            for (const [k, v] of base.props) props.set(k, v);
          continue;
        }
        if (p.type !== 'ObjectProperty') continue; // a method is never a size
        const name = keyName(p);
        if (name === null) continue;
        const values = rec(p.value);
        if (values.length) props.set(name, values);
      }
      return [scale(props)];
    }

    case 'ArrayExpression': {
      const props = new Map();
      node.elements.forEach((el, i) => {
        if (!el || el.type === 'SpreadElement') return;
        const values = rec(el);
        if (values.length) props.set(String(i), values);
      });
      return [scale(props)];
    }

    case 'MemberExpression': {
      const objects = rec(node.object).filter(isScale);
      if (!objects.length) return [];
      const key = memberKey(node, scope, depth);
      const out = [];
      for (const obj of objects) {
        // An unresolvable key (`TYPE[variant]`) still narrows things to one scale, and every entry
        // in it is something the app can reach — take the lot rather than nothing.
        if (key === null) {
          for (const v of obj.props.values()) out.push(...v);
        } else {
          out.push(...(obj.props.get(key) ?? []));
        }
      }
      return cap(out);
    }

    case 'CallExpression': {
      const {callee} = node;
      if (
        callee?.type !== 'MemberExpression' ||
        callee.computed ||
        callee.object?.type !== 'Identifier' ||
        callee.object.name !== 'Math' ||
        callee.property?.type !== 'Identifier'
      )
        return [];
      const fn = MATH_FNS[callee.property.name];
      if (!fn) return [];
      // Every argument must resolve, or the result isn't the one the app computes.
      const args = node.arguments.map(a => numbers(rec(a)));
      if (!args.length || args.some(a => !a.length)) return [];
      return cap(combine(args).map(vals => fn(...vals)));
    }

    default:
      return [];
  }
}

/** Cartesian product of per-argument candidate lists, clipped to the fan-out budget. */
function combine(lists) {
  let rows = [[]];
  for (const list of lists) {
    const next = [];
    for (const row of rows)
      for (const v of list) {
        if (next.length >= MAX_CANDIDATES) break;
        next.push([...row, v]);
      }
    rows = next;
  }
  return rows;
}

/** Property name a member expression reads, resolving a computed key when it folds to a constant. */
function memberKey(node, scope, depth) {
  if (!node.computed)
    return node.property.type === 'Identifier' ? node.property.name : null;
  if (node.property.type === 'StringLiteral') return node.property.value;
  if (node.property.type === 'NumericLiteral')
    return String(node.property.value);
  const folded = numbers(evaluate(node.property, scope, depth + 1));
  return folded.length === 1 ? String(folded[0]) : null;
}

/**
 * Resolves a name to its candidate values, memoizing the answer (including "nothing").
 *
 * Constants come first. A name with no constant behind it may still be a scale that was handed
 * down — see buildScope() — and that is resolved on demand, so the bindings of the thousands of
 * names a bundle contains are only ever evaluated for the handful a fontSize actually reads.
 */
function lookup(name, scope, depth) {
  const known = scope.env.get(name);
  if (known !== undefined) return known; // a memoized "nothing" is an answer, not a miss
  const bound = scope.bindings.get(name);
  if (!bound || scope.resolving.has(name)) return [];

  scope.resolving.add(name);
  const values = cap(
    bound.flatMap(node => evaluate(node, scope, depth + 1)).filter(isScale),
  );
  scope.resolving.delete(name);
  scope.env.set(name, values);
  return values;
}

/**
 * Builds the fold scope for one program: the names it binds to a constant, and the names a scale is
 * handed down under.
 *
 * Binding a name once is the whole scoping model for constants — no scope tracking, so a name
 * declared twice (a module constant shadowed by a local, say) is left out rather than resolved to
 * the wrong one. Bundles rename colliding identifiers, so in practice this only skips genuine reuse.
 *
 * The handed-down half is what makes a scale work in a component that did not declare it:
 * `<Dial sz={SZ}>` (or `jsx(Dial, {sz: SZ})` once a bundler has lowered it) against a `sz.big`
 * inside. Only a name passed a *named* value binds — `{sz: SZ}`, not an inline literal — which keeps
 * this to the "pass the scale down" pattern it is for. A name passed different scales in different
 * places resolves to all of them, which over-bakes at worst; declared names always win.
 *
 * @param {object} ast  Parsed program.
 * @returns {{env: Map<string,any[]>, bindings: Map<string,object[]>, resolving: Set<string>}}
 */
function buildScope(ast) {
  const declarations = [];
  const counts = new Map();
  const bindings = new Map();
  const bind = (name, node) => {
    if (node.type !== 'Identifier' && node.type !== 'MemberExpression') return;
    const list = bindings.get(name);
    if (list) list.push(node);
    else bindings.set(name, [node]);
  };

  walk(ast.program, n => {
    if (n.type === 'VariableDeclarator') {
      if (n.id?.type === 'Identifier' && n.init) {
        declarations.push(n);
        counts.set(n.id.name, (counts.get(n.id.name) ?? 0) + 1);
      }
      return;
    }
    if (
      n.type === 'JSXAttribute' &&
      n.name?.type === 'JSXIdentifier' &&
      n.value?.type === 'JSXExpressionContainer'
    ) {
      bind(n.name.name, n.value.expression);
      return;
    }
    if (n.type === 'ObjectProperty') {
      const name = keyName(n);
      if (name !== null) bind(name, n.value);
    }
  });

  // Constants fold first, against no bindings at all: lookup() memoizes what it resolves, and a
  // binding consulted mid-fold could cache an answer taken from a half-built environment. Two passes,
  // so a scale that reads a constant declared below it still folds; a third would only help a chain
  // that deep, which no type scale is.
  const scope = {env: new Map(), bindings: new Map(), resolving: new Set()};
  for (let pass = 0; pass < 2; pass++) {
    for (const d of declarations) {
      const name = d.id.name;
      if (counts.get(name) > 1) continue;
      const values = evaluate(d.init, scope);
      if (values.length) scope.env.set(name, values);
    }
  }
  // A declared name resolves to its declaration or to nothing; it never falls through to a binding.
  for (const name of counts.keys()) bindings.delete(name);
  scope.bindings = bindings;
  return scope;
}

/*--- Discovery ----------------------------------------------------------------------------------*/

/** Rounds a folded value to a bakeable pixel size, or null when it isn't one. */
function toPixelSize(value) {
  const px = Math.round(value);
  return Number.isFinite(px) && px >= MIN_PX && px <= MAX_PX ? px : null;
}

/** One-line, length-capped rendering of the expression behind an unresolved fontSize. */
function snippet(source, node) {
  const raw = source.slice(node.start, node.end).replace(/\s+/g, ' ').trim();
  return raw.length > 48 ? `${raw.slice(0, 47)}…` : raw;
}

/** Literal-only discovery, for a source the parser rejected. */
function scanLiterals(source) {
  const sizes = new Set();
  for (const m of source.matchAll(LITERAL_SIZE)) {
    const px = toPixelSize(Number(m[1]));
    if (px !== null) sizes.add(px);
  }
  return {sizes: [...sizes].sort((a, b) => a - b), dynamic: []};
}

/**
 * Finds the pixel sizes an app's text renders at, folding constants so a token-driven scale is
 * discovered rather than missed.
 *
 * @param {string} source  JS/JSX/TSX source: a bundle, a chunk, or a single app entry.
 * @returns {{sizes: number[], dynamic: Array<{expr:string, count:number}>}} Ascending de-duplicated
 *          sizes, plus the `fontSize` expressions that could not be resolved at build time.
 */
export function analyzeFontSizes(source) {
  if (!source) return {sizes: [], dynamic: []};
  let ast;
  try {
    ast = parse(source, PARSER_OPTS);
  } catch {
    return scanLiterals(source); // never worse than the literal scan this replaced
  }

  const scope = buildScope(ast);
  const sizes = new Set();
  const dynamic = new Map(); // expression text → occurrences
  walk(ast.program, n => {
    if (n.type !== 'ObjectProperty' || keyName(n) !== 'fontSize') return;
    // A value that resolves but can't be baked (0, negative, past the engine's uint8 pixel_size) is
    // an authoring slip, not a discovery failure: leave it out of the bake and out of the report.
    const resolved = numbers(evaluate(n.value, scope));
    if (resolved.length) {
      for (const value of resolved) {
        const px = toPixelSize(value);
        if (px !== null) sizes.add(px);
      }
      return;
    }
    const expr = snippet(source, n.value);
    dynamic.set(expr, (dynamic.get(expr) ?? 0) + 1);
  });

  return {
    sizes: [...sizes].sort((a, b) => a - b),
    dynamic: [...dynamic]
      .map(([expr, count]) => ({expr, count}))
      .sort((a, b) => b.count - a.count || a.expr.localeCompare(b.expr)),
  };
}

/**
 * Compact signature of the sizes a source uses. Watch loops fold this into their "did the asset
 * inputs change?" key so a `fontSize` edit re-runs the check below.
 *
 * Without it the check is skipped exactly where it matters most: an app with no imported font has no
 * baked `sizes` in that key at all, so editing a size changes nothing the loop compares and the
 * re-bake never happens. Sizes that resolve and sizes that don't both count — swapping a constant
 * for a runtime value changes what is reported without changing what is baked.
 *
 * @param {{sizes:number[], dynamic:Array<{expr:string}>}} used  analyzeFontSizes() result.
 * @returns {string} Sizes and unresolved expressions, joined.
 */
export function fontSizeSignature(used) {
  const {sizes = [], dynamic = []} = used ?? {};
  return `${sizes.join(',')}|${dynamic.map(d => d.expr).join('\u0000')}`;
}

/*--- Coverage check -----------------------------------------------------------------------------*/

/** Pixel sizes a bakeFont() result — or the job that produced it — covers. */
function bakedSizes(font) {
  return (font.sizes ?? [])
    .map(s => (typeof s === 'number' ? s : s?.pixelSize))
    .filter(px => Number.isFinite(px));
}

/**
 * The size the engine substitutes for an unbaked one: nearest wins, a tie goes to the larger.
 * Mirrors pick_closest() in engine/font/font_registry.c, including the clamp the text renderer
 * applies to the requested size first.
 *
 * @param {number[]} baked  Sizes the family is baked at (non-empty).
 * @param {number} wanted   The size the app asked for.
 * @returns {number} The size that actually renders.
 */
export function pickBakedSize(baked, wanted) {
  const target = Math.min(Math.max(wanted, RENDER_MIN_PX), RENDER_MAX_PX);
  let best = baked[0];
  for (const px of baked.slice(1)) {
    const diff = Math.abs(target - px);
    const bestDiff = Math.abs(target - best);
    if (diff < bestDiff || (diff === bestDiff && px > best)) best = px;
  }
  return best;
}

/**
 * Finds the used sizes no font in the bake covers.
 *
 * Which fonts are checked follows what the app can draw with, the same way the glyph check does it:
 * each imported font when there are any (they all render the app's text, and a family missing a size
 * is the bug worth reporting), the engine's built-in font when there are none.
 *
 * @param {object} opts
 * @param {number[]} [opts.sizes]       Sizes the app uses (from analyzeFontSizes).
 * @param {Array<object>} [opts.fonts]  bakeFont() results or jobs; empty means built-in font only.
 * @returns {{gaps: Array<{size:number, family:string, snapsTo:number, baked:number[]}>,
 *          dropped: Array<{family:string, registered:number[], ignored:number[]}>}} `gaps` are sizes
 *          that will snap; `dropped` splits a family baked past the engine's per-family limit into
 *          the sizes that register and the ones that never do.
 */
export function findSizeGaps({sizes = [], fonts = []}) {
  const checked = fonts.length
    ? fonts.map(f => ({family: f.family, baked: bakedSizes(f)}))
    : [{family: BUILTIN_FAMILY, baked: [...BUILTIN_SIZES]}];

  const gaps = [];
  const dropped = [];
  for (const {family, baked} of checked) {
    if (!baked.length) continue;
    // Registration stops at the limit, so anything past it is not merely unbaked but unreachable.
    const live = baked.slice(0, MAX_BAKED_SIZES);
    if (baked.length > live.length)
      dropped.push({
        family,
        registered: live,
        ignored: baked.slice(MAX_BAKED_SIZES),
      });
    for (const size of sizes)
      if (!live.includes(size))
        gaps.push({
          size,
          family,
          snapsTo: pickBakedSize(live, size),
          baked: live,
        });
  }
  return {gaps: gaps.sort((a, b) => a.size - b.size), dropped};
}

const MAX_LISTED = 8;

/** `[a, b, c]` as an assets.config.js `sizes` line would spell it. */
const sizeList = sizes => `[${[...sizes].sort((a, b) => a - b).join(', ')}]`;

/** A family name as a valid object key (a family is a file basename — `Inter-Regular` needs quotes). */
const configKey = family =>
  /^[A-Za-z_$][\w$]*$/.test(family) ? family : JSON.stringify(family);

/**
 * Renders the size check as a developer-facing warning, ending with the change that fixes it.
 *
 * @param {object} opts
 * @param {Array<object>} [opts.gaps]     From findSizeGaps().
 * @param {Array<object>} [opts.dropped]  From findSizeGaps().
 * @param {Array<object>} [opts.dynamic]  From analyzeFontSizes().
 * @param {Array<object>} [opts.fonts]    The same fonts, for the suggested config line.
 * @returns {string} The warning text (empty when there is nothing to report).
 */
export function formatFontSizeReport({
  gaps = [],
  dropped = [],
  dynamic = [],
  fonts = [],
}) {
  const blocks = [];

  if (gaps.length) {
    const lines = gaps
      .slice(0, MAX_LISTED)
      .map(
        g =>
          `  ${g.size}px → renders at ${g.snapsTo}px in ${g.family} (baked: ${g.baked.join(', ')})`,
      );
    if (gaps.length > MAX_LISTED)
      lines.push(`  … and ${gaps.length - MAX_LISTED} more`);
    blocks.push(
      `embedded-react: ${gaps.length} font size(s) the app uses are not baked and will render at the ` +
        `nearest baked size:\n${lines.join('\n')}\n${gapFix(gaps, fonts)}`,
    );
  }

  if (dynamic.length) {
    const total = dynamic.reduce((n, d) => n + d.count, 0);
    const lines = dynamic.slice(0, MAX_LISTED).map(d => {
      const times = d.count > 1 ? `  (×${d.count})` : '';
      return `  fontSize: ${d.expr}${times}`;
    });
    if (dynamic.length > MAX_LISTED)
      lines.push(`  … and ${dynamic.length - MAX_LISTED} more`);
    blocks.push(
      `embedded-react: ${total} fontSize value(s) could not be read at build time, so no size was ` +
        `baked for them (each snaps to the nearest baked size at runtime):\n${lines.join('\n')}\n` +
        `  fix: name the size where it is used, from a module-level constant the bake can fold ` +
        `(const TYPE = {body: 14};\n       fontSize: TYPE.body) — one arriving through a prop or ` +
        `state can't be traced. Or pin the sizes:\n       assets.config.js → ` +
        `fonts: {${fonts.length ? configKey(fonts[0].family) : '<Family>'}: {sizes: [...]}}`,
    );
  }

  for (const d of dropped) {
    blocks.push(
      `embedded-react: ${d.family} is baked at more than ${MAX_BAKED_SIZES} sizes — the engine ` +
        `registers the first ${MAX_BAKED_SIZES} and ignores ${sizeList(d.ignored)}, so text at ` +
        `those sizes snaps to one that did register.\n  fix: choose the ${MAX_BAKED_SIZES} that ` +
        `matter — assets.config.js → fonts: {${configKey(d.family)}: {sizes: ${sizeList(d.registered)}}}`,
    );
  }

  return blocks.join('\n');
}

/** The config (or design) change that closes a set of gaps. */
function gapFix(gaps, fonts) {
  if (!fonts.length) {
    return (
      `  fix: the built-in font is baked at ${sizeList(BUILTIN_SIZES)} — use one of those sizes, or ` +
      `import a .ttf/.otf\n       and bake your own (assets.config.js → fonts: ` +
      `{<Family>: {sizes: ${sizeList(new Set(gaps.map(g => g.size)))}}})`
    );
  }
  const perFamily = fonts
    .map(f => {
      const missing = gaps.filter(g => g.family === f.family).map(g => g.size);
      if (!missing.length) return null;
      const wanted = new Set([...bakedSizes(f), ...missing]);
      return `${configKey(f.family)}: {sizes: ${sizeList(wanted)}}`;
    })
    .filter(Boolean);
  return `  fix: assets.config.js → fonts: {${perFamily.join(', ')}}`;
}

let lastWarning = null; // dev loops re-bake on every save — don't reprint an unchanged warning

/**
 * Checks size coverage and warns. Repeats are suppressed while the warning is unchanged, so a
 * watch/hot-reload loop reports each gap once rather than on every save.
 *
 * @param {object} opts
 * @param {{sizes:number[], dynamic:Array<object>}} opts.used  analyzeFontSizes() result.
 * @param {Array<object>} [opts.fonts]  bakeFont() results or jobs; empty means built-in font only.
 * @param {Function} [opts.log]         Sink for the warning (default console.warn).
 * @returns {{gaps:Array<object>, dropped:Array<object>}} What was found (both empty when clean).
 */
export function warnFontSizes({used, fonts = [], log = console.warn}) {
  const {sizes = [], dynamic = []} = used ?? {};
  const {gaps, dropped} = findSizeGaps({sizes, fonts});
  const text = formatFontSizeReport({gaps, dropped, dynamic, fonts});
  if (text && text !== lastWarning) log(text);
  lastWarning = text || null;
  return {gaps, dropped};
}
