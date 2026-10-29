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
 * Animations in the package output, lowered the way the C output starts them, so they run frame for frame alike.
 *
 * An animated value is a slot. Each animation the IR starts is a leaf descriptor, started by ASTART with its
 * toValue in a register; its delay is the engine's own delay_ms, as in C. Each C on_complete callback is an
 * animation-done function: a chain's steps start one another from them, and a completion wait is a delay leaf
 * started from one. A completion is a helper that takes `finished`, entered through a done function of its own
 * when an animation ends with it, or called when C calls it directly. A run joined at run time (a spring beside a
 * longer delay) is a parallel group of a delay leaf and the animation. A composition with waits keeps the handle
 * of its last wait in a slot, so a stop can end it.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {
  ANIM_PROP_FIELDS,
  AnimFlag,
  AnimKind,
  BindingKind,
  ENGINE_VALUES,
  EventField,
  FunctionKind,
  InterpFlag,
  NONE,
  ValueType,
} from './format.mts';
import {FunctionBuilder} from './bytecode.mts';
import {abiProp, floatBits} from './props.mts';
import type {Arena, Slot} from './arena.mts';
import type {Label, Register} from './bytecode.mts';
import type {Codegen} from './codegen.mts';
import type {
  IrAnimation,
  IrCompletion,
  IrStartAnimations,
  IrStartChain,
} from '../ir/animations.mts';
import type {IrApp} from '../ir/app.mts';
import type {IrPressFade} from '../ir/functions.mts';
import type {IrStmt} from '../ir/statements.mts';
import type {AnimBind} from '../style-text.mts';
import type {
  ProgramAnim,
  ProgramBinding,
  ProgramFunction,
  ProgramInterp,
} from './writer.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** A looping chain's slots: its pass counter, and where its value started. */
interface LoopSlots {
  passes: Slot | null;
  from: Slot | null;
}

/** An animation-done function, by the C callback it stands for. */
type DoneFunction =
  | {kind: 'completion'; completion: IrCompletion}
  | {kind: 'wait'; completion: IrCompletion; delayMs: number; owner: number}
  | {kind: 'step'; chain: IrStartChain; index: number}
  | {kind: 'repeat'; chain: IrStartChain};

/** The animation statements. */
type AnimationStmt = IrStmt & {
  kind: 'startAnimations' | 'startChain' | 'completeAfter' | 'stopAnimations';
};

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** Waits that run at once, as the C output's ER_AOT_MAX_WAITS. */
const MAX_WAITS = 8;

/** Running-animation entries a wait takes: its delay leaf, and a joined run's group. */
const WAIT_ENTRIES = 2;

/** A register operand that names no register (ASTART with no toValue in one). */
const NO_REGISTER = 0xff;

/** A <TouchableOpacity> fade's easing, as the C output's. */
const PRESS_FADE_EASING = 'ER_EASE_QUAD_IN_OUT';

/*----------------------------------------------------------------------------------------------------------------------
 - Classes
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * The program's animations: the slots they keep, their descriptors and interpolations, and the done functions
 * and completion helpers code names. Functions are numbered in place until the program's are, then given their
 * final indices.
 *
 * @class AnimationPlan
 */
export class AnimationPlan {
  /** Each animated value's slot, by its C handle. */
  readonly values = new Map<string, Slot>();
  /** The slot each started composition with waits keeps its last wait's handle in, by owner. */
  readonly owners = new Map<number, Slot>();
  readonly loops = new Map<number, LoopSlots>();
  readonly anims: ProgramAnim[] = [];
  readonly interps: ProgramInterp[] = [];
  /** The done functions, in function order, by C name. */
  readonly doneFunctions = new Map<string, DoneFunction>();
  /** The completions, in function order (callers first), by C name. */
  readonly completions = new Map<string, IrCompletion>();
  private readonly descriptors = new Map<object, number>();
  private readonly waits = new Map<number, number>();
  private readonly interpIndex = new Map<string, number>();
  /** The running-animation table's entries. */
  readonly entries: number = 0;
  private doneBase = 0;
  private helperBase = 0;

  /**
   * Plans the slots the app's animations keep, and the running-animation table.
   *
   * @param app  The app.
   * @param arena  The arena being planned.
   */
  constructor(app: IrApp, arena: Arena) {
    const {out} = app;
    for (const value of app.animations) {
      this.values.set(
        value.cVar,
        arena.slot(ValueType.avalue, floatBits(value.initial)),
      );
    }
    for (const [, record] of out.animWaits) {
      if (record.isStarted) {
        this.owners.set(record.owner, arena.slot(ValueType.anim));
      }
    }

    // Each C callback, in the order the app declares them; a completion's own, first, is called from the rest.
    const completions: IrCompletion[] = [];
    for (const callback of out.animCbs) {
      if (callback.kind === 'completion') {
        completions.push(callback.completion);
        this.doneFunctions.set(callback.completion.name, {
          kind: 'completion',
          completion: callback.completion,
        });
      } else if (callback.kind === 'completionWait') {
        this.doneFunctions.set(callback.wait.callback, {
          kind: 'wait',
          completion: callback.completion,
          delayMs: callback.wait.delayMs,
          owner: callback.wait.owner,
        });
      } else {
        const {chain} = callback;
        if (chain.completion && chain.completionWait) {
          this.doneFunctions.set(chain.completionWait.callback, {
            kind: 'wait',
            completion: chain.completion,
            delayMs: chain.completionWait.delayMs,
            owner: chain.completionWait.owner,
          });
        }
        for (let index = chain.steps.length - 1; index >= 1; index--) {
          this.doneFunctions.set(chainCallback(chain, index), {
            kind: 'step',
            chain,
            index,
          });
        }
        if (chain.loop) {
          this.doneFunctions.set(chainCallback(chain, 0), {
            kind: 'repeat',
            chain,
          });
          this.loops.set(chain.id, {
            passes:
              chain.loop.iterations >= 0 ? arena.slot(ValueType.int) : null,
            from: chain.loop.resetHandle ? arena.slot(ValueType.float) : null,
          });
        }
      }
    }

    // A completion that a completion calls was lowered inside it, so before it: callers go first, as CALL needs.
    for (const completion of completions.reverse()) {
      this.completions.set(completion.name, completion);
    }

    // Each value runs one animation at a time, and one more while a start cancels it; each wait takes its own,
    // even in an app that only waits.
    if (this.values.size || this.owners.size) {
      const waits = this.owners.size ? MAX_WAITS * WAIT_ENTRIES : 0;
      this.entries = this.values.size + 1 + waits;
      arena.animTable(this.entries);
    }
  }

  /**
   * Numbers the done functions and the completion helpers.
   *
   * @param doneBase  The first done function's index.
   * @param helperBase  The first helper's index.
   */
  number(doneBase: number, helperBase: number): void {
    this.doneBase = doneBase;
    this.helperBase = helperBase;
  }

  /**
   * The done function a C callback is.
   *
   * @param name  The callback's C name.
   *
   * @returns Its index.
   */
  done(name: string): number {
    const index = [...this.doneFunctions.keys()].indexOf(name);
    if (index < 0) {
      throw new Error(`AOT internal: no animation callback "${name}"`);
    }
    return this.doneBase + index;
  }

  /**
   * The helper a completion is.
   *
   * @param completion  The completion.
   *
   * @returns Its index.
   */
  helper(completion: IrCompletion): number {
    return (
      this.helperBase + [...this.completions.keys()].indexOf(completion.name)
    );
  }

  /**
   * An animated value's slot.
   *
   * @param handle  Its C handle.
   *
   * @returns The slot.
   */
  value(handle: string): Slot {
    const slot = this.values.get(handle);
    if (!slot) {
      throw new Error(`AOT internal: no animated value "${handle}"`);
    }
    return slot;
  }

  /**
   * An animation's leaf descriptor, made the first time.
   *
   * @param animation  The animation.
   *
   * @returns Its index.
   */
  leaf(animation: IrAnimation): number {
    let index = this.descriptors.get(animation);
    if (index === undefined) {
      index = this.anims.push(this.leafRecord(animation)) - 1;
      this.descriptors.set(animation, index);
    }
    return index;
  }

  /**
   * A press fade's descriptor: a timing of the node's opacity value to a constant.
   *
   * @param fade  The fade.
   *
   * @returns Its index.
   */
  fade(fade: IrPressFade): number {
    let index = this.descriptors.get(fade);
    if (index === undefined) {
      index =
        this.anims.push({
          ...leafDefaults(),
          kind: AnimKind.timing,
          easing: ENGINE_VALUES.get(PRESS_FADE_EASING)!,
          target: this.value(fade.value).index,
          amount: fade.durationMs,
          to: floatBits(fade.opacity),
        }) - 1;
      this.descriptors.set(fade, index);
    }
    return index;
  }

  /**
   * A completion wait's delay leaf.
   *
   * @param delayMs  The wait.
   *
   * @returns Its index.
   */
  wait(delayMs: number): number {
    let index = this.waits.get(delayMs);
    if (index === undefined) {
      index = this.anims.push({...leafDefaults(), amount: delayMs}) - 1;
      this.waits.set(delayMs, index);
    }
    return index;
  }

  /**
   * A joined run: a parallel group of its delay and its one animation, done when both are and not finished as
   * soon as the animation stops.
   *
   * @param start  The start.
   *
   * @returns The group's index.
   */
  join(start: IrStartAnimations): number {
    let index = this.descriptors.get(start);
    if (index === undefined) {
      index = this.anims.length;
      this.anims.push(
        {
          ...leafDefaults(),
          kind: AnimKind.parallel,
          first: index + 1,
          count: 2,
        },
        {...leafDefaults(), amount: start.completionJoin!.delayMs},
        this.leafRecord(start.animations[0]),
      );
      this.descriptors.set(start, index);
    }
    return index;
  }

  /**
   * An animated value's bind as a binding: straight onto the prop, or through an interpolation.
   *
   * @param bind  The bind.
   *
   * @returns The binding.
   */
  binding(bind: AnimBind): ProgramBinding {
    const field = ANIM_PROP_FIELDS.get(bind.prop);
    if (!field) {
      throw new Error(`AOT internal: no ABI prop for ${bind.prop}`);
    }
    const prop = abiProp(field).id;
    const slot = this.value(bind.cVar);
    const {interp} = bind;
    if (!interp) {
      return {kind: BindingKind.animated, event: prop, target: slot.index};
    }

    // Interpolations that are the same are stored once, so list rows that bind one can share a template.
    const record: ProgramInterp = {
      slot: slot.index,
      flags: interp.colors ? InterpFlag.color : 0,
      extrapolateLeft: ENGINE_VALUES.get(interp.exLeft)!,
      extrapolateRight: ENGINE_VALUES.get(interp.exRight)!,
      input: interp.input.map(floatBits),
      output: interp.colors
        ? interp.colors.map(color => color >>> 0)
        : interp.output.map(floatBits),
    };
    const key = JSON.stringify(record);
    let index = this.interpIndex.get(key);
    if (index === undefined) {
      index = this.interps.push(record) - 1;
      this.interpIndex.set(key, index);
    }
    return {kind: BindingKind.interpolated, event: prop, target: index};
  }

  /**
   * The slot a composition keeps its last wait's handle in.
   *
   * @param owner  The composition.
   *
   * @returns The slot.
   */
  owner(owner: number): Slot {
    const slot = this.owners.get(owner);
    if (!slot) {
      throw new Error(`AOT internal: no slot for composition ${owner}`);
    }
    return slot;
  }

  private leafRecord(animation: IrAnimation): ProgramAnim {
    const {curve} = animation;
    const record: ProgramAnim = {
      ...leafDefaults(),
      target: this.value(animation.handle).index,
      delayMs: animation.delayMs,
      flags:
        (animation.isLooping ? AnimFlag.loop : 0) |
        (animation.to?.kind === 'number' ? 0 : AnimFlag.toRegister),
      to: animation.to?.kind === 'number' ? floatBits(animation.to.value) : 0,
    };
    switch (curve.kind) {
      case 'spring':
        return {
          ...record,
          kind: AnimKind.spring,
          params: [
            floatBits(curve.stiffness),
            floatBits(curve.damping),
            floatBits(curve.mass),
            0,
          ],
        };
      case 'decay':
        return {
          ...record,
          kind: AnimKind.decay,
          params: [
            floatBits(curve.velocity),
            floatBits(curve.deceleration),
            0,
            0,
          ],
        };
      case 'timing':
        return {
          ...record,
          kind: AnimKind.timing,
          easing: ENGINE_VALUES.get(curve.easing)!,
          amount: curve.durationMs,
          params: curve.bezier
            ? (curve.bezier.map(floatBits) as ProgramAnim['params'])
            : [0, 0, 0, 0],
        };
    }
  }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * A descriptor with nothing set: a delay leaf of 0 ms, which a record overrides.
 *
 * @returns The record.
 */
const leafDefaults = (): ProgramAnim => ({
  kind: AnimKind.timing,
  easing: 0,
  flags: 0,
  target: NONE,
  prop: NONE,
  first: 0,
  count: 0,
  amount: 0,
  delayMs: 0,
  to: 0,
  params: [0, 0, 0, 0],
});

/**
 * The name of a chain's callback that starts step `index` (step 0's starts a repeat), as the C output names it.
 *
 * @param chain  The chain.
 * @param index  The step.
 *
 * @returns The C name.
 */
const chainCallback = (chain: IrStartChain, index: number): string =>
  `er_seqcb_${chain.id}_${index}`;

/**
 * The done function the step after `index` is started from, or what runs after the last step: the repeat for a
 * loop, else the completion's wait or the completion.
 *
 * @param chain  The chain.
 * @param index  The step.
 * @param plan  The program's animations.
 *
 * @returns The function, or NONE.
 */
function afterStep(
  chain: IrStartChain,
  index: number,
  plan: AnimationPlan,
): number {
  if (index < chain.steps.length - 1) {
    return plan.done(chainCallback(chain, index + 1));
  }
  if (chain.loop) {
    return plan.done(chainCallback(chain, 0));
  }
  const name = chain.completionWait?.callback ?? chain.completion?.name;
  return name ? plan.done(name) : NONE;
}

/**
 * Starts a descriptor, as er_anim_value_animate starts the C output's animation.
 *
 * @param codegen  The function's codegen.
 * @param descriptor  The descriptor.
 * @param done  The done function, or NONE.
 * @param to  The register its toValue is in, or null when the descriptor holds it.
 *
 * @returns The running animation's handle.
 */
function start(
  codegen: Codegen,
  descriptor: number,
  done: number,
  to: Register | null,
): Register {
  const handle = codegen.fn.register('anim');
  codegen.fn.abx(
    'ASTART',
    handle.index,
    descriptor,
    (done & 0xffff) | ((to?.index ?? NO_REGISTER) << 16),
  );
  return handle;
}

/**
 * An animation's toValue in a float register, as C converts it, or null when its descriptor holds it. A decay with
 * none is given the value it holds now.
 *
 * @param codegen  The function's codegen.
 * @param animation  The animation.
 * @param plan  The program's animations.
 *
 * @returns The register.
 */
function toValue(
  codegen: Codegen,
  animation: IrAnimation,
  plan: AnimationPlan,
): Register | null {
  if (animation.to?.kind === 'number') {
    return null;
  }
  if (animation.to) {
    return codegen.valueAs(animation.to, 'float');
  }
  const register = codegen.fn.register('float');
  codegen.fn.abx('AVGET', register.index, plan.value(animation.handle).index);
  return register;
}

/**
 * Starts one animation.
 *
 * @param codegen  The function's codegen.
 * @param animation  The animation.
 * @param done  The done function, or NONE.
 * @param plan  The program's animations.
 */
function startAnimation(
  codegen: Codegen,
  animation: IrAnimation,
  done: number,
  plan: AnimationPlan,
): void {
  codegen.scoped(() =>
    start(
      codegen,
      plan.leaf(animation),
      done,
      toValue(codegen, animation, plan),
    ),
  );
}

/**
 * Starts a completion's wait, keeping its handle where a stop of its composition finds it.
 *
 * @param codegen  The function's codegen.
 * @param completion  The completion it runs.
 * @param delayMs  The wait.
 * @param owner  The composition.
 * @param plan  The program's animations.
 */
function startWait(
  codegen: Codegen,
  completion: IrCompletion,
  delayMs: number,
  owner: number,
  plan: AnimationPlan,
): void {
  codegen.scoped(() => {
    const handle = start(
      codegen,
      plan.wait(delayMs),
      plan.done(completion.name),
      null,
    );
    codegen.fn.abx('SET32', handle.index, plan.owner(owner).index);
  });
}

/**
 * Calls a completion, as C calls it directly.
 *
 * @param codegen  The function's codegen.
 * @param completion  The completion.
 * @param finished  A bool register holding what it is passed.
 * @param plan  The program's animations.
 */
function callCompletion(
  codegen: Codegen,
  completion: IrCompletion,
  finished: Register,
  plan: AnimationPlan,
): void {
  codegen.fn.abx('CALL', finished.index, plan.helper(completion));
}

/**
 * Calls a completion with a constant.
 *
 * @param codegen  The function's codegen.
 * @param completion  The completion.
 * @param finished  What it is passed.
 * @param plan  The program's animations.
 */
function callCompletionWith(
  codegen: Codegen,
  completion: IrCompletion,
  finished: boolean,
  plan: AnimationPlan,
): void {
  codegen.scoped(() =>
    callCompletion(
      codegen,
      completion,
      codegen.load('bool', finished ? 1 : 0),
      plan,
    ),
  );
}

/**
 * Lowers an animation statement where the C output starts or stops one.
 *
 * @param codegen  The function's codegen.
 * @param stmt  The statement.
 * @param plan  The program's animations.
 */
export function animationStatement(
  codegen: Codegen,
  stmt: AnimationStmt,
  plan: AnimationPlan,
): void {
  const {fn} = codegen;
  switch (stmt.kind) {
    case 'startAnimations': {
      const {animations, completion, completionJoin} = stmt;
      if (!animations.length) {
        return;
      }

      // A joined run is one tree, kept where a stop of its composition finds it.
      if (completionJoin && completion) {
        codegen.scoped(() => {
          const handle = start(
            codegen,
            plan.join(stmt),
            plan.done(completion.name),
            toValue(codegen, animations[0], plan),
          );
          fn.abx('SET32', handle.index, plan.owner(completionJoin.owner).index);
        });
        return;
      }

      // The completion (or its wait) goes on the last animation, the only one when there is a completion.
      const name = stmt.completionWait?.callback ?? completion?.name;
      animations.forEach((animation, index) =>
        startAnimation(
          codegen,
          animation,
          index === animations.length - 1 && name ? plan.done(name) : NONE,
          plan,
        ),
      );
      return;
    }
    case 'completeAfter':
      if (stmt.owner !== null) {
        startWait(codegen, stmt.completion, stmt.delayMs, stmt.owner, plan);
      } else {
        callCompletionWith(codegen, stmt.completion, true, plan);
      }
      return;
    case 'startChain': {
      // A loop first records the pass and where the value starts, then step 0 starts here.
      const loop = plan.loops.get(stmt.id);
      if (loop?.passes) {
        codegen.scoped(() =>
          fn.abx('SET32', codegen.load('int', 1).index, loop.passes!.index),
        );
      }
      if (loop?.from) {
        codegen.scoped(() => {
          const from = fn.register('float');
          fn.abx(
            'AVGET',
            from.index,
            plan.value(stmt.loop!.resetHandle!).index,
          );
          fn.abx('SET32', from.index, loop.from!.index);
        });
      }
      startAnimation(codegen, stmt.steps[0], afterStep(stmt, 0, plan), plan);
      return;
    }
    case 'stopAnimations':
      // Each value freezes where it is, which ends its animation as not finished; then the composition's wait.
      for (const handle of stmt.handles) {
        codegen.scoped(() => {
          const value = fn.register('float');
          const slot = plan.value(handle).index;
          fn.abx('AVGET', value.index, slot);
          fn.abx('AVSET', value.index, slot);
        });
      }
      if (stmt.waitOwner !== null && plan.owners.has(stmt.waitOwner)) {
        codegen.scoped(() => {
          const handle = fn.register('anim');
          fn.abx('GET32', handle.index, plan.owner(stmt.waitOwner!).index);
          fn.abc('ASTOP', handle.index);
        });
      }
  }
}

/**
 * Starts a <TouchableOpacity>'s fade, before the app's own handler for the event runs.
 *
 * @param codegen  The handler's codegen.
 * @param fade  The fade.
 * @param plan  The program's animations.
 */
export function startPressFade(
  codegen: Codegen,
  fade: IrPressFade,
  plan: AnimationPlan,
): void {
  codegen.scoped(() => start(codegen, plan.fade(fade), NONE, null));
}

/**
 * The done functions, in order: each one ends the chain or composition when the animation before it was
 * interrupted, calling the completion as not finished, as C's callbacks do.
 *
 * @param plan  The program's animations.
 * @param codegenFor  Makes a function's codegen.
 *
 * @returns The functions.
 */
export function doneFunctions(
  plan: AnimationPlan,
  codegenFor: (fn: FunctionBuilder) => Codegen,
): ProgramFunction[] {
  return [...plan.doneFunctions].map(([name, done]) => {
    const fn = new FunctionBuilder(FunctionKind.animDone, `animation ${name}`);
    const codegen = codegenFor(fn);
    const finished = fn.register('bool');
    fn.abc('EVT', finished.index, EventField.finished);
    if (done.kind === 'completion') {
      callCompletion(codegen, done.completion, finished, plan);
      return fn.finish();
    }

    const completion =
      done.kind === 'wait' ? done.completion : done.chain.completion;
    const go = fn.label();
    const end = fn.label();
    fn.branch('JT', go, finished);
    if (completion) {
      callCompletion(codegen, completion, finished, plan);
    }
    fn.branch('JMP', end);
    fn.place(go);

    if (done.kind === 'wait') {
      startWait(codegen, done.completion, done.delayMs, done.owner, plan);
    } else if (done.kind === 'step') {
      startAnimation(
        codegen,
        done.chain.steps[done.index],
        afterStep(done.chain, done.index, plan),
        plan,
      );
    } else {
      repeat(codegen, done.chain, end, plan);
    }
    fn.place(end);
    return fn.finish();
  });
}

/**
 * A loop's repeat, when its last step finished: it counts the pass, puts the value back, and starts step 0 again.
 *
 * @param codegen  The done function's codegen.
 * @param chain  The chain.
 * @param end  Where the function returns.
 * @param plan  The program's animations.
 */
function repeat(
  codegen: Codegen,
  chain: IrStartChain,
  end: Label,
  plan: AnimationPlan,
): void {
  const {fn} = codegen;
  const loop = chain.loop!;
  const slots = plan.loops.get(chain.id)!;
  if (slots.passes) {
    const passes = slots.passes;
    codegen.scoped(() => {
      const count = fn.register('int');
      const more = fn.register('bool');
      const next = fn.label();
      fn.abx('GET32', count.index, passes.index);
      fn.abc(
        'LT',
        more.index,
        count.index,
        codegen.load('int', loop.iterations).index,
      );
      fn.branch('JT', next, more);
      if (chain.completion) {
        if (loop.finalWait) {
          startWait(
            codegen,
            chain.completion,
            loop.finalWait.delayMs,
            loop.finalWait.owner,
            plan,
          );
        } else {
          callCompletionWith(codegen, chain.completion, true, plan);
        }
      }
      fn.branch('JMP', end);
      fn.place(next);
      fn.abc('ADD', count.index, count.index, codegen.load('int', 1).index);
      fn.abx('SET32', count.index, passes.index);
    });
  }
  if (slots.from) {
    const from = slots.from;
    codegen.scoped(() => {
      const value = fn.register('float');
      fn.abx('GET32', value.index, from.index);
      fn.abx('AVSET', value.index, plan.value(loop.resetHandle!).index);
    });
  }
  startAnimation(codegen, loop.restart, afterStep(chain, 0, plan), plan);
}
