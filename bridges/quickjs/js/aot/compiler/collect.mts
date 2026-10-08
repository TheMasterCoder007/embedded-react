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

import {parse} from '@babel/parser';
import {readFileSync, existsSync} from 'node:fs';
import {resolve} from 'node:path';
import {aotError} from './diagnostics.mts';
import {evalStaticOrThrow, evalStatic, withUndefined} from './static-eval.mts';
import {i64Lit, cTypeOfValue, floatLit, cstr} from './c-syntax.mts';
import type * as t from '@babel/types';
import type {AotError} from './diagnostics.mts';
import type {Screen} from './options.mts';
import type {
  AssetImport,
  Caps,
  CType,
  EffectRecord,
  FunctionNode,
  ItemField,
  ItemStruct,
  Scope,
  StateRecord,
  StateTable,
  SvgArtifact,
  VectorArtifact,
} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** No state or ref slot widened to 64 bits — the collectors' default. */
export const NO_WIDE = new Set<string>();

const IMAGE_EXT_RE = /\.(png|jpe?g|webp|gif|bmp)$/i;

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Whether a node is a function the compiler can compile or inline.
 *
 * @param node  Any node, or nothing.
 *
 * @returns Whether it is a function declaration, a function expression, or an arrow.
 */
export const isFn = (node: t.Node | null | undefined): node is FunctionNode =>
  (node &&
    (node.type === 'FunctionDeclaration' ||
      node.type === 'FunctionExpression' ||
      node.type === 'ArrowFunctionExpression')) as boolean;

/**
 * Finds the app's root component: `function App` or `const App = …`, exported or not.
 *
 * @param program  The parsed module.
 *
 * @returns The App function.
 */
export function findComponent(program: t.Program): FunctionNode {
  for (const stmt of program.body) {
    const declaration =
      stmt.type === 'ExportNamedDeclaration' ? stmt.declaration : stmt;

    if (!declaration) continue;

    if (
      declaration.type === 'FunctionDeclaration' &&
      declaration.id?.name === 'App'
    ) {
      return declaration;
    }

    if (declaration.type === 'VariableDeclaration') {
      for (const decl of declaration.declarations) {
        if ((decl.id as t.Identifier)?.name === 'App' && isFn(decl.init)) {
          return decl.init;
        }
      }
    }
  }

  throw new Error(
    'AOT: no `App` component found (expected `export function App() { ... }`)',
  );
}

/**
 * Every name a variable declaration directly in `body` binds, destructuring included. In JS each one
 * shadows a module binding of that name for the whole function body, not only from its declaration on.
 *
 * @param body  A function body's statements.
 *
 * @returns The names.
 */
export function declaredNames(body: t.Statement[]): string[] {
  // Walk a binding pattern down to the identifiers it binds.
  const names: string[] = [];
  const walk = (pattern: t.Node | null | undefined): void => {
    if (!pattern) return;
    if (pattern.type === 'Identifier') {
      names.push(pattern.name);
    } else if (pattern.type === 'ArrayPattern') {
      pattern.elements.forEach(walk);
    } else if (pattern.type === 'ObjectPattern') {
      pattern.properties.forEach(property =>
        walk(
          property.type === 'RestElement' ? property.argument : property.value,
        ),
      );
    } else if (pattern.type === 'AssignmentPattern') {
      walk(pattern.left);
    } else if (pattern.type === 'RestElement') {
      walk(pattern.argument);
    }
  };

  // Collect identifiers from each top-level variable declaration, including destructured bindings.
  for (const stmt of body) {
    if (stmt.type === 'VariableDeclaration') {
      for (const decl of stmt.declarations) {
        walk(decl.id);
      }
    }
  }

  return names;
}

/**
 * The module's compile-time scope: `screen`, the seed, and every module-level `const` that folds.
 *
 * @param program  The parsed module.
 * @param screen  The panel size in this build folds its layout at.
 * @param seed  Bindings folded in first, so a const can reference them (image imports).
 *
 * @returns The scope.
 */
export function moduleScope(
  program: t.Program,
  screen: Screen,
  seed: Scope = {},
): Scope {
  // Seed the scope (image imports as asset names) before folding module consts, so a const can reference them.
  const scope: Scope = {screen, ...seed};

  // Fold module variables in order so later ones see earlier ones; skip functions, destructuring and non-folds.
  for (const stmt of program.body) {
    const declaration =
      stmt.type === 'ExportNamedDeclaration' ? stmt.declaration : stmt;
    if (declaration?.type !== 'VariableDeclaration') continue;
    for (const decl of declaration.declarations) {
      if (
        !decl.id ||
        decl.id.type !== 'Identifier' ||
        !decl.init ||
        isFn(decl.init)
      ) {
        continue;
      }
      try {
        scope[decl.id.name] = evalStatic(decl.init, withUndefined(scope));
      } catch {
        /* not a static const — skip */
      }
    }
  }

  return scope;
}

/**
 * Infers a C struct shape from a list-state's initial elements (objects of strings/numbers).
 *
 * @param items  The folded initial value.
 * @param name  The state's name, for errors.
 *
 * @returns The struct: one field per key of the first element.
 */
function inferItemStruct(items: unknown, name: string): ItemStruct {
  // Require a non-empty array so the first element can define the list state's fixed item shape.
  const shapeHint =
    'a list state is a fixed-shape struct array: each element must be an OBJECT with the same string/number fields, ' +
    'e.g. useState([{ title: "A", n: 1 }, { title: "B", n: 2 }]). The first element defines the columns.';
  if (!Array.isArray(items) || !items.length) {
    throw aotError(
      `AOT: list state "${name}" needs ≥1 initial element to infer its item shape`,
      shapeHint,
    );
  }

  // Require the first list element to be a plain object so its keys can define the item struct.
  const first = items[0];
  if (typeof first !== 'object' || first === null || Array.isArray(first)) {
    throw aotError(
      `AOT: list state "${name}" elements must be objects`,
      shapeHint,
    );
  }

  // Type each field from the first element's value: a string, an int or a float.
  const fields = Object.keys(first).map((key): ItemField => {
    const fieldValue = first[key];
    if (typeof fieldValue === 'string') return {key, kind: 'string'};
    if (typeof fieldValue === 'number') {
      return {key, kind: Number.isInteger(fieldValue) ? 'int' : 'float'};
    }

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
 * (`<prefix>count`). prefix='' (the App) keeps the storage names exactly the JS names.
 * `caps` are the compiler's resolved caps (see resolveOptions); a list state takes its capacity from it.
 *
 * @param fnBody  The component's body.
 * @param scope  Compile-time constants the initial values fold against.
 * @param caps  The compile's caps.
 * @param prefix  The C storage prefix: none for the App, `c<n>_` for an inlined instance.
 * @param wide  Slots to declare 64-bit (see compileWidened).
 *
 * @returns The component's state table.
 */
export function collectState(
  fnBody: t.BlockStatement,
  scope: Scope,
  caps: Caps,
  prefix = '',
  wide: Set<string> = NO_WIDE,
): StateTable {
  // Index each state by its JS name and, when it has one, by its setter's name.
  const byName = new Map<string, StateRecord>();
  const bySetter = new Map<string, StateRecord>();

  // Scan the body's top-level declarations for useState and useHostValue calls.
  for (const stmt of fnBody.body) {
    if (stmt.type !== 'VariableDeclaration') continue;

    // Inspect each declaration in the statement so multiple hook bindings in one `const` are collected independently.
    for (const decl of stmt.declarations) {
      // Only hook-style call declarations can define collected state; plain variables and other initializers are skipped.
      const init = decl.init;
      if (init?.type !== 'CallExpression') continue;

      // useHostValue is a host-fed scalar: an s_state field like useState, written only via er_app_set_<name>().
      const isHost = (init.callee as t.Identifier).name === 'useHostValue';
      if ((init.callee as t.Identifier).name !== 'useState' && !isHost) {
        continue;
      }

      // Accept useHostValue(name) as a direct scalar binding, while useState must keep the usual [value, setter] tuple pattern.
      if (
        isHost ? decl.id.type !== 'Identifier' : decl.id.type !== 'ArrayPattern'
      ) {
        continue;
      }

      // Derive the read name and optional setter from the binding shape: host values bind directly, while useState binds [value, setter].
      const name = isHost
        ? (decl.id as t.Identifier).name
        : ((decl.id as t.ArrayPattern).elements[0] as t.Identifier | null)
            ?.name;
      const setter = isHost
        ? undefined
        : ((decl.id as t.ArrayPattern).elements[1] as t.Identifier | null)
            ?.name;
      if (!name) continue;

      // Build either a list-state record from an array initializer or a scalar record from the folded initial value.
      const cField = prefix + name; // C storage name (== name for the App; instance-unique for a child)
      const initArg = init.arguments[0];
      let record: StateRecord;
      if (!isHost && initArg?.type === 'ArrayExpression') {
        // List state → a fixed-capacity C struct array + a count (s_<name>[CAP], s_<name>_count).
        const items = evalStaticOrThrow(
          initArg,
          scope,
          `AOT: the initial value of list state "${name}" must be a compile-time constant array`,
          'useState([...]) initial must be a literal array of objects/numbers/strings — no ' +
            'runtime values or function calls in the initial.',
        ) as Record<string, unknown>[];

        // Infer the item struct, pinning any error to the initial value since this pass adds no location.
        let struct: ItemStruct;
        try {
          struct = inferItemStruct(items, name);
        } catch (error) {
          if (initArg.loc && !(error as AotError).aotLoc) {
            (error as AotError).aotLoc = initArg.loc.start; // collectState isn't withLoc-wrapped
          }
          throw error;
        }
        record = {
          name,
          cField,
          setter,
          kind: 'list',
          struct,
          items,
          cap: caps.listCap,
          cTypeName: `ErItem_${cField}`,
          arrayName: `s_${cField}`,
          countMember: `s_${cField}_count`,
        };
      } else {
        // Scalar state → one s_state field. Fold the initial value; no argument starts it at 0.
        const initValue = initArg
          ? evalStaticOrThrow(
              initArg,
              scope,
              `AOT: the initial value of state "${name}" must be a compile-time constant`,
              'useState(x) initial must be a literal or a constant expression (number, string, ' +
                'bool, or arithmetic over consts) — not a runtime value or function call.',
            )
          : 0;

        // A host value must be a finite number; check it directly, since cTypeOfValue maps non-numbers to int.
        if (isHost && !Number.isFinite(initValue)) {
          throw aotError(
            `AOT: useHostValue("${name}") must be a number — its initial value evaluated to ` +
              `${JSON.stringify(initValue)}`,
            'useHostValue(0) / useHostValue(0.0) — the host feeds an int or float; booleans, ' +
              'strings, and objects are not supported.',
          );
        }

        // Refuse undefined, null and non-finite initials here; they would fold into invalid C with no location.
        if (
          initValue === undefined ||
          initValue === null ||
          (typeof initValue === 'number' && !Number.isFinite(initValue))
        ) {
          const error = aotError(
            `AOT: the initial value of state "${name}" is ${initValue === undefined ? 'undefined' : String(initValue)}`,
            "give it a concrete starting value: useState(0), useState(''), useState(false).",
          );
          if (initArg?.loc) {
            error.aotLoc = initArg.loc.start;
          }
          throw error;
        }

        // Pick the slot's C type from the initial value.
        let cType: CType = cTypeOfValue(initValue);
        // Flag booleans: cTypeOfValue makes them int, but text lowering must render false differently from 0.
        const isBool = typeof initValue === 'boolean';
        // A literal written like 70.0 forces a float slot so the state can hold fractions (a smooth drag).
        if (
          cType === 'int' &&
          initArg?.type === 'NumericLiteral' &&
          typeof initArg.extra?.raw === 'string' &&
          /[.eE]/.test(initArg.extra!.raw as string)
        ) {
          cType = 'float';
        }

        // A setter stores a timestamp in it, so it holds 64 bits (see compileWidened).
        if (cType === 'int' && !isBool && wide.has(cField)) {
          cType = 'i64';
        }

        // String scalar → a fixed char buffer in ErAppState; setters snprintf into it (see scalarAssign).
        const initCode =
          cType === 'string'
            ? cstr(String(initValue))
            : cType === 'float'
              ? floatLit(initValue)
              : cType === 'i64'
                ? i64Lit(Number(initValue))
                : String(Number(initValue));
        record = {
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

      // Register the state for reads by name and for writes through its setter.
      byName.set(name, record);
      if (setter) {
        bySetter.set(setter, record);
      }
    }
  }

  return {byName, bySetter};
}

/**
 * The JSX a component body returns. A top-level `if` whose test folds are decided at compile time, so a
 * responsive layout returns the branch for this build's screen.
 *
 * @param fnBody  The component's body.
 * @param scope  Compile-time constants the `if` tests fold against.
 *
 * @returns The returned element.
 */
export function findReturnJSX(
  fnBody: t.BlockStatement,
  scope: Scope = {},
): t.JSXElement {
  // Fold top-level `if (staticCond) return …` at compile time — responsive layouts switch on `screen`.
  const scan = (statements: t.Statement[]): t.JSXElement | null => {
    for (const stmt of statements) {
      // A top-level `if` must fold; only the branch it takes is scanned.
      if (stmt.type === 'IfStatement') {
        let testValue;
        try {
          testValue = evalStatic(stmt.test, scope);
        } catch {
          throw new Error(
            'AOT: a top-level `if` in the component must have a compile-time-constant test (e.g. ' +
              'on the `screen` global) — runtime layout branching is not supported',
          );
        }

        // Follow only the statically selected branch and keep scanning it until a JSX return is found.
        const branch = testValue ? stmt.consequent : stmt.alternate;
        if (branch) {
          const branchJSX = scan(
            branch.type === 'BlockStatement' ? branch.body : [branch],
          );
          if (branchJSX) return branchJSX;
        }

        continue;
      }

      // A `return` with a value ends the scan and must return a single JSX element.
      if (stmt.type === 'ReturnStatement' && stmt.argument) {
        if (stmt.argument.type === 'JSXElement') return stmt.argument;
        throw new Error(
          `AOT: the component must return a single JSX element (got ${stmt.argument.type})`,
        );
      }
    }

    return null;
  };

  // Scan the body; a component that never returns is an error.
  const returnJSX = scan(fnBody.body);
  if (!returnJSX) {
    throw new Error('AOT: component has no return statement');
  }

  return returnJSX;
}

/**
 * Returns the JSX a function component returns (arrow expression body or a block's return).
 *
 * @param fn  The component.
 * @param scope  Compile-time constants its top-level `if` tests fold against.
 *
 * @returns The returned element.
 */
export function componentReturnJSX(
  fn: FunctionNode,
  scope: Scope = {},
): t.JSXElement {
  if (fn.body.type === 'JSXElement') return fn.body;
  if (fn.body.type === 'BlockStatement') return findReturnJSX(fn.body, scope);
  throw new Error('AOT: component body must return a JSX element');
}

/**
 * Whether a function returns JSX (a component) rather than a value (a helper).
 *
 * @param fn  The function.
 *
 * @returns Whether its body is JSX or has a top-level `return <…/>`.
 */
const fnReturnsJSX = (fn: FunctionNode): boolean =>
  fn.body.type === 'JSXElement' ||
  (fn.body.type === 'BlockStatement' &&
    fn.body.body.some(
      statement =>
        statement.type === 'ReturnStatement' &&
        statement.argument?.type === 'JSXElement',
    ));

/**
 * Resolves a component definition expression to its function node, unwrapping memo(fn) / React.memo(fn).
 *
 * @param node  The definition's initializer.
 *
 * @returns The function, or null when it is not one.
 */
function asComponentFn(node: t.Node | null | undefined): FunctionNode | null {
  if (isFn(node)) return node;

  // Treat memo(...) and React.memo(...) wrappers as transparent when they wrap a component function.
  if (
    node?.type === 'CallExpression' &&
    isFn(node.arguments[0]) &&
    ((node.callee as t.Identifier).name === 'memo' ||
      (node.callee.type === 'MemberExpression' &&
        (node.callee.property as t.Identifier)?.name === 'memo'))
  ) {
    return node.arguments[0];
  }

  return null;
}

/**
 * Collects top-level function components (name → fn node), excluding the `App` entry component.
 *
 * @param program  The parsed module.
 *
 * @returns The components, by name.
 */
export function collectComponents(
  program: t.Program,
): Map<string, FunctionNode> {
  const components = new Map<string, FunctionNode>();
  for (const stmt of program.body) {
    const declaration =
      stmt.type === 'ExportNamedDeclaration' ? stmt.declaration : stmt;
    if (!declaration) continue;

    // `function Card() { return <…/>; }`
    if (
      declaration.type === 'FunctionDeclaration' &&
      declaration.id &&
      declaration.id.name !== 'App' &&
      fnReturnsJSX(declaration)
    ) {
      components.set(declaration.id.name, declaration);
    }

    // `const Card = (…) => <…/>`, optionally wrapped in memo(…).
    if (declaration.type === 'VariableDeclaration') {
      for (const decl of declaration.declarations) {
        if (decl.id?.type !== 'Identifier' || decl.id.name === 'App') continue;
        const fn = asComponentFn(decl.init); // unwrap memo(...)
        if (fn && fnReturnsJSX(fn)) {
          components.set(decl.id.name, fn);
        }
      }
    }
  }

  return components;
}

/**
 * Collects callable HELPER functions (non-component, non-hook) a handler can inline: module-level
 * `function f(){}` / `const f = () => {}` and the component's own local `const f = (args) => {…}` arrows.
 * A helper is any function that does NOT return JSX (those are components) and is a plain function (not a
 * useCallback/useMemo/useState call). Returns Map (name → fn node). Re-collected per component (cheap).
 *
 * @param componentBody  The component's body.
 * @param program  The parsed module.
 *
 * @returns The helpers, by name.
 */
export function collectHelpers(
  componentBody: t.BlockStatement | t.Expression,
  program: t.Program,
): Map<string, FunctionNode> {
  // Record a binding as a helper when it holds a function that does not return JSX.
  const helpers = new Map<string, FunctionNode>();
  const addHelper = (
    name: string | undefined,
    fn: t.Node | null | undefined,
  ): void => {
    if (name && name !== 'App' && isFn(fn) && !fnReturnsJSX(fn)) {
      helpers.set(name, fn);
    }
  };

  // Module-level function declarations and function-valued variables.
  for (const stmt of program.body) {
    const declaration =
      stmt.type === 'ExportNamedDeclaration' ? stmt.declaration : stmt;
    if (!declaration) continue;
    if (declaration.type === 'FunctionDeclaration' && declaration.id) {
      addHelper(declaration.id.name, declaration);
    }
    if (declaration.type === 'VariableDeclaration') {
      for (const decl of declaration.declarations) {
        if (decl.id?.type === 'Identifier') {
          addHelper(decl.id.name, decl.init);
        }
      }
    }
  }

  // The component's own top-level arrows and functions (a hook call is not a function, so it is skipped).
  if (componentBody.type === 'BlockStatement') {
    for (const stmt of componentBody.body) {
      if (stmt.type !== 'VariableDeclaration') continue;
      for (const decl of stmt.declarations) {
        if (decl.id?.type === 'Identifier') {
          addHelper(decl.id.name, decl.init);
        }
      }
    }
  }

  return helpers;
}

/**
 * Collects image imports — `import wxSun from './assets/wx_sun.png'` → Map (local → { name, importPath }).
 * `name` is the file's basename without extension (the asset key `<Image source>` resolves to and that the
 * Flow A bundler also uses); `importPath` is the source-relative path the CLI bakes from. Mirrors the Flow A
 * esbuild asset plugin so the same `import → basename` convention holds in both flows.
 *
 * @param program  The parsed module.
 *
 * @returns The image imports, by local name.
 */
export function collectImageImports(
  program: t.Program,
): Map<string, AssetImport> {
  // Map each default import of an image file to the file's basename.
  const byLocal = new Map<string, AssetImport>();
  for (const stmt of program.body) {
    if (
      stmt.type !== 'ImportDeclaration' ||
      typeof stmt.source.value !== 'string'
    ) {
      continue;
    }

    const importPath = stmt.source.value;
    if (!IMAGE_EXT_RE.test(importPath)) continue;

    const name = importPath.split(/[\\/]/).pop()!.replace(IMAGE_EXT_RE, '');
    for (const specifier of stmt.specifiers) {
      if (specifier.type === 'ImportDefaultSpecifier') {
        byLocal.set(specifier.local.name, {name, importPath});
      }
    }
  }

  return byLocal;
}

/**
 * Collects `import x from './foo.svg'` → Map(localName → { name, importPath }). Unlike an image (whose
 * bytes the CLI bakes AFTER compile, so compile only needs its asset name), an <Svg source> needs the
 * GEOMETRY during compile — so the CLI bakes the .svg to a vector artifact up front (bakeSvgArtifacts) and
 * passes it in as opts.svgArtifacts, keyed by `name`. emitSvgSource resolves the local → name → artifact.
 *
 * @param program  The parsed module.
 *
 * @returns The .svg imports, by local name.
 */
export function collectSvgImports(
  program: t.Program,
): Map<string, AssetImport> {
  // Map each default import of a .svg file to the file's basename.
  const byLocal = new Map<string, AssetImport>();
  for (const stmt of program.body) {
    if (
      stmt.type !== 'ImportDeclaration' ||
      typeof stmt.source.value !== 'string'
    ) {
      continue;
    }

    const importPath = stmt.source.value;
    if (!/\.svg$/i.test(importPath)) continue;

    const name = importPath
      .split(/[\\/]/)
      .pop()!
      .replace(/\.svg$/i, '');
    for (const specifier of stmt.specifiers) {
      if (specifier.type === 'ImportDefaultSpecifier') {
        byLocal.set(specifier.local.name, {name, importPath});
      }
    }
  }

  return byLocal;
}

/**
 * Bakes a Flow B app's <Svg source> imports → { assetName: artifact } via the SAME baker Flow A's esbuild
 * .svg loader uses (svgToVector). This is the I/O half (reads .svg files) kept OUT of the pure compileSource;
 * both CLI entries (aot/compile.mts and the consumer cli.mjs) call it and hand the result to opts.svgArtifacts.
 *
 * @param src  App.jsx source text.
 * @param baseDir  Directory the import paths are resolved against (the app's dir).
 *
 * @returns Each import's artifact, by asset name: its op-tape, or a PNG for one the vector baker cannot draw.
 */
export async function bakeSvgArtifacts(
  src: string,
  baseDir: string,
): Promise<Record<string, SvgArtifact>> {
  // Parse with the TS plugin too (harmless for plain JSX), so a .tsx app's <Svg source> imports are found.
  const program = parse(src, {
    sourceType: 'module',
    plugins: ['jsx', 'typescript'],
  }).program;
  const imports = collectSvgImports(program);
  if (imports.size === 0) return {};

  // Load the baker only when there is something to bake.
  const {svgToVector, svgToRaster, writeRasterPng} =
    await import('../../assets/bake-svg.mjs');

  // Read each imported file and bake it to a vector op-tape.
  const artifacts: Record<string, SvgArtifact> = {};
  for (const [, svgImport] of imports) {
    const svgPath = resolve(baseDir, svgImport.importPath);
    if (!existsSync(svgPath)) {
      throw new Error(
        `AOT: <Svg source> asset "${svgImport.name}" not found at ${svgPath}`,
      );
    }

    // Rasterize an SVG the vector baker cannot fully draw to a PNG, so it renders as an Image instead of dropping.
    const svgText = readFileSync(svgPath, 'utf8');
    const artifact = (await svgToVector(svgText)) as VectorArtifact;
    if (artifact.dropped && artifact.dropped.length) {
      console.warn(
        `embedded-react: ${svgImport.name}.svg uses unsupported SVG feature(s) [${artifact.dropped.join(', ')}] — ` +
          `rasterizing it as a fallback image (Flow B bakes the PNG into assets.generated.c). Simplify the SVG ` +
          `to keep it a live vector.`,
      );
      const {width, height, png} = await svgToRaster(svgText);
      artifacts[svgImport.name] = {
        kind: 'raster',
        name: svgImport.name,
        width,
        height,
        png: writeRasterPng(svgImport.name, png),
      };
    } else {
      artifacts[svgImport.name] = artifact;
    }
  }

  return artifacts;
}

/**
 * Collects `const fn = useCallback((...) => {...}, deps)` → Map (name → arrow fn node). Deps are ignored:
 * the AOT re-renders via its own dependency tracking, so useCallback only names a shared C handler.
 *
 * @param fnBody  The component's body.
 *
 * @returns The callbacks, by name.
 */
export function collectCallbacks(
  fnBody: t.BlockStatement | t.Expression,
): Map<string, FunctionNode> {
  const callbacks = new Map<string, FunctionNode>();
  if (fnBody.type !== 'BlockStatement') return callbacks;

  // Collect top-level useCallback bindings whose first argument is an inline function, keyed by the variable name.
  for (const stmt of fnBody.body) {
    if (stmt.type !== 'VariableDeclaration') continue;
    for (const decl of stmt.declarations) {
      const init = decl.init;
      if (
        init?.type === 'CallExpression' &&
        (init.callee as t.Identifier).name === 'useCallback' &&
        decl.id.type === 'Identifier' &&
        isFn(init.arguments[0])
      ) {
        callbacks.set(decl.id.name, init.arguments[0]);
      }
    }
  }

  return callbacks;
}

/**
 * Collects `const m = useMemo(() => expr, deps)` → Map (name → the memo's expression node).
 *
 * @param fnBody  The component's body.
 *
 * @returns Each memo's expression, by name.
 */
export function collectMemos(
  fnBody: t.BlockStatement | t.Expression,
): Map<string, t.Expression> {
  const memos = new Map<string, t.Expression>();
  if (fnBody.type !== 'BlockStatement') return memos;

  // Collect expression-bodied useMemo bindings; block-bodied memos are rejected until statement-bodied folding is supported.
  for (const stmt of fnBody.body) {
    if (stmt.type !== 'VariableDeclaration') continue;
    for (const decl of stmt.declarations) {
      const init = decl.init;
      if (
        init?.type === 'CallExpression' &&
        (init.callee as t.Identifier).name === 'useMemo' &&
        decl.id.type === 'Identifier' &&
        isFn(init.arguments[0])
      ) {
        const body = init.arguments[0].body;
        if (body.type === 'BlockStatement') {
          throw new Error(
            `AOT: useMemo for "${decl.id.name}" must be a single expression (for now)`,
          );
        }
        memos.set(decl.id.name, body);
      }
    }
  }

  return memos;
}

/**
 * Collects `useEffect(() => {…}, deps)` calls → { fn, deps, node }. Dep validity is checked at compile time.
 *
 * @param fnBody  The component's body.
 *
 * @returns The effects, in source order.
 */
export function collectEffects(
  fnBody: t.BlockStatement | t.Expression,
): EffectRecord[] {
  const effects: EffectRecord[] = [];
  if (fnBody.type !== 'BlockStatement') return effects;

  // Collect top-level useEffect calls, requiring an inline function so the effect body can be lowered later with its deps.
  for (const stmt of fnBody.body) {
    if (stmt.type !== 'ExpressionStatement') continue;
    const call = stmt.expression;
    if (
      call?.type === 'CallExpression' &&
      call.callee.type === 'Identifier' &&
      call.callee.name === 'useEffect'
    ) {
      if (!isFn(call.arguments[0])) {
        throw aotError(
          'AOT: useEffect must take an inline function',
          'write useEffect(() => { … }, []).',
        );
      }
      effects.push({
        fn: call.arguments[0],
        deps: call.arguments[1],
        node: call,
      });
    }
  }

  return effects;
}

/**
 * True if a function component declares any useState (per-instance child state — not yet supported).
 *
 * @param fn  The component.
 *
 * @returns Whether its body calls useState.
 */
export function usesState(fn: FunctionNode): boolean {
  if (fn.body.type !== 'BlockStatement') return false;

  return fn.body.body.some(
    statement =>
      statement.type === 'VariableDeclaration' &&
      statement.declarations.some(
        decl =>
          decl.init?.type === 'CallExpression' &&
          (decl.init.callee as t.Identifier).name === 'useState',
      ),
  );
}
