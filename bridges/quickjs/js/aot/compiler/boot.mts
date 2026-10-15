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

import {attemptLowering} from './check.mts';
import {INT} from './ir/types.mts';
import type * as t from '@babel/types';
import type {
  IrBoot,
  IrBootAssumption,
  IrBootSlot,
  ScreenDimension,
} from './ir/boot.mts';
import type {IrExpr} from './ir/expressions.mts';
import type {Env, Scope} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** Lowers a JS expression to the IR (expressions.mts's lowerExpr, passed in to keep the imports one-way). */
type LowerExpr = (node: t.Node, env: Env) => IrExpr;

/** What one compile has found out about its boot values. */
interface BootContext {
  /** The app's environment, which a slot's expression lowers against, once the app has one. */
  env: Env | null;
  lower: LowerExpr | null;
  /** Every slot, each after the slots its expression reads. */
  slots: IrBootSlot[];
  /** The values a constant fold read at the build's panel. */
  assumed: Set<BootValue>;
  /** The expressions whose slots are being lowered; their parts are not slots of their own. */
  lowering: Set<t.Node>;
  /** The boot value each expression read as a whole stands for, by the scope it was read in. */
  wholes: Map<t.Node, Map<Scope, BootValue>>;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** The kinds of value a boot slot holds. */
const SLOT_KINDS = new Set(['bool', 'int', 'float', 'string']);

/** The current compile's boot values, or null outside a compile. */
let context: BootContext | null = null;

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * A value known at boot: `screen.width` / `screen.height`, or an expression derived only from them and
 * constants. It carries the JS expression that computes it, the names that expression reads, and what it is
 * on the panel the build was compiled for, which a site that needs a compile-time constant folds instead.
 */
export class BootValue {
  /** The expression, or null for a panel dimension. */
  readonly node: t.Expression | null;
  /** The names the expression reads. */
  readonly scope: Scope;
  /** What it is on the build's panel. */
  readonly value: unknown;
  /** Boot can work it out from its expression. */
  readonly lowerable: boolean;
  /** What its slot is named after, once it is bound to a name. */
  name: string | null;
  /** The panel dimension it is, if it is one. */
  readonly dimension: ScreenDimension | null;
  /** What a fold of it assumes when boot cannot work it out: the test that picked it. */
  readonly basis: BootValue[];
  /** A value assuming this one is the same as assuming, as for `!flag` and `flag`. */
  equivalent: BootValue | null = null;
  /** The slot boot reads it from, once looked up; null when boot cannot work it out. */
  slot: IrBootSlot | null | undefined = undefined;

  /**
   * @param node  The expression, or null for a panel dimension.
   * @param scope  The names the expression reads.
   * @param value  What it is on the build's panel.
   * @param lowerable  Boot can work it out from its expression.
   * @param name  What its slot is named after, if it is bound to a name yet.
   * @param dimension  The panel dimension it is, if it is one.
   * @param basis  What a fold of it assumes when boot cannot work it out.
   */
  constructor(
    node: t.Expression | null,
    scope: Scope,
    value: unknown,
    lowerable: boolean,
    name: string | null = null,
    dimension: ScreenDimension | null = null,
    basis: BootValue[] = [],
  ) {
    this.node = node;
    this.scope = scope;
    this.value = value;
    this.lowerable = lowerable;
    this.name = name;
    this.dimension = dimension;
    this.basis = basis;
  }
}

/** Thrown by a strict fold that read a boot value, so the caller takes its runtime path instead. */
export class BootReadError extends Error {
  constructor() {
    super('AOT: the value depends on the panel size');
  }
}

/**
 * Whether a value is a boot value.
 *
 * @param value  The value.
 *
 * @returns True for a BootValue.
 */
export const isBoot = (value: unknown): value is BootValue =>
  value instanceof BootValue;

/**
 * Whether a value is an object or array a fold built, which may hold boot values (an AST node never is).
 *
 * @param value  The value.
 *
 * @returns True for a plain object or an array.
 */
export const isContainer = (value: unknown): value is object =>
  Array.isArray(value) ||
  (typeof value === 'object' &&
    value !== null &&
    Object.getPrototypeOf(value) === Object.prototype);

/**
 * What a value is on the build's panel, one level down: a boot value's own value, anything else itself.
 *
 * @param value  The value.
 *
 * @returns The value.
 */
export function buildValueOf(value: unknown): unknown {
  let current = value;
  while (isBoot(current)) {
    current = current.value;
  }

  return current;
}

/**
 * Whether a value is, or holds, a boot value.
 *
 * @param value  The value.
 *
 * @returns True when it does.
 */
export function containsBoot(value: unknown): boolean {
  if (isBoot(value)) return true;
  if (!isContainer(value)) return false;
  return Object.values(value).some(containsBoot);
}

/**
 * A value with every boot value in it replaced by what it is on the build's panel, each one recorded as an
 * assumption. A container that holds none is returned as is.
 *
 * @param value  The value.
 *
 * @returns The value as the build's panel gives it.
 */
export function foldBoot(value: unknown): unknown {
  if (isBoot(value)) {
    context?.assumed.add(value.equivalent ?? value);
    return foldBoot(value.value);
  }

  if (!isContainer(value) || !containsBoot(value)) return value;
  if (Array.isArray(value)) return value.map(foldBoot);
  return Object.fromEntries(
    Object.entries(value).map(([key, entry]) => [key, foldBoot(entry)]),
  );
}

/**
 * Names the boot values a const binds that are not named yet: the value itself, or each entry of an object
 * or array after its key, so their slots read like the source.
 *
 * @param value  The const's value.
 * @param name  The const's name.
 */
export function nameBootValues(value: unknown, name: string): void {
  if (isBoot(value)) {
    value.name ??= name;
  } else if (isContainer(value) && containsBoot(value)) {
    for (const [key, entry] of Object.entries(value)) {
      nameBootValues(entry, `${name}_${key}`);
    }
  }
}

/**
 * The `screen` global: its width and height are boot values, set from er_app_build()'s arguments.
 *
 * @param screen  The panel the build is compiled for.
 * @param screen.width  Its width.
 * @param screen.height  Its height.
 *
 * @returns The binding.
 */
export const screenBinding = (screen: {
  width: number;
  height: number;
}): {width: BootValue; height: BootValue} => ({
  width: new BootValue(null, {}, screen.width, true, null, 'width'),
  height: new BootValue(null, {}, screen.height, true, null, 'height'),
});

/**
 * Starts recording a compile's boot values.
 */
export function beginBoot(): void {
  context = {
    env: null,
    lower: null,
    slots: [],
    assumed: new Set(),
    lowering: new Set(),
    wholes: new Map(),
  };
}

/**
 * Gives the compile's boot values the environment and lowering their slots are worked out with.
 *
 * @param env  The app's environment.
 * @param lower  The expression lowering.
 */
export function setBootLowering(env: Env, lower: LowerExpr): void {
  if (context) {
    context.env = env;
    context.lower = lower;
  }
}

/**
 * Ends a compile's boot values: what the compile's slots are, and what its folds assumed of the panel.
 *
 * @returns The app's boot values.
 */
export function endBoot(): IrBoot {
  const ending = context;
  if (!ending) return {slots: [], assumptions: []};

  // Look each assumed value's slot up while the context still lowers, so the guard can read it.
  const assumptions: IrBootAssumption[] = [];
  const guarded = new Set<IrBootSlot>();
  let assumesPanel = false;
  const assume = (value: BootValue): void => {
    const slot = bootSlotOf(value);
    const buildValue = buildValueOf(value);
    if (slot && isGuardable(buildValue)) {
      if (!guarded.has(slot)) {
        guarded.add(slot);
        assumptions.push({kind: 'value', slot, value: buildValue});
      }
    } else if (value.basis.length) {
      value.basis.forEach(assume);
    } else {
      assumesPanel = true;
    }
  };
  ending.assumed.forEach(assume);
  if (assumesPanel) {
    assumptions.push({kind: 'panel'});
  }

  context = null;
  return {slots: ending.slots, assumptions};
}

/**
 * Drops a compile's boot values after it failed.
 */
export function abandonBoot(): void {
  context = null;
}

/**
 * Whether a guard can compare a slot against a value.
 *
 * @param value  The value on the build's panel.
 *
 * @returns True for a finite number, a string, or a boolean.
 */
const isGuardable = (value: unknown): value is number | string | boolean =>
  typeof value === 'string' ||
  typeof value === 'boolean' ||
  (typeof value === 'number' && Number.isFinite(value));

/**
 * A C identifier for a slot named after `name`, unique among the compile's slots.
 *
 * @param name  The name.
 *
 * @returns The field.
 */
function slotField(name: string): string {
  const base = name.replace(/[^A-Za-z0-9_]/g, '_').replace(/^(?=\d)/, '_');
  const taken = new Set(context!.slots.map(slot => slot.field));
  let field = base || 'value';
  for (let suffix = 2; taken.has(field); suffix++) {
    field = `${base}_${suffix}`;
  }

  return field;
}

/**
 * Whether two lowered expressions compute the same value the same way (they read slots by identity).
 *
 * @param first  One expression.
 * @param second  The other.
 *
 * @returns True when they do.
 */
function sameExpr(first: unknown, second: unknown): boolean {
  if (first === second) return true;
  if (
    typeof first !== 'object' ||
    typeof second !== 'object' ||
    first === null ||
    second === null ||
    Array.isArray(first) !== Array.isArray(second)
  ) {
    return false;
  }

  const firstKeys = Object.keys(first);
  return (
    firstKeys.length === Object.keys(second).length &&
    firstKeys.every(key =>
      key === 'slot'
        ? (first as Record<string, unknown>)[key] ===
          (second as Record<string, unknown>)[key]
        : sameExpr(
            (first as Record<string, unknown>)[key],
            (second as Record<string, unknown>)[key],
          ),
    )
  );
}

/**
 * The environment a slot's expression lowers in: the app's, with no runtime names, and `scope` as its
 * constants.
 *
 * @param env  The app's environment.
 * @param scope  The names the expression reads.
 *
 * @returns The environment.
 */
const bootEnv = (env: Env, scope: Scope): Env => ({
  ...env,
  state: new Map(),
  locals: new Map(),
  consts: scope,
  animations: new Map(),
  refs: new Map(),
  pans: new Map(),
  callbacks: new Map(),
  helpers: new Map(),
  children: null,
  fnProps: undefined,
  cbPrefix: undefined,
  event: null,
  gesture: null,
  pan: null,
  math64: false,
  animLocals: undefined,
});

/**
 * The environment a boot value's own expression lowers in, for a caller that lowers it some other way than
 * as a slot (a style enum, say).
 *
 * @param value  The value.
 *
 * @returns The environment, or null when boot cannot work the value out.
 */
export function bootEnvOf(value: BootValue): Env | null {
  if (!context?.env || !value.lowerable || !value.node) return null;
  return bootEnv(context.env, value.scope);
}

/**
 * The slot boot reads a value from, lowered on first use, after the slots its expression reads. A value boot
 * cannot work out (one only a fold can give, or one read outside a compile) has none.
 *
 * @param value  The value.
 *
 * @returns Its slot, or null.
 */
export function bootSlotOf(value: BootValue): IrBootSlot | null {
  if (value.slot !== undefined) return value.slot;
  if (!context?.env || !context.lower) return null;

  // Null first, so an expression that reads itself has no slot.
  value.slot = null;
  let slot: IrBootSlot;
  if (value.dimension) {
    slot = {
      field: slotField(`screen_${value.dimension}`),
      type: INT,
      init: {kind: 'screen', dimension: value.dimension},
    };
  } else {
    if (!value.lowerable || !value.node) return null;
    const {env, lower} = context;
    let expr: IrExpr;
    const {lowering} = context;
    lowering.add(value.node);
    try {
      expr = attemptLowering(() =>
        lower(value.node!, bootEnv(env, value.scope)),
      );
    } catch {
      return null;
    } finally {
      lowering.delete(value.node);
    }

    if (!SLOT_KINDS.has(expr.type.kind)) return null;
    // A value that only reads another slot shares it, as does one worked out the same way as an earlier slot.
    if (expr.kind === 'boot') {
      value.slot = expr.slot;
      return expr.slot;
    }

    const sameSlot = context.slots.find(
      slot => slot.init.kind === 'expr' && sameExpr(slot.init.expr, expr),
    );
    if (sameSlot) {
      value.slot = sameSlot;
      return sameSlot;
    }

    slot = {
      field: slotField(value.name ?? 'value'),
      type: expr.type,
      init: {kind: 'expr', expr},
    };
  }

  context.slots.push(slot);
  value.slot = slot;
  return slot;
}

/**
 * Marks the assumptions recorded so far, for a lowering attempt whose folds should not stand if it is abandoned.
 *
 * @returns A function that drops every assumption recorded after the mark.
 */
export function bootCheckpoint(): () => void {
  const marked = context;
  const saved = marked ? new Set(marked.assumed) : null;
  return () => {
    if (marked && saved) {
      marked.assumed = saved;
    }
  };
}

/**
 * The boot value an expression read as a whole stands for: the first one found for it in this scope, so every
 * read of the expression shares one slot. Null while a slot is being lowered: its expression is one slot.
 *
 * @param node  The expression.
 * @param scope  The scope it was read in.
 * @param value  The boot value a fold of it gave.
 *
 * @returns The boot value to read, or null.
 */
export function wholeBootValue(
  node: t.Node,
  scope: Scope,
  value: BootValue,
): BootValue | null {
  if (!context || context.lowering.size) return null;
  let byScope = context.wholes.get(node);
  if (!byScope) {
    byScope = new Map();
    context.wholes.set(node, byScope);
  }

  const known = byScope.get(scope);
  if (known) return known;
  byScope.set(scope, value);
  return value;
}

/**
 * Records that a fold read a boot value at the build's panel.
 *
 * @param value  The value.
 */
export function assumeBoot(value: BootValue): void {
  context?.assumed.add(value);
}
