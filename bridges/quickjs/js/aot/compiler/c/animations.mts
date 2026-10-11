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

import {floatLit} from '../c-syntax.mts';
import {printExpr} from './expressions.mts';
import type {
  IrAnimation,
  IrAnimationStart,
  IrCompletion,
  IrCompletionWait,
  IrStartChain,
  IrWaitSlot,
} from '../ir/animations.mts';
import type {CFunction} from '../out.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** What a chain adds at file scope: it's on_complete callbacks and the slots its loop keeps. */
export interface ChainFunctions {
  /** In the order they are emitted: the last step's callback first, the repeat (if any) last. */
  callbacks: CFunction[];
  decls: string[];
}

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * One animation as a scoped ERAnimConfig and the er_anim_value_animate call that starts it. Animation lines
 * carry their own indent, whatever block they sit in.
 *
 * @param animation  The animation.
 * @param configName  The config's name, unique within the function.
 * @param onComplete  The C function to run when it completes, or null.
 *
 * @returns The C block.
 */
export function printAnimation(
  animation: IrAnimation,
  configName: string,
  onComplete: string | null,
): string[] {
  const set = (field: string, value: string) =>
    `        ${configName}.${field} = ${value};`;
  const lines = [
    '    {',
    `        ERAnimConfig ${configName};`,
    `        memset(&${configName}, 0, sizeof(${configName}));`,
  ];

  // The curve's own fields.
  const curve = animation.curve;
  if (curve.kind === 'spring') {
    lines.push(
      set('type', 'ER_ANIM_SPRING'),
      set('stiffness', floatLit(curve.stiffness)),
      set('damping', floatLit(curve.damping)),
      set('mass', floatLit(curve.mass)),
    );
  } else if (curve.kind === 'decay') {
    lines.push(
      set('type', 'ER_ANIM_DECAY'),
      set('deceleration', floatLit(curve.deceleration)),
      set('velocity', floatLit(curve.velocity)),
    );
  } else {
    lines.push(
      set('type', 'ER_ANIM_TIMING'),
      set('duration_ms', String(curve.durationMs)),
      set('easing', curve.easing),
    );
    if (curve.bezier) {
      const [x1, y1, x2, y2] = curve.bezier.map(point => floatLit(point));
      lines.push(
        `        ${configName}.bezier_x1 = ${x1}; ${configName}.bezier_y1 = ${y1};`,
        `        ${configName}.bezier_x2 = ${x2}; ${configName}.bezier_y2 = ${y2};`,
      );
    }
  }

  // The fields every curve shares.
  if (animation.delayMs > 0) {
    lines.push(set('delay_ms', String(animation.delayMs)));
  }
  if (animation.isLooping) {
    lines.push(set('loop', 'true'));
  }
  if (onComplete) {
    lines.push(set('on_complete', onComplete));
  }

  // Decay with no target is given the value it holds now.
  const to = animation.to
    ? printExpr(animation.to)
    : `er_anim_value_get(${animation.handle})`;
  lines.push(
    `        er_anim_value_animate(${animation.handle}, (float)(${to}), &${configName});`,
    '    }',
  );

  return lines;
}

/**
 * Runs a completion as finished, outside the engine's own on_complete.
 *
 * @param completion  The completion.
 *
 * @returns The C line.
 */
export const printCompletionCall = (completion: IrCompletion): string =>
  `    ${completion.name}(true, NULL);`;

/**
 * Starts a composition's completion wait in its slot.
 *
 * @param wait  The slot.
 * @param delayMs  The wait.
 * @param completion  The completion it runs.
 *
 * @returns The C line.
 */
const printWaitStart = (
  wait: IrWaitSlot,
  delayMs: number,
  completion: IrCompletion,
): string =>
  `    app_wait_start(&${wait.slot}, ${delayMs}, ${wait.timerFn}, ${completion.name});`;

/**
 * The body of a wait slot's timer function: the wait is over, so the completion runs as finished.
 *
 * @param slot  The slot.
 *
 * @returns The C lines.
 */
export const printWaitEnd = (slot: string): string[] => [
  `    app_wait_end(&${slot});`,
];

/**
 * The wait slots and the helpers that drive them, each helper only when the generated code calls it (an unused
 * static function would warn).
 *
 * @param slots  The slots.
 * @param callers  The generated code, to look for calls in.
 *
 * @returns The C block, or '' with no slots.
 */
export function printWaitDeclarations(
  slots: string[],
  callers: string,
): string {
  if (!slots.length) {
    return '';
  }

  const usesStart = callers.includes('app_wait_start(');
  const usesEnd = callers.includes('app_wait_end(');
  const blocks = [
    `/* A composition's wait between its last animation and its completion: the pending timer (or -1) and the
   completion it runs. Stopping the composition ends the wait early, as not finished. */
typedef struct
{
    int timer;
    ERAnimCompleteFn done;
} ErAnimWait;`,
    slots.map(slot => `static ErAnimWait ${slot} = {-1, NULL};`).join('\n'),
  ];
  if (usesStart || callers.includes('app_wait_stop(')) {
    blocks.push(`/* Ends a pending wait early: its completion runs as not finished. */
static void app_wait_stop(ErAnimWait* wait)
{
    if (wait->timer < 0)
    {
        return;
    }
    er_timer_clear(wait->timer);
    wait->timer = -1;
    if (wait->done)
    {
        wait->done(false, NULL);
    }
}`);
  }
  if (usesStart) {
    blocks.push(`/* Starts a wait. One still pending was replaced by this start, so it ends as not finished first. */
static void app_wait_start(ErAnimWait* wait, int ms, void (*fn)(void), ERAnimCompleteFn done)
{
    app_wait_stop(wait);
    wait->done = done;
    wait->timer = er_timer_add(ms, false, fn);
}`);
  }
  if (usesEnd) {
    blocks.push(`/* The wait ran its course: its completion runs as finished. */
static void app_wait_end(ErAnimWait* wait)
{
    wait->timer = -1;
    if (wait->done)
    {
        wait->done(true, NULL);
    }
}`);
  }

  return blocks.join('\n\n');
}

/**
 * The name of a chain's callback that starts step `index` (step 0's starts a repeat).
 *
 * @param chain  The chain.
 * @param index  The step.
 *
 * @returns The C function's name.
 */
const chainCallback = (chain: IrStartChain, index: number): string =>
  `er_seqcb_${chain.id}_${index}`;

/** The slot that counts a counted loop's passes. */
const passCounter = (chain: IrStartChain): string => `s_loop${chain.id}_n`;

/** The slot that keeps where a looped value started. */
const startValue = (chain: IrStartChain): string => `s_loop${chain.id}_from`;

/**
 * The callback the step after `index` is started from, or what runs after the last step: the repeat for a
 * loop, else the completion.
 *
 * @param chain  The chain.
 * @param index  The step.
 *
 * @returns The C function's name, or null for nothing.
 */
function afterStep(chain: IrStartChain, index: number): string | null {
  if (index < chain.steps.length - 1) {
    return chainCallback(chain, index + 1);
  }

  if (chain.loop) {
    return chainCallback(chain, 0);
  }

  return chain.completionWait?.callback ?? chain.completion?.name ?? null;
}

/**
 * Calls the completion with `finished`, from inside a chain callback.
 *
 * @param completion  The completion, or null.
 * @param finished  The C value passed as `finished`.
 *
 * @returns The C line, or none.
 */
const callCompletion = (
  completion: IrCompletion | null,
  finished: string,
): string[] =>
  completion ? [`        ${completion.name}(${finished}, NULL);`] : [];

/**
 * Ends a chain or composition when the animation before was interrupted, calling the completion as not finished.
 *
 * @param completion  The completion, or null.
 *
 * @returns The C lines.
 */
const endIfInterrupted = (completion: IrCompletion | null): string[] => [
  '    if (!finished)',
  '    {',
  ...callCompletion(completion, 'false'),
  '        return;',
  '    }',
];

/**
 * The on_complete callback that starts a completion's wait once the last animation has finished.
 *
 * @param completion  The completion.
 * @param wait  The wait.
 *
 * @returns The C function.
 */
export const printCompletionWait = (
  completion: IrCompletion,
  wait: IrCompletionWait,
): CFunction => ({
  name: wait.callback,
  body: [
    ...endIfInterrupted(completion),
    printWaitStart(wait.wait, wait.delayMs, completion),
  ],
});

/**
 * A chain's file-scope callbacks and loop slots. Each callback first ends the chain if the step before it was
 * interrupted, as JS does.
 *
 * @param chain  The chain.
 *
 * @returns The callbacks and declarations.
 */
export function printChainFunctions(chain: IrStartChain): ChainFunctions {
  const interrupted = endIfInterrupted(chain.completion);

  // The wait after the last step comes first, then steps n..1, each started from the callback of the step before.
  const callbacks: CFunction[] = [];
  if (chain.completion && chain.completionWait) {
    callbacks.push(printCompletionWait(chain.completion, chain.completionWait));
  }
  for (let index = chain.steps.length - 1; index >= 1; index--) {
    callbacks.push({
      name: chainCallback(chain, index),
      body: [
        ...interrupted,
        ...printAnimation(
          chain.steps[index],
          `cfg${chain.id}_${index}`,
          afterStep(chain, index),
        ),
      ],
    });
  }

  const decls: string[] = [];
  const loop = chain.loop;
  if (!loop) {
    return {callbacks, decls};
  }

  // The repeat runs when the last step finishes: it counts the pass, resets the value, and starts step 0 again.
  const repeat = [...interrupted];
  if (loop.iterations >= 0) {
    const counter = passCounter(chain);
    decls.push(`static int ${counter};`);
    repeat.push(
      `    if (${counter} >= ${loop.iterations})`,
      '    {',
      ...callCompletion(chain.completion, 'true'),
      '        return;',
      '    }',
      `    ${counter}++;`,
    );
  }
  if (loop.resetHandle) {
    decls.push(`static float ${startValue(chain)};`);
    repeat.push(
      `    er_anim_value_set(${loop.resetHandle}, ${startValue(chain)});`,
    );
  }

  repeat.push(
    ...printAnimation(loop.restart, `cfg${chain.id}_0`, afterStep(chain, 0)),
  );
  callbacks.push({name: chainCallback(chain, 0), body: repeat});

  return {callbacks, decls};
}

/**
 * The C that starts an animation where `.start()` was called. A chain's callbacks and slots are file-scope
 * (see printChainFunctions).
 *
 * @param start  The start.
 *
 * @returns The C lines, which carry their own indent.
 */
export function printAnimationStart(start: IrAnimationStart): string[] {
  switch (start.kind) {
    case 'startAnimations': {
      // A completion nothing can run (an endless loop of delays) is still referenced, so -Werror builds pass.
      if (!start.animations.length) {
        return start.completion ? [`    (void)${start.completion.name};`] : [];
      }

      // The completion (or its wait) goes on the last animation, the only one when there is a completion.
      const last = start.animations.length - 1;
      const onComplete =
        start.completionWait?.callback ?? start.completion?.name ?? null;
      return start.animations.flatMap((animation, index) =>
        printAnimation(
          animation,
          `cfg${start.firstConfig + index}`,
          index === last ? onComplete : null,
        ),
      );
    }
    case 'completeAfter':
      return start.wait
        ? [printWaitStart(start.wait, start.delayMs, start.completion)]
        : [printCompletionCall(start.completion)];
    case 'startChain': {
      // A loop first records the pass and where the value starts, then step 0 starts inline.
      const lines: string[] = [];
      if (start.loop && start.loop.iterations >= 0) {
        lines.push(`    ${passCounter(start)} = 1;`);
      }
      if (start.loop?.resetHandle) {
        lines.push(
          `    ${startValue(start)} = er_anim_value_get(${start.loop.resetHandle});`,
        );
      }

      return [
        ...lines,
        ...printAnimation(
          start.steps[0],
          `cfg${start.id}_0`,
          afterStep(start, 0),
        ),
      ];
    }
  }
}
