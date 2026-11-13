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

/*
 * What lowering holds in place of a typed value. A number's width is not known until every store into every slot
 * has been lowered, so lowering leaves each expression untyped and the typing pass fills it in, in place. Until
 * then lowering can ask only for a value's shape, which no width changes.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {defersTyping, whenTyped} from './check.mts';
import type {IrExpr} from './ir/expressions.mts';
import type {IrSource} from './ir/source.mts';
import type {IrType} from './ir/types.mts';
import type {Local, ScalarState, SlotValue} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** What a value is, whatever width the typing pass gives a number. */
export type Shape = 'string' | 'bool' | 'number' | 'struct' | 'node';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** What lowering knows of an expression the typing pass has yet to fill in. */
interface Pending {
  shape: Shape;
  /** What it is typed from, when that is one source expression. */
  source?: IrSource;
  /** The expressions it is worked out from, otherwise. */
  from?: IrExpr[];
}

/** Holds what lowering knows of an expression the typing pass has yet to fill in. */
const PENDING = Symbol('pending');

/** What a slot records about its value, which the typing pass decides for a number. */
const SLOT_FACETS = ['slotKind', 'isBoolean', 'isLiteral', 'wide'] as const;

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Makes reading a field fail until the typing pass sets it.
 *
 * @param target  The object.
 * @param field  The field.
 */
function hideUntilTyped(target: object, field: string): void {
  Object.defineProperty(target, field, {
    get() {
      throw new Error(
        `AOT internal: "${field}" was read before the typing pass set it`,
      );
    },
    enumerable: true,
    configurable: true,
  });
}

/**
 * The shape of a value of a type.
 *
 * @param type  The type.
 *
 * @returns The shape.
 */
export function shapeOfType(type: IrType): Shape {
  switch (type.kind) {
    case 'string':
    case 'bool':
    case 'struct':
    case 'node':
      return type.kind;
    default:
      return 'number';
  }
}

/**
 * The shape of an expression's value, typed or not.
 *
 * @param expression  The expression.
 *
 * @returns The shape.
 */
export function shapeOf(expression: IrExpr): Shape {
  const pending = (expression as {[PENDING]?: Pending})[PENDING];
  return pending?.shape ?? shapeOfType(expression.type);
}

/**
 * What an expression is, where that is known before it is typed: a typed expression is itself, and an untyped
 * one whatever its source fixes it as.
 *
 * @param expression  The expression.
 *
 * @returns The expression it is, or null for one the typing pass works out.
 */
export function settledOf(expression: IrExpr): IrExpr | null {
  const pending = (expression as {[PENDING]?: Pending})[PENDING];
  if (!pending) return expression;
  return pending.source?.kind === 'fixed'
    ? settledOf(pending.source.expression)
    : null;
}

/**
 * The state an expression is a bare read of, typed or not.
 *
 * @param expression  The expression.
 *
 * @returns The state, or null.
 */
export function readStateOf(expression: IrExpr): ScalarState | null {
  const pending = (expression as {[PENDING]?: Pending})[PENDING];
  if (pending) {
    return pending.source?.kind === 'state' ? pending.source.state : null;
  }

  return expression.kind === 'state' ? expression.state : null;
}

/**
 * The local an expression is a bare read of, typed or not.
 *
 * @param expression  The expression.
 *
 * @returns The local, or null.
 */
export function readLocalOf(expression: IrExpr): Local | null {
  const pending = (expression as {[PENDING]?: Pending})[PENDING];
  if (pending) {
    return pending.source?.kind === 'local' ? pending.source.local : null;
  }

  return expression.kind === 'local' ? expression.local : null;
}

/**
 * An expression the typing pass works out. Outside a compile it is worked out at once.
 *
 * @param shape  Its value's shape.
 * @param build  Works it out, once every type it reads is known.
 * @param from  What it is typed from: one source expression, or the expressions it is worked out from.
 *
 * @returns The expression, which lowering can place but not read.
 */
export function typedLater(
  shape: Shape,
  build: () => IrExpr,
  from?: IrSource | IrExpr[],
): IrExpr {
  if (!defersTyping()) return build();

  const pending = {} as IrExpr;
  Object.defineProperty(pending, PENDING, {
    value: (Array.isArray(from)
      ? {shape, from}
      : {shape, source: from}) satisfies Pending,
    configurable: true,
  });
  hideUntilTyped(pending, 'kind');
  hideUntilTyped(pending, 'type');
  whenTyped(() => {
    const typed = build();
    const fields = pending as unknown as Record<string | symbol, unknown>;
    delete fields[PENDING];
    delete fields.kind;
    delete fields.type;
    Object.assign(pending, typed);
  });
  return pending;
}

/**
 * The fields a walk over lowered values goes on into. An expression the typing pass has yet to fill in is
 * walked as what it is typed from, which reads at least what it will; a field the pass has yet to set is left
 * out.
 *
 * @param value  The object.
 *
 * @returns Each field's name and value.
 */
export function walkedEntries(value: object): [string, unknown][] {
  const pending = (value as {[PENDING]?: Pending})[PENDING];
  if (pending) {
    return [
      ['typedFrom', pending.source],
      ['workedFrom', pending.from],
    ];
  }

  return Object.entries(Object.getOwnPropertyDescriptors(value)).flatMap(
    ([name, descriptor]): [string, unknown][] =>
      descriptor.enumerable && 'value' in descriptor
        ? [[name, descriptor.value]]
        : [],
  );
}

/**
 * The kind of a lowered value a walk is on, if it has one yet.
 *
 * @param value  The object.
 *
 * @returns Its `kind`, or undefined for an expression the typing pass has yet to fill in.
 */
export const walkedKind = (value: object): unknown =>
  (value as {[PENDING]?: Pending})[PENDING]
    ? undefined
    : (value as {kind?: unknown}).kind;

/**
 * Has the typing pass set fields of a statement or a declaration. Until then they cannot be read, so a copy
 * taken before the pass fails instead of going without them.
 *
 * @param target  The object.
 * @param names  The fields.
 * @param values  Works out the fields; one left undefined is left out.
 */
export function setLater<Target extends object>(
  target: Target,
  names: (keyof Target & string)[],
  values: () => Partial<Target>,
): void {
  if (defersTyping()) {
    names.forEach(name => hideUntilTyped(target, name));
  }
  whenTyped(() => {
    const typed = values();
    for (const name of names) {
      delete target[name];
      if (typed[name] !== undefined) {
        target[name] = typed[name] as Target[typeof name];
      }
    }
  });
}

/**
 * Whether the typing pass has yet to say what a slot holds.
 *
 * @param slot  The slot's record: a local, or a state.
 *
 * @returns True until its kind is set.
 */
export const holdsUntyped = (slot: SlotValue): boolean =>
  Object.getOwnPropertyDescriptor(slot, 'slotKind')?.get !== undefined;

/**
 * The shape of the value a local holds.
 *
 * @param local  The local.
 *
 * @returns The shape.
 */
export function localShape(local: SlotValue): Shape {
  if (holdsUntyped(local)) return 'number';
  if (local.isBoolean) return 'bool';
  if (local.slotKind === 'string') return 'string';
  if (local.slotKind === undefined) return 'struct';
  return (local.slotKind as string) === 'node' ? 'node' : 'number';
}

/**
 * Has a slot record what the typing pass finds its value to be. A string's or a boolean's is known at once.
 *
 * @param target  The slot's record: a local, say.
 * @param shape  The value's shape.
 * @param value  What the slot records about it, once it is typed.
 *
 * @returns The record.
 */
export function holdLater<Target extends object>(
  target: Target,
  shape: Shape,
  value: () => SlotValue,
): Target & SlotValue {
  const held = target as Target & SlotValue;
  const set = (): void => {
    const facets = value();
    for (const facet of SLOT_FACETS) {
      delete (held as Record<string, unknown>)[facet];
      if (facets[facet] !== undefined) {
        (held as Record<string, unknown>)[facet] = facets[facet];
      }
    }
  };
  if (!defersTyping()) {
    set();
    return held;
  }

  if (shape === 'string') {
    held.slotKind = 'string';
  } else if (shape === 'bool') {
    held.slotKind = 'int';
    held.isBoolean = true;
  } else {
    hideUntilTyped(held, 'slotKind');
  }
  whenTyped(set);
  return held;
}

/**
 * Has a slot record what another records, which the typing pass may have yet to find.
 *
 * @param target  The slot's record.
 * @param from  The record it copies.
 *
 * @returns The record.
 */
export function holdSame<Target extends object>(
  target: Target,
  from: SlotValue,
): Target & SlotValue {
  if (holdsUntyped(from)) {
    return holdLater(target, 'number', () => from);
  }

  const held = target as Target & SlotValue;
  for (const facet of SLOT_FACETS) {
    if (from[facet] !== undefined) {
      (held as Record<string, unknown>)[facet] = from[facet];
    }
  }
  return held;
}

/**
 * A local that holds what another holds, which the typing pass may have yet to find.
 *
 * @param from  The local it copies.
 * @param rest  Its own fields.
 *
 * @returns The local.
 */
export function copyLocal(from: Local, rest: Partial<Local>): Local {
  if (!holdsUntyped(from)) {
    return {...from, ...rest};
  }

  const copy = {} as Local;
  for (const key of Object.keys(from) as (keyof Local)[]) {
    if (!(SLOT_FACETS as readonly string[]).includes(key)) {
      (copy as unknown as Record<string, unknown>)[key] = from[key];
    }
  }
  Object.assign(copy, rest);
  return holdLater(copy, 'number', () => from);
}
