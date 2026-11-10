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

import {checkNumber} from './check.mts';
import {parse} from '@babel/parser';
import {readFileSync, existsSync} from 'node:fs';
import {posix, resolve} from 'node:path';
import {aotError, locateError} from './diagnostics.mts';
import {mergeBranchElements} from './branch-merge.mts';
import {inferItemStruct} from './list-items.mts';
import {isObjectTree} from './records.mts';
import {stateType} from './ir/types.mts';
import {splitCallOf} from './list-split.mts';
import {
  bootCheckpoint,
  bootSlotOf,
  buildValueOf,
  foldBoot,
  isBoot,
  nameBootValues,
  screenBinding,
} from './boot.mts';
import type {BootValue} from './boot.mts';
import {
  evalStaticOrThrow,
  evalStatic,
  setBinding,
  withUndefined,
  rethrowFoldLimit,
  RuntimeLeaf,
} from './static-eval.mts';
import type * as t from '@babel/types';
import type {SlotWidths} from './ir/types.mts';
import type {AotError} from './diagnostics.mts';
import type {Screen} from './options.mts';
import type {
  AssetImport,
  Caps,
  SlotKind,
  EffectRecord,
  FunctionNode,
  ItemStruct,
  Local,
  RecordState,
  ScalarState,
  Scope,
  StateRecord,
  StateTable,
  SvgArtifact,
  VectorArtifact,
} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * The kind of slot a constant is stored in: string, float (a fractional number), or int (anything else).
 *
 * @param value  A compile-time value.
 *
 * @returns Its slot kind.
 */
const slotKindOfValue = (value: unknown): 'string' | 'float' | 'int' =>
  typeof value === 'string'
    ? 'string'
    : typeof value === 'number' && !Number.isInteger(value)
      ? 'float'
      : 'int';

/** No state or ref slot widened to 64 bits — the collectors' default. */
export const NO_WIDE: SlotWidths = new Map();

/**
 * What a whole-number initial value needs beyond an int slot, typed as a constant of its size is: nothing in
 * the int range, a 64-bit number past it, a float past 64 bits.
 *
 * @param value  The initial value.
 *
 * @returns 'number' for a 64-bit slot, 'float' for a float one, or null for an int.
 */
export const wideInitial = (value: unknown): 'number' | 'float' | null =>
  typeof value !== 'number' || !Number.isInteger(value)
    ? null
    : value < -(2 ** 63) || value >= 2 ** 63
      ? 'float'
      : value < -(2 ** 31) || value > 2 ** 31 - 1
        ? 'number'
        : null;

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
  for (const statement of program.body) {
    const declaration =
      statement.type === 'ExportNamedDeclaration'
        ? statement.declaration
        : statement;

    if (!declaration) continue;

    if (
      declaration.type === 'FunctionDeclaration' &&
      declaration.id?.name === 'App'
    ) {
      return declaration;
    }

    if (declaration.type === 'VariableDeclaration') {
      for (const declarator of declaration.declarations) {
        if (
          (declarator.id as t.Identifier)?.name === 'App' &&
          isFn(declarator.init)
        ) {
          return declarator.init;
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
  for (const statement of body) {
    if (statement.type === 'VariableDeclaration') {
      for (const declarator of statement.declarations) {
        walk(declarator.id);
      }
    }
  }

  return names;
}

/**
 * A const's folded value, with the boot values in it named after the const.
 *
 * @param name  The const's name.
 * @param value  Its folded value.
 *
 * @returns The value.
 */
export function bindConst(name: string, value: unknown): unknown {
  nameBootValues(value, name);
  return value;
}

/**
 * The module's compile-time scope: `screen`, the seed, and every module-level `const` that folds. A const
 * derived from `screen` is a boot value (see BootValue).
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
  const scope: Scope = {screen: screenBinding(screen), ...seed};

  // Fold module variables in order so later ones see earlier ones; skip functions, destructuring and non-folds.
  for (const statement of program.body) {
    const declaration =
      statement.type === 'ExportNamedDeclaration'
        ? statement.declaration
        : statement;
    if (declaration?.type !== 'VariableDeclaration') continue;
    for (const declarator of declaration.declarations) {
      if (
        !declarator.id ||
        declarator.id.type !== 'Identifier' ||
        !declarator.init ||
        isFn(declarator.init)
      ) {
        continue;
      }
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
        // Not a static const, so skipped, unless a string helper in it is past what a package can run.
        rethrowFoldLimit(error);
      }
    }
  }

  return scope;
}

/**
 * Refuses an initial list with more rows than a list holds: they would be dropped at boot without a word. Each
 * string's cap is sized from its values when the app is lowered.
 *
 * @param stateName  The state's JS name.
 * @param items  The list's initial rows.
 * @param caps  The compile's caps.
 * @param initialArgument  The initial value, for the error's location.
 */
function checkInitialRows(
  stateName: string,
  items: unknown[],
  caps: Caps,
  initialArgument: t.Node | undefined,
): void {
  if (items.length <= caps.listCap) {
    return;
  }

  const error = aotError(
    `AOT: list state "${stateName}" starts with ${items.length} rows, but a list holds ${caps.listCap}`,
    'raise ER_AOT_LIST_CAP (the listCap option), or start with fewer rows.',
  );
  if (initialArgument?.loc) {
    error.aotLoc = initialArgument.loc.start;
  }
  throw error;
}

/**
 * A state's initial value, when it is a number or a boolean derived from `screen`. A string stays folded: a closed
 * set of strings is an interned enum, and there are only as many layouts as strings.
 *
 * @param initialArgument  The initial value.
 * @param scope  Compile-time constants it folds against.
 *
 * @returns The boot value, or null.
 */
function bootNumberOf(initialArgument: t.Node, scope: Scope): BootValue | null {
  let value: unknown;
  try {
    value = evalStatic(initialArgument, scope, 'boot');
  } catch {
    return null;
  }

  if (!isBoot(value) || !value.lowerable) return null;
  const built = buildValueOf(value);
  return typeof built === 'number' || typeof built === 'boolean' ? value : null;
}

/**
 * Settles each initial value derived from `screen`, once slots can be lowered: boot works it out, typed by its
 * expression so that every panel gets the same slot (`screen.width / 2` is a fraction on every panel, even where it
 * comes out whole). One boot can't work out is taken at the build's panel.
 *
 * @param states  The states.
 */
export function settleBootInitials(states: Iterable<StateRecord>): void {
  for (const state of states) {
    if (state.kind !== 'scalar' || !state.bootValue) continue;
    const slot = bootSlotOf(state.bootValue);
    if (slot?.type.kind === 'float') {
      state.slotKind = 'float';
      state.isBoolean = false;
    } else if (slot?.type.kind === 'int' || slot?.type.kind === 'bool') {
      state.slotKind = state.widened ? 'i64' : 'int';
      state.isBoolean = slot.type.kind === 'bool';
    }

    if (slot) {
      state.bootInitial = slot;
    } else {
      foldBoot(state.bootValue);
    }
    delete state.bootValue;
  }
}

/**
 * A read of a scalar state, as a local that stands for it.
 *
 * @param state  The state.
 *
 * @returns The local.
 */
export const stateLocal = (state: ScalarState): Local => ({
  slotKind: state.slotKind,
  isBoolean: state.isBoolean,
  ...(state.widened === 'number' ? {isLiteral: true} : {}),
  source: {kind: 'state', state, location: undefined},
  expression: {kind: 'state', state, type: stateType(state)},
});

/**
 * A scalar state's slot, its kind picked from its initial value.
 *
 * @param name  The JS name.
 * @param slotName  The storage name.
 * @param setter  The setter's JS name.
 * @param initialValue  The folded initial value.
 * @param initialNode  What it was written as, if anything: `70.0` makes a float slot.
 * @param isHost  It is a useHostValue.
 * @param bootValue  An initial value derived from `screen`.
 * @param wide  Slots to declare 64-bit (see compileWidened).
 *
 * @returns The state.
 */
function scalarStateOf(
  name: string,
  slotName: string,
  setter: string | undefined,
  initialValue: unknown,
  initialNode: t.Node | undefined,
  isHost: boolean,
  bootValue: BootValue | null,
  wide: SlotWidths,
): ScalarState {
  // Pick the slot's kind from the initial value.
  let slotKind: SlotKind = slotKindOfValue(initialValue);
  // Flag booleans: slotKindOfValue makes them int, but text lowering must render false differently from 0.
  const isBoolean = typeof initialValue === 'boolean';
  // A literal written like 70.0 forces a float slot so the state can hold fractions (a smooth drag).
  if (
    slotKind === 'int' &&
    initialNode?.type === 'NumericLiteral' &&
    typeof initialNode.extra?.raw === 'string' &&
    /[.eE]/.test(initialNode.extra!.raw as string)
  ) {
    slotKind = 'float';
  }

  // A whole number past the int range starts it 64-bit (past 64 bits, a float), as does a setter that
  // stores a timestamp or such a number in it (see compileWidened).
  const initialWidth = slotKind === 'int' ? wideInitial(initialValue) : null;
  if (initialWidth === 'float') {
    slotKind = 'float';
  }

  const widened =
    slotKind === 'int' && !isBoolean
      ? (wide.get(slotName) ??
        (initialWidth === 'number' ? 'number' : undefined))
      : undefined;
  if (widened) {
    slotKind = 'i64';
  }

  // String scalar → a fixed buffer; setters format into it (see scalarAssign).
  const initial =
    slotKind === 'string' ? String(initialValue) : Number(initialValue);
  if (slotKind === 'float') {
    checkNumber(initial);
  } else if (slotKind === 'i64') {
    BigInt(initial as number); // refuses a value that is not a whole number
  }
  return {
    name,
    slotName,
    setter,
    kind: 'scalar',
    slotKind,
    isBoolean,
    initial,
    host: isHost, // host-fed → also an input the host sets
    initialAt: initialNode?.loc?.start,
    ...(widened ? {widened} : {}),
    ...(bootValue ? {bootValue} : {}),
  };
}

/**
 * What an object literal holds at a key, when it is written as a plain field.
 *
 * @param node  The object.
 * @param key  The key.
 *
 * @returns The field's value, or undefined.
 */
function fieldNodeOf(
  node: t.Node | undefined,
  key: string,
): t.Node | undefined {
  if (node?.type !== 'ObjectExpression') return undefined;
  for (const property of [...node.properties].reverse()) {
    if (property.type !== 'ObjectProperty' || property.computed) continue;
    const name =
      property.key.type === 'Identifier'
        ? property.key.name
        : property.key.type === 'StringLiteral'
          ? property.key.value
          : null;
    if (name === key) return property.value;
  }
  return undefined;
}

/**
 * An object state: one scalar state per field, nested objects flattened to dotted paths.
 *
 * @param name  The JS name.
 * @param prefix  The storage prefix.
 * @param setter  The setter's JS name.
 * @param value  The folded initial value.
 * @param initialNode  What it was written as.
 * @param wide  Slots to declare 64-bit (see compileWidened).
 *
 * @returns The record.
 */
function recordStateOf(
  name: string,
  prefix: string,
  setter: string | undefined,
  value: Record<string, unknown>,
  initialNode: t.Node,
  wide: SlotWidths,
): RecordState {
  const fields = new Map<string, ScalarState>();
  const fail = (message: string, hint: string, at: t.Node | undefined) => {
    const error = aotError(message, hint);
    const location = (at ?? initialNode).loc;
    if (location) error.aotLoc = location.start;
    return error;
  };
  const flatten = (
    object: Record<string, unknown>,
    path: string[],
    node: t.Node | undefined,
  ): Record<string, unknown> => {
    const entries = Object.entries(object);
    if (!entries.length) {
      throw fail(
        `AOT: state "${[name, ...path].join('.')}" is an empty object`,
        'give it the fields it will hold, each with a starting value.',
        node,
      );
    }
    const folded: Record<string, unknown> = {};
    for (const [key, fieldValue] of entries) {
      const fieldPath = [...path, key];
      const fieldName = [name, ...fieldPath].join('.');
      const fieldNode = fieldNodeOf(node, key);
      // Fields are keyed by their dotted path, so a key can't hold a dot.
      if (key === '' || key.includes('.')) {
        throw fail(
          `AOT: state "${[name, ...path].join('.')}" has a field named "${key}"`,
          'name each field with letters, digits or `_`.',
          fieldNode,
        );
      }
      if (isObjectTree(fieldValue)) {
        folded[key] = flatten(fieldValue, fieldPath, fieldNode);
        continue;
      }
      if (Array.isArray(fieldValue)) {
        throw fail(
          `AOT: state "${fieldName}" is an array inside an object state`,
          'keep the list in a useState of its own.',
          fieldNode,
        );
      }
      if (
        fieldValue == null ||
        (typeof fieldValue === 'number' && !Number.isFinite(fieldValue)) ||
        !['number', 'string', 'boolean'].includes(typeof fieldValue)
      ) {
        throw fail(
          `AOT: the initial value of state "${fieldName}" is ${typeof fieldValue === 'function' ? 'a function' : String(fieldValue)}`,
          "give each field a concrete starting value: 0, '' or false.",
          fieldNode,
        );
      }
      const state = scalarStateOf(
        fieldName,
        prefix + fieldName,
        undefined,
        fieldValue,
        fieldNode,
        false,
        null,
        wide,
      );
      state.initialAt ??= initialNode.loc?.start;
      fields.set(fieldPath.join('.'), state);
      folded[key] = new RuntimeLeaf(stateLocal(state));
    }
    return folded;
  };
  const folded = flatten(value, [], initialNode);
  return {kind: 'record', name, setter, fields, value: folded};
}

/**
 * Collects a component's useState declarations → state descriptors keyed by JS name (reads) and setter
 * name (writes). `prefix` namespaces the STORAGE so each inlined child instance gets its own slots: the
 * lookup keys stay the bare JS names (`count`), but the field / array / count derive from `slotName`
 * (`<prefix>count`). prefix='' (the App) keeps the storage names exactly the JS names.
 * `caps` are the compiler's resolved caps (see resolveOptions); a list state takes its capacity from it.
 *
 * @param functionBody  The component's body.
 * @param scope  Compile-time constants the initial values fold against.
 * @param caps  The compile's caps.
 * @param prefix  The storage prefix: none for the App, `c<n>_` for an inlined instance.
 * @param wide  Slots to declare 64-bit (see compileWidened).
 *
 * @returns The component's state table.
 */
export function collectState(
  functionBody: t.BlockStatement,
  scope: Scope,
  caps: Caps,
  prefix = '',
  wide: SlotWidths = NO_WIDE,
): StateTable {
  // Index each state by its JS name and, when it has one, by its setter's name.
  const byName = new Map<string, StateRecord>();
  const bySetter = new Map<string, StateRecord | RecordState>();

  // Scan the body's top-level declarations for useState and useHostValue calls.
  for (const statement of functionBody.body) {
    if (statement.type !== 'VariableDeclaration') continue;

    // Inspect each declaration in the statement so multiple hook bindings in one `const` are collected independently.
    for (const declarator of statement.declarations) {
      // Only hook-style call declarations can define collected state; plain variables and other initializers are skipped.
      const initializer = declarator.init;
      if (initializer?.type !== 'CallExpression') continue;

      // useHostValue is a host-fed scalar: a state slot like useState, written only by the host through its input.
      const isHost =
        (initializer.callee as t.Identifier).name === 'useHostValue';
      if ((initializer.callee as t.Identifier).name !== 'useState' && !isHost) {
        continue;
      }

      // Accept useHostValue(name) as a direct scalar binding, while useState must keep the usual [value, setter] tuple pattern.
      if (
        isHost
          ? declarator.id.type !== 'Identifier'
          : declarator.id.type !== 'ArrayPattern'
      ) {
        continue;
      }

      // Derive the read name and optional setter from the binding shape: host values bind directly, while useState binds [value, setter].
      const name = isHost
        ? (declarator.id as t.Identifier).name
        : ((declarator.id as t.ArrayPattern).elements[0] as t.Identifier | null)
            ?.name;
      const setter = isHost
        ? undefined
        : ((declarator.id as t.ArrayPattern).elements[1] as t.Identifier | null)
            ?.name;
      if (!name) continue;

      // Build either a list-state record from an array initializer, or a split of a constant, or a scalar record from
      // the folded initial value.
      const slotName = prefix + name; // storage name (== name for the App; instance-unique for a child)
      const initialArgument = initializer.arguments[0];
      let record: StateRecord;
      if (
        !isHost &&
        (initialArgument?.type === 'ArrayExpression' ||
          splitCallOf(initialArgument))
      ) {
        // List state → a fixed-capacity pool of rows + a count.
        const items = evalStaticOrThrow(
          initialArgument,
          scope,
          `AOT: the initial value of list state "${name}" must be a compile-time constant array`,
          'useState([...]) initial must be a literal array of objects, numbers, strings or booleans, or a split ' +
            'of a constant string — no runtime values or other function calls in the initial.',
        ) as unknown[];

        // Infer the item struct, pinning any error to the initial value since this pass adds no location.
        let struct: ItemStruct;
        try {
          struct = inferItemStruct(items, `list state "${name}"`);
        } catch (error) {
          if (initialArgument.loc && !(error as AotError).aotLoc) {
            (error as AotError).aotLoc = initialArgument.loc.start; // collectState isn't withLoc-wrapped
          }
          throw error;
        }
        checkInitialRows(name, items, caps, initialArgument);
        record = {
          name,
          slotName,
          setter,
          kind: 'list',
          struct,
          items,
          capacity: caps.listCap,
          initialAt: initialArgument?.loc?.start,
        };
      } else {
        // Scalar state → one slot. Fold the initial value; no argument starts it at 0. A number derived
        // from `screen` is worked out at boot (see settleBootInitials), so it doesn't take a layout per value.
        const bootValue =
          !isHost && initialArgument
            ? bootNumberOf(initialArgument, scope)
            : null;
        const initialValue = bootValue
          ? buildValueOf(bootValue)
          : initialArgument
            ? evalStaticOrThrow(
                initialArgument,
                scope,
                `AOT: the initial value of state "${name}" must be a compile-time constant`,
                'useState(x) initial must be a literal or a constant expression (number, string, ' +
                  'bool, or arithmetic over consts) — not a runtime value or function call.',
              )
            : 0;

        // Object state → a record, a scalar slot per field, which reads of its name fold through.
        if (!isHost && isObjectTree(initialValue)) {
          const recordState = recordStateOf(
            name,
            prefix,
            setter,
            initialValue,
            initialArgument,
            wide,
          );
          for (const [key, field] of recordState.fields) {
            byName.set(`${name}.${key}`, field);
          }
          if (setter) {
            bySetter.set(setter, recordState);
          }
          setBinding(scope, name, recordState.value);
          continue;
        }

        // A host value must be a finite number; check it directly, since slotKindOfValue maps non-numbers to int.
        if (isHost && !Number.isFinite(initialValue)) {
          throw aotError(
            `AOT: useHostValue("${name}") must be a number — its initial value evaluated to ` +
              `${JSON.stringify(initialValue)}`,
            'useHostValue(0) / useHostValue(0.0) — the host feeds an int or float; booleans, ' +
              'strings, and objects are not supported.',
          );
        }

        // Refuse undefined, null and non-finite initials here; they would fold into nonsense with no location.
        if (
          initialValue === undefined ||
          initialValue === null ||
          (typeof initialValue === 'number' && !Number.isFinite(initialValue))
        ) {
          const error = aotError(
            `AOT: the initial value of state "${name}" is ${initialValue === undefined ? 'undefined' : String(initialValue)}`,
            "give it a concrete starting value: useState(0), useState(''), useState(false).",
          );
          if (initialArgument?.loc) {
            error.aotLoc = initialArgument.loc.start;
          }
          throw error;
        }

        record = scalarStateOf(
          name,
          slotName,
          setter,
          initialValue,
          initialArgument,
          isHost,
          bootValue,
          wide,
        );
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
 * The JSX a component body returns. A top-level `if` whose test folds is decided at compile time, so a
 * responsive layout returns the branch for this build's screen. When the test depends on the panel and both
 * outcomes return elements of one shape, they merge into one element whose values boot picks.
 *
 * @param functionBody  The component's body.
 * @param scope  Compile-time constants the `if` tests fold against.
 *
 * @returns The returned element.
 */
export function findReturnJSX(
  functionBody: t.BlockStatement,
  scope: Scope = {},
): t.JSXElement {
  const statementsOf = (branch: t.Statement): t.Statement[] =>
    branch.type === 'BlockStatement' ? branch.body : [branch];

  // Fold top-level `if (staticCond) return …` at compile time — responsive layouts switch on `screen`.
  const scan = (statements: t.Statement[]): t.JSXElement | null => {
    for (const [index, statement] of statements.entries()) {
      // A top-level `if` must fold; only the branch it takes is scanned.
      if (statement.type === 'IfStatement') {
        let testValue;
        try {
          testValue = evalStatic(statement.test, scope, 'boot');
        } catch {
          throw new Error(
            'AOT: a top-level `if` in the component must have a compile-time-constant test (e.g. ' +
              'on the `screen` global) — runtime layout branching is not supported',
          );
        }

        // A test that depends on the panel needs no variant when both outcomes have one shape.
        if (isBoot(testValue)) {
          const merged = mergeOutcomes(statement, statements.slice(index + 1));
          if (merged) return merged;
          testValue = foldBoot(testValue);
        }

        // Follow only the statically selected branch and keep scanning it until a JSX return is found.
        const branch = testValue ? statement.consequent : statement.alternate;
        if (branch) {
          const branchJSX = scan(statementsOf(branch));
          if (branchJSX) return branchJSX;
        }

        continue;
      }

      // A `return` with a value ends the scan and must return a single JSX element.
      if (statement.type === 'ReturnStatement' && statement.argument) {
        return returned(statement.argument);
      }
    }

    return null;
  };

  // The element a `return` gives: a ternary on the panel is decided as a top-level `if` is.
  const returned = (argument: t.Expression): t.JSXElement => {
    if (argument.type === 'JSXElement') return argument;
    if (argument.type !== 'ConditionalExpression') {
      throw new Error(
        `AOT: the component must return a single JSX element (got ${argument.type})`,
      );
    }

    let testValue;
    try {
      testValue = evalStatic(argument.test, scope, 'boot');
    } catch {
      throw new Error(
        'AOT: a ternary the component returns must have a compile-time-constant test (e.g. on the `screen` ' +
          'global) — runtime layout branching is not supported',
      );
    }

    if (isBoot(testValue)) {
      const restore = bootCheckpoint();
      try {
        const merged = mergeBranchElements(
          argument.test,
          returned(argument.consequent),
          returned(argument.alternate),
        );
        if (merged) return merged;
      } catch {
        /* an outcome that does not compile here is left to the fold, which reports it if it is taken */
      }
      restore();
      testValue = foldBoot(testValue);
    }

    return returned(testValue ? argument.consequent : argument.alternate);
  };

  // What each outcome of an `if` returns (a branch that does not return falls through), merged when it can be.
  const mergeOutcomes = (
    statement: t.IfStatement,
    rest: t.Statement[],
  ): t.JSXElement | null => {
    const restore = bootCheckpoint();
    try {
      const whenTrue = scan(statementsOf(statement.consequent)) ?? scan(rest);
      const whenFalse =
        (statement.alternate && scan(statementsOf(statement.alternate))) ??
        scan(rest);
      const merged =
        whenTrue && whenFalse
          ? mergeBranchElements(statement.test, whenTrue, whenFalse)
          : null;
      if (merged) return merged;
    } catch {
      /* an outcome that does not compile here is left to the fold, which reports it if it is taken */
    }

    restore();
    return null;
  };

  // Scan the body; a component that never returns is an error.
  const returnJSX = scan(functionBody.body);
  if (!returnJSX) {
    throw new Error('AOT: component has no return statement');
  }

  return returnJSX;
}

/**
 * Returns the JSX a function component returns (arrow expression body or a block's return).
 *
 * @param functionNode  The component.
 * @param scope  Compile-time constants its top-level `if` tests fold against.
 *
 * @returns The returned element.
 */
export function componentReturnJSX(
  functionNode: FunctionNode,
  scope: Scope = {},
): t.JSXElement {
  if (functionNode.body.type === 'JSXElement') return functionNode.body;
  if (functionNode.body.type === 'BlockStatement')
    return findReturnJSX(functionNode.body, scope);
  throw new Error('AOT: component body must return a JSX element');
}

/**
 * Whether a function returns JSX (a component) rather than a value (a helper).
 *
 * @param functionNode  The function.
 *
 * @returns Whether its body is JSX or has a top-level `return <…/>`.
 */
const fnReturnsJSX = (functionNode: FunctionNode): boolean =>
  functionNode.body.type === 'JSXElement' ||
  (functionNode.body.type === 'BlockStatement' &&
    functionNode.body.body.some(
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
  for (const statement of program.body) {
    const declaration =
      statement.type === 'ExportNamedDeclaration'
        ? statement.declaration
        : statement;
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
      for (const declarator of declaration.declarations) {
        if (
          declarator.id?.type !== 'Identifier' ||
          declarator.id.name === 'App'
        )
          continue;
        const functionNode = asComponentFn(declarator.init); // unwrap memo(...)
        if (functionNode && fnReturnsJSX(functionNode)) {
          components.set(declarator.id.name, functionNode);
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
    functionNode: t.Node | null | undefined,
  ): void => {
    if (
      name &&
      name !== 'App' &&
      isFn(functionNode) &&
      !fnReturnsJSX(functionNode)
    ) {
      helpers.set(name, functionNode);
    }
  };

  // Module-level function declarations and function-valued variables.
  for (const statement of program.body) {
    const declaration =
      statement.type === 'ExportNamedDeclaration'
        ? statement.declaration
        : statement;
    if (!declaration) continue;
    if (declaration.type === 'FunctionDeclaration' && declaration.id) {
      addHelper(declaration.id.name, declaration);
    }
    if (declaration.type === 'VariableDeclaration') {
      for (const declarator of declaration.declarations) {
        if (declarator.id?.type === 'Identifier') {
          addHelper(declarator.id.name, declarator.init);
        }
      }
    }
  }

  // The component's own top-level arrows and functions (a hook call is not a function, so it is skipped).
  if (componentBody.type === 'BlockStatement') {
    for (const statement of componentBody.body) {
      if (statement.type !== 'VariableDeclaration') continue;
      for (const declarator of statement.declarations) {
        if (declarator.id?.type === 'Identifier') {
          addHelper(declarator.id.name, declarator.init);
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
 * esbuild asset plugin so the same `import → basename` convention holds in both flows. Two different files of
 * one name would be one asset, so they are refused.
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
  const pathOf = new Map<string, string>(); // name → the file it names
  const normalized = (path: string): string =>
    posix.normalize(path.replace(/\\/g, '/'));
  for (const statement of program.body) {
    if (
      statement.type !== 'ImportDeclaration' ||
      typeof statement.source.value !== 'string'
    ) {
      continue;
    }

    const importPath = statement.source.value;
    if (!IMAGE_EXT_RE.test(importPath)) continue;

    const name = importPath.split(/[\\/]/).pop()!.replace(IMAGE_EXT_RE, '');
    const first = pathOf.get(name);
    if (first !== undefined && normalized(first) !== normalized(importPath)) {
      const error = aotError(
        `AOT: two images are named "${name}" (${first} and ${importPath})`,
        'an image is named by its file name, so rename one of the files.',
      );
      locateError(error, statement.loc?.start);
      throw error;
    }
    pathOf.set(name, importPath);
    for (const specifier of statement.specifiers) {
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
 * passes it in as options.svgArtifacts, keyed by `name`. emitSvgSource resolves the local → name → artifact.
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
  for (const statement of program.body) {
    if (
      statement.type !== 'ImportDeclaration' ||
      typeof statement.source.value !== 'string'
    ) {
      continue;
    }

    const importPath = statement.source.value;
    if (!/\.svg$/i.test(importPath)) continue;

    const name = importPath
      .split(/[\\/]/)
      .pop()!
      .replace(/\.svg$/i, '');
    for (const specifier of statement.specifiers) {
      if (specifier.type === 'ImportDefaultSpecifier') {
        byLocal.set(specifier.local.name, {name, importPath});
      }
    }
  }

  return byLocal;
}

/**
 * Bakes a Flow B app's <Svg source> imports → { assetName: artifact } via the SAME baker Flow A's esbuild
 * .svg loader uses (svgToVector). This is the I/O half (reads .svg files) kept OUT of the pure compilePackage;
 * both CLI entries (aot/compile.mts and the consumer cli.mjs) call it and hand the result to options.svgArtifacts.
 *
 * @param source  App.jsx source text.
 * @param baseDirectory  Directory the import paths are resolved against (the app's dir).
 *
 * @returns Each import's artifact, by asset name: its op-tape, or a PNG for one the vector baker cannot draw.
 */
export async function bakeSvgArtifacts(
  source: string,
  baseDirectory: string,
): Promise<Record<string, SvgArtifact>> {
  // Parse with the TS plugin too (harmless for plain JSX), so a .tsx app's <Svg source> imports are found.
  const program = parse(source, {
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
    const svgPath = resolve(baseDirectory, svgImport.importPath);
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
          `rasterising it as a fallback image (Flow B bakes the PNG into the package). Simplify the SVG ` +
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
 * the AOT re-renders via its own dependency tracking, so useCallback only names a shared handler.
 *
 * @param functionBody  The component's body.
 *
 * @returns The callbacks, by name.
 */
export function collectCallbacks(
  functionBody: t.BlockStatement | t.Expression,
): Map<string, FunctionNode> {
  const callbacks = new Map<string, FunctionNode>();
  if (functionBody.type !== 'BlockStatement') return callbacks;

  // Collect top-level useCallback bindings whose first argument is an inline function, keyed by the variable name.
  for (const statement of functionBody.body) {
    if (statement.type !== 'VariableDeclaration') continue;
    for (const declarator of statement.declarations) {
      const initializer = declarator.init;
      if (
        initializer?.type === 'CallExpression' &&
        (initializer.callee as t.Identifier).name === 'useCallback' &&
        declarator.id.type === 'Identifier' &&
        isFn(initializer.arguments[0])
      ) {
        callbacks.set(declarator.id.name, initializer.arguments[0]);
      }
    }
  }

  return callbacks;
}

/**
 * Collects `const m = useMemo(() => expr, deps)` → Map (name → the memo's expression node).
 *
 * @param functionBody  The component's body.
 *
 * @returns Each memo's expression, by name.
 */
export function collectMemos(
  functionBody: t.BlockStatement | t.Expression,
): Map<string, t.Expression> {
  const memos = new Map<string, t.Expression>();
  if (functionBody.type !== 'BlockStatement') return memos;

  // Collect expression-bodied useMemo bindings; block-bodied memos are rejected until statement-bodied folding is supported.
  for (const statement of functionBody.body) {
    if (statement.type !== 'VariableDeclaration') continue;
    for (const declarator of statement.declarations) {
      const initializer = declarator.init;
      if (
        initializer?.type === 'CallExpression' &&
        (initializer.callee as t.Identifier).name === 'useMemo' &&
        declarator.id.type === 'Identifier' &&
        isFn(initializer.arguments[0])
      ) {
        const body = initializer.arguments[0].body;
        if (body.type === 'BlockStatement') {
          throw new Error(
            `AOT: useMemo for "${declarator.id.name}" must be a single expression (for now)`,
          );
        }
        memos.set(declarator.id.name, body);
      }
    }
  }

  return memos;
}

/**
 * Collects `useEffect(() => {…}, deps)` calls → { fn, deps, node }. Dep validity is checked at compile time.
 *
 * @param functionBody  The component's body.
 *
 * @returns The effects, in source order.
 */
export function collectEffects(
  functionBody: t.BlockStatement | t.Expression,
): EffectRecord[] {
  const effects: EffectRecord[] = [];
  if (functionBody.type !== 'BlockStatement') return effects;

  // Collect top-level useEffect calls, requiring an inline function so the effect body can be lowered later with its deps.
  for (const statement of functionBody.body) {
    if (statement.type !== 'ExpressionStatement') continue;
    const call = statement.expression;
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
      // React runs an effect with no deps after every render, which Flow B has no lowering for.
      if (call.arguments.length < 2) {
        const error = aotError(
          'AOT: useEffect needs a dependency list',
          'pass `[]` (run once) or `[a, b]` (re-run when a/b change).',
        );
        if (call.loc) error.aotLoc = call.loc.start;
        throw error;
      }
      effects.push({
        functionNode: call.arguments[0],
        dependencies: call.arguments[1],
        node: call,
      });
    }
  }

  return effects;
}

/**
 * True if a function component declares any useState (per-instance child state — not yet supported).
 *
 * @param functionNode  The component.
 *
 * @returns Whether its body calls useState.
 */
export function usesState(functionNode: FunctionNode): boolean {
  if (functionNode.body.type !== 'BlockStatement') return false;

  return functionNode.body.body.some(
    statement =>
      statement.type === 'VariableDeclaration' &&
      statement.declarations.some(
        declarator =>
          declarator.init?.type === 'CallExpression' &&
          (declarator.init.callee as t.Identifier).name === 'useState',
      ),
  );
}
