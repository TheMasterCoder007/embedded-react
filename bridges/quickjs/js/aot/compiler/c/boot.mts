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

import {cstr} from '../c-syntax.mts';
import {doubleLit, printExpr, printInDoubles} from './expressions.mts';
import type {IrBoot, IrBootAssumption, IrBootSlot} from '../ir/boot.mts';
import type {Screen} from '../options.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * How far a fraction a guard reads may stray from the build's, relative to its size. Boot works in doubles as JS
 * does, so only a libm function's last digit can differ; any other panel's value is far outside this.
 */
const FLOAT_TOLERANCE = 1e-9;

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Whether an app works anything out at boot or assumes anything of its panel, so it has an app_boot().
 *
 * @param boot  The app's boot values.
 *
 * @returns True when it does.
 */
export const hasBoot = (boot: IrBoot): boolean =>
  boot.slots.length > 0 || boot.assumptions.length > 0;

/**
 * A slot's C declaration in the ErAppBoot struct. A fraction is a double, as JS holds it.
 *
 * @param slot  The slot.
 *
 * @returns The C line.
 */
function printSlotDecl(slot: IrBootSlot): string {
  const cType =
    slot.type.kind === 'float'
      ? 'double'
      : slot.type.kind === 'string'
        ? 'const char*'
        : 'int';
  return `    ${cType} ${slot.field};`;
}

/**
 * The file-scope struct of the values er_app_build() works out at boot.
 *
 * @param boot  The app's boot values.
 *
 * @returns The C, or '' when there are none.
 */
export const printBootDecls = (boot: IrBoot): string =>
  boot.slots.length
    ? `/* The values that depend on the panel size, worked out once by er_app_build(). */\n` +
      `typedef struct\n{\n${boot.slots.map(printSlotDecl).join('\n')}\n} ErAppBoot;\n\n` +
      `static ErAppBoot s_boot;\n`
    : '';

/**
 * The C test that a panel meets one assumption.
 *
 * @param assumption  The assumption.
 * @param screen  The panel the build was compiled for.
 *
 * @returns The C condition.
 */
function printAssumption(assumption: IrBootAssumption, screen: Screen): string {
  if (assumption.kind === 'panel') {
    return `(screen_w == ${screen.width} && screen_h == ${screen.height})`;
  }

  const read = `s_boot.${assumption.slot.field}`;
  const {value} = assumption;
  if (typeof value === 'string')
    return `(strcmp(${read}, ${cstr(value)}) == 0)`;
  if (typeof value === 'boolean') return value ? read : `!${read}`;
  if (assumption.slot.type.kind !== 'float') {
    return `(${read} == ${value})`;
  }

  // A fraction within a small margin: boot and the compiler need not round its last digit alike.
  const margin = doubleLit(FLOAT_TOLERANCE * (1 + Math.abs(value)));
  const expected = doubleLit(value);
  return `(${read} - ${expected} <= ${margin} && ${expected} - ${read} <= ${margin})`;
}

/**
 * app_boot(): sets each boot value from the panel size, then reports whether the panel meets everything the
 * build assumed of it.
 *
 * @param boot  The app's boot values.
 * @param screen  The panel the build was compiled for.
 *
 * @returns The C function, or '' when the app has no boot values.
 */
export function printBootFunction(boot: IrBoot, screen: Screen): string {
  if (!hasBoot(boot)) return '';

  const lines = boot.slots.map(({field, init}) => {
    const value =
      init.kind === 'expr'
        ? printInDoubles(() => printExpr(init.expr))
        : init.dimension === 'width'
          ? 'screen_w'
          : 'screen_h';
    return `    s_boot.${field} = ${value};`;
  });
  const body = lines.join('\n');
  const reads = (name: string) => new RegExp(`\\b${name}\\b`).test(body);
  const guards = boot.assumptions.map(assumption =>
    printAssumption(assumption, screen),
  );
  const unused = ['screen_w', 'screen_h']
    .filter(name => !reads(name) && !guards.some(guard => guard.includes(name)))
    .map(name => `    (void)${name};`);
  const verdict = guards.length
    ? `    return ${guards.join('\n        && ')};`
    : '    return true;';
  return (
    '/* Works out the values that depend on the panel size, and whether this panel takes the layout the app\n' +
    '   was compiled for: what the compiler folded at its panel has to come out the same here. */\n' +
    'static bool app_boot(int screen_w, int screen_h)\n{\n' +
    [...unused, ...lines, verdict].join('\n') +
    '\n}'
  );
}
