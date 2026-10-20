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
} from '../ir/animations.mts';
import type {CFunction} from './statements.mts';

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
 * @param userData  What the engine passes that function as user_data, or null.
 *
 * @returns The C block.
 */
export function printAnimation(
  animation: IrAnimation,
  configName: string,
  onComplete: string | null,
  userData: string | null = null,
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
  if (userData) {
    lines.push(set('on_complete_user_data', userData));
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
 * Starts a completion wait for one run of a composition.
 *
 * @param owner  The composition.
 * @param delayMs  The wait.
 * @param completion  The completion it runs.
 *
 * @returns The C line.
 */
const printWaitStart = (
  owner: number,
  delayMs: number,
  completion: IrCompletion,
): string => `    app_wait_start(${owner}, ${delayMs}, ${completion.name});`;

/**
 * The completion-wait table and the helpers that drive it, each helper only when the generated code calls it
 * (an unused static function would warn). Every run of a composition has a wait of its own, so starting one never
 * ends another; a stop ends every run of its composition.
 *
 * @param callers  The generated code, to look for calls in.
 *
 * @returns The C block.
 */
export function printWaitDeclarations(callers: string): string {
  const blocks = [
    `#ifndef ER_AOT_MAX_WAITS
#define ER_AOT_MAX_WAITS 8
#endif

/* The wait between a run's last animation and its completion: the composition it belongs to (which a stop
   names), the time left, the animations of a joined run still running, and the completion it runs. */
typedef struct
{
    int remaining_ms;
    int owner;
    ERAnimCompleteFn done;
    uint8_t state;
    uint8_t parts;
} ErAnimWait;
enum
{
    APP_WAIT_FREE,
    APP_WAIT_PENDING,
    APP_WAIT_DUE,
    APP_WAIT_STOPPING,
    /* A joined run that has completed, kept until its animations still running have reported. */
    APP_WAIT_DRAINING
};
static ErAnimWait s_waits[ER_AOT_MAX_WAITS];

/* Takes a free wait for one run; runs never share one. */
static int app_wait_claim(int owner, int ms, int parts, ERAnimCompleteFn done)
{
    for (int i = 0; i < ER_AOT_MAX_WAITS; i++)
    {
        if (s_waits[i].state == APP_WAIT_FREE)
        {
            s_waits[i].remaining_ms = ms;
            s_waits[i].owner = owner;
            s_waits[i].done = done;
            s_waits[i].parts = (uint8_t)parts;
            s_waits[i].state = APP_WAIT_PENDING;
            return i;
        }
    }

    /* The table is full (raise ER_AOT_MAX_WAITS): the run cannot wait, so it ends now, as not finished. A run
       that completion starts while the table is still full is dropped instead of recursing. */
    static bool s_reporting_full;
    if (!s_reporting_full)
    {
        s_reporting_full = true;
        done(false, NULL);
        s_reporting_full = false;
    }
    return -1;
}

/* Counts down every wait, then completes the ones that ran out. A joined run that runs out first holds until
   its animations finish. A wait a completion starts here waits its full time. */
static void app_wait_tick(int dt_ms)
{
    for (int i = 0; i < ER_AOT_MAX_WAITS; i++)
    {
        if (s_waits[i].state == APP_WAIT_PENDING && s_waits[i].remaining_ms > 0)
        {
            s_waits[i].remaining_ms -= dt_ms;
        }
        if (s_waits[i].state == APP_WAIT_PENDING && s_waits[i].remaining_ms <= 0 && s_waits[i].parts == 0)
        {
            s_waits[i].state = APP_WAIT_DUE;
        }
    }
    for (int i = 0; i < ER_AOT_MAX_WAITS; i++)
    {
        if (s_waits[i].state == APP_WAIT_DUE)
        {
            s_waits[i].state = APP_WAIT_FREE;
            s_waits[i].done(true, NULL);
        }
    }
}`,
  ];
  if (callers.includes('app_wait_start(')) {
    blocks.push(`/* Starts the wait between a run's last animation and its completion. */
static void app_wait_start(int owner, int ms, ERAnimCompleteFn done)
{
    (void)app_wait_claim(owner, ms, 0, done);
}`);
  }
  if (callers.includes('app_wait_join(')) {
    blocks.push(`/* Starts a run that waits for its animations as well as its delay: it completes as finished once ms have passed
   and all of its parts (animations) have finished, and as not finished as soon as one is interrupted. Each
   animation runs app_wait_part when it ends, with what this returns as its user_data (NULL: the table was full,
   so the run has already ended). */
static void* app_wait_join(int owner, int ms, int parts, ERAnimCompleteFn done)
{
    const int i = app_wait_claim(owner, ms, parts, done);
    return i < 0 ? NULL : (void*)(intptr_t)(i + 1);
}

/* An animation of a joined run has ended. */
static void app_wait_part(bool finished, void* user_data)
{
    if (!user_data)
    {
        return;
    }

    ErAnimWait* wait = &s_waits[(intptr_t)user_data - 1];
    wait->parts--;
    if (wait->state == APP_WAIT_DRAINING)
    {
        if (wait->parts == 0)
        {
            wait->state = APP_WAIT_FREE;
        }
        return;
    }

    /* A stop in progress reports the run itself (see app_wait_report). */
    if (wait->state != APP_WAIT_PENDING)
    {
        return;
    }
    if (!finished)
    {
        wait->state = wait->parts ? APP_WAIT_DRAINING : APP_WAIT_FREE;
        wait->done(false, NULL);
    }
    else if (wait->parts == 0 && wait->remaining_ms <= 0)
    {
        wait->state = APP_WAIT_FREE;
        wait->done(true, NULL);
    }
}`);
  }
  if (callers.includes('app_wait_detach(')) {
    blocks.push(`/* A stop's two halves: detach the composition's waits, then report them as not finished. Anything a
   completion in between starts is a new run, which the stop leaves alone. */
static void app_wait_detach(int owner)
{
    for (int i = 0; i < ER_AOT_MAX_WAITS; i++)
    {
        if (s_waits[i].owner == owner &&
            (s_waits[i].state == APP_WAIT_PENDING || s_waits[i].state == APP_WAIT_DUE))
        {
            s_waits[i].state = APP_WAIT_STOPPING;
        }
    }
}

static void app_wait_report(int owner)
{
    for (int i = 0; i < ER_AOT_MAX_WAITS; i++)
    {
        if (s_waits[i].owner == owner && s_waits[i].state == APP_WAIT_STOPPING)
        {
            s_waits[i].state = s_waits[i].parts ? APP_WAIT_DRAINING : APP_WAIT_FREE;
            s_waits[i].done(false, NULL);
        }
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
    printWaitStart(wait.owner, wait.delayMs, completion),
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
    const finish =
      loop.finalWait && chain.completion
        ? [
            '    ' +
              printWaitStart(
                loop.finalWait.owner,
                loop.finalWait.delayMs,
                chain.completion,
              ),
          ]
        : callCompletion(chain.completion, 'true');
    repeat.push(
      `    if (${counter} >= ${loop.iterations})`,
      '    {',
      ...finish,
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

      // A joined run takes its wait first, and every animation reports to it.
      const join = start.completionJoin;
      if (join && start.completion) {
        return [
          '    {',
          `        void* join = app_wait_join(${join.owner}, ${join.delayMs}, ${start.animations.length}, ${start.completion.name});`,
          ...start.animations.flatMap((animation, index) =>
            printAnimation(
              animation,
              `cfg${start.firstConfig + index}`,
              'app_wait_part',
              'join',
            ).map(line => `    ${line}`),
          ),
          '    }',
        ];
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
      return start.owner !== null
        ? [printWaitStart(start.owner, start.delayMs, start.completion)]
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
