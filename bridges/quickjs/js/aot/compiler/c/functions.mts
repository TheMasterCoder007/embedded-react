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

import {cScalarType, floatLit} from '../c-syntax.mts';
import {printChainFunctions, printCompletionWait} from './animations.mts';
import {toCExpr} from './expressions.mts';
import {printPanEvent} from './pan-responder.mts';
import {APP_UPDATE_CALL, printCallback, printStmts} from './statements.mts';
import type {CFunction} from './statements.mts';
import type {
  IrAnimationCallback,
  IrDepEffect,
  IrEffectFunction,
  IrHandler,
  IrMountEffect,
  IrPressFade,
  IrStaticDecl,
  IrTimerFunction,
} from '../ir/functions.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

const INDENT = '    ';

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * The function a dep-driven effect runs in.
 *
 * @param effect  The effect.
 *
 * @returns Its name.
 */
export const effectName = (effect: IrDepEffect): string =>
  `er_effect_${effect.id}`;

/**
 * The flag that says an effect's last run reached its cleanup.
 *
 * @param effectId  The effect's id.
 *
 * @returns The flag's name.
 */
export const armedFlag = (effectId: number): string => `s_eff${effectId}_armed`;

/**
 * The copy an effect keeps of one dep from its last run.
 *
 * @param effect  The effect.
 * @param depIndex  The dep's position in the list.
 *
 * @returns The copy's name.
 */
const depCopy = (effect: IrDepEffect, depIndex: number): string =>
  `s_eff${effect.id}_d${depIndex}`;

/**
 * A press fade as a scoped timing animation of the node's opacity value.
 *
 * @param fade  The fade.
 *
 * @returns The C lines.
 */
const printPressFade = (fade: IrPressFade): string[] => [
  '    {',
  '        ERAnimConfig cfg;',
  '        memset(&cfg, 0, sizeof(cfg));',
  '        cfg.type = ER_ANIM_TIMING;',
  `        cfg.duration_ms = ${fade.durationMs};`,
  '        cfg.easing = ER_EASE_QUAD_IN_OUT;',
  `        er_anim_value_animate(${fade.value}, ${floatLit(fade.opacity)}, &cfg);`,
  '    }',
];

/**
 * An event handler as a C function.
 *
 * @param handler  The handler.
 *
 * @returns The function.
 */
export function printHandler(handler: IrHandler): CFunction {
  switch (handler.kind) {
    case 'callback':
      return {name: handler.name, body: printCallback(handler.callback)};
    case 'pressFade':
      return {
        name: handler.name,
        body: [
          ...printPressFade(handler.fade),
          ...(handler.then
            ? [`    ${handler.then}(node, data, user_data);`]
            : []),
        ],
      };
    case 'panEvent':
      return printPanEvent(handler.pan, handler.event);
  }
}

/**
 * A timer callback as a C function.
 *
 * @param timer  The timer callback.
 *
 * @returns The function.
 */
export const printTimerFunction = (timer: IrTimerFunction): CFunction => ({
  name: timer.name,
  body: printCallback(timer.callback!),
});

/**
 * An animation's on_complete callbacks as C functions.
 *
 * @param callback  The callbacks.
 *
 * @returns The functions.
 */
export function printAnimationCallback(
  callback: IrAnimationCallback,
): CFunction[] {
  switch (callback.kind) {
    case 'completion':
      return [
        {
          name: callback.completion.name,
          body: printCallback(callback.completion),
        },
      ];
    case 'completionWait':
      return [printCompletionWait(callback.completion, callback.wait)];
    case 'chain':
      return printChainFunctions(callback.chain).callbacks;
  }
}

/**
 * A function an effect runs in. A dep-driven effect with a cleanup first runs the last run's cleanup, if that
 * run armed it.
 *
 * @param effectFunction  The function.
 *
 * @returns The C function.
 */
export function printEffectFunction(
  effectFunction: IrEffectFunction,
): CFunction {
  switch (effectFunction.kind) {
    case 'effect': {
      const {effect} = effectFunction;
      const name = effectName(effect);
      const body = printStmts(effect.body, INDENT);
      if (!effect.hasCleanup) {
        return {name, body};
      }

      const armed = armedFlag(effect.id);
      return {
        name,
        body: [
          `    if (${armed})`,
          '    {',
          `        ${armed} = 0;`,
          `        ${name}_cleanup();`,
          '    }',
          ...body,
        ],
      };
    }
    case 'cleanup':
      return {
        name: `${effectName(effectFunction.effect)}_cleanup`,
        body: printStmts(effectFunction.effect.cleanup!, INDENT),
      };
    case 'mount':
      return {
        name: effectFunction.name,
        body: printStmts(effectFunction.body, INDENT),
      };
  }
}

/**
 * The statement that copies one dep of an effect, so the next app_update() can tell whether it changed.
 *
 * @param effect  The effect.
 * @param depIndex  The dep's position in the list.
 *
 * @returns The C statement, without its `;`.
 */
function printDepSnapshot(effect: IrDepEffect, depIndex: number): string {
  const dep = toCExpr(effect.deps[depIndex]);
  const copy = depCopy(effect, depIndex);
  return dep.cType === 'string'
    ? `snprintf(${copy}, sizeof(${copy}), "%s", ${dep.code})`
    : `${copy} = ${dep.code}`;
}

/**
 * What er_app_build() runs for one mount effect.
 *
 * @param mountEffect  The mount effect.
 *
 * @returns The C lines.
 */
export function printMountEffect(mountEffect: IrMountEffect): string[] {
  switch (mountEffect.kind) {
    case 'inline':
      return printCallback(mountEffect.callback);
    case 'call':
      return [
        `    ${mountEffect.name}();`,
        ...(mountEffect.updatesState ? [APP_UPDATE_CALL] : []),
      ];
    case 'depEffect': {
      const {effect} = mountEffect;
      return [
        `    ${effectName(effect)}();`,
        ...effect.deps.map(
          (_, depIndex) => `    ${printDepSnapshot(effect, depIndex)};`,
        ),
      ];
    }
  }
}

/**
 * The app_update() block that copies each dep of an effect that changed, and runs the effect once if any did.
 *
 * @param effect  The effect.
 *
 * @returns The C block.
 */
export function printDepEffectCheck(effect: IrDepEffect): string {
  const lines = ['    {', '        int er_changed = 0;'];
  effect.deps.forEach((depExpr, depIndex) => {
    const dep = toCExpr(depExpr);
    const copy = depCopy(effect, depIndex);
    if (dep.cType === 'string') {
      lines.push(
        `        if (strcmp(${copy}, ${dep.code}) != 0) { ` +
          `${printDepSnapshot(effect, depIndex)}; er_changed = 1; }`,
      );
    } else {
      const cType = cScalarType(dep.cType);
      lines.push(
        `        ${cType} er_d${depIndex} = ${dep.code}; if (er_d${depIndex} != ` +
          `${copy}) { ${copy} = er_d${depIndex}; ` +
          `er_changed = 1; }`,
      );
    }
  });

  lines.push(`        if (er_changed) ${effectName(effect)}();`, '    }');
  return lines.join('\n');
}

/**
 * A file-scope variable that effects and animation chains keep between calls.
 *
 * @param decl  The variable.
 * @param stringCap  The size of a string buffer.
 *
 * @returns The C declarations.
 */
export function printStaticDecl(
  decl: IrStaticDecl,
  stringCap: number,
): string[] {
  switch (decl.kind) {
    case 'local':
      return [
        decl.cType === 'string'
          ? `static char ${decl.name}[${stringCap}];`
          : `static ${cScalarType(decl.cType)} ${decl.name};`,
      ];
    case 'armed':
      return [`static int ${armedFlag(decl.effect.id)};`];
    case 'deps':
      return decl.effect.deps.map((depExpr, depIndex) => {
        const copy = depCopy(decl.effect, depIndex);
        const {cType} = toCExpr(depExpr);
        return cType === 'string'
          ? `static char ${copy}[${stringCap}];`
          : `static ${cScalarType(cType)} ${copy};`;
      });
    case 'chain':
      return printChainFunctions(decl.chain).decls;
  }
}
