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

// The QuickJS C bridge (native_ui_bridge.c) installs a global `NativeUI` object before the
// bundle runs. Re-export it so the rest of the JS imports it cleanly instead of touching the
// global directly.
import type {KeyboardConfig, ScrollPosition} from './embedded-react/index.d.ts';
import type {HostProps} from './props.mts';

/** A function the bridge runs a frame or a native event through. */
export type Batcher = (
  callback: (firstArgument: unknown, secondArgument: unknown) => unknown,
  firstArgument: unknown,
  secondArgument: unknown,
) => unknown;

/** Where the on-screen keyboard is, in screen pixels. */
export interface KeyboardFrame {
  x: number;
  y: number;
  width: number;
  height: number;
}

/** What native_ui_bridge.c installs. A node or a value is its handle; a failed create is 0. */
export interface NativeUIBridge {
  createNode(type: string): number;
  destroyNode(node: number): void;
  setRoot(node: number): void;
  setProps(node: number, props: HostProps): void;
  setTextSpans(node: number, spans: HostProps[]): void;
  setVectorOps(
    node: number,
    ops: number[] | Float32Array,
    paints: number[],
    gradients?: number[],
    dirtyRect?: number[],
  ): void;
  setEvent(node: number, name: string, handler: Function | null): void;
  appendChild(parent: number, child: number): void;
  insertBefore(parent: number, child: number, beforeChild: number): void;
  removeChild(parent: number, child: number): void;
  commit(): void;
  now(): number;
  // Advances the engine clock and pumps timers, jobs and a frame.
  tick(milliseconds: number): void;
  scrollTo(node: number, x?: number, y?: number): ScrollPosition | undefined;

  animValueCreate(initial: number): number;
  animValueDestroy(value: number): void;
  animValueSet(value: number, to: number): void;
  animValueGet(value: number): number;
  animValueBind(value: number, node: number, prop: string): void;
  animValueBindInterpolated(
    value: number,
    node: number,
    prop: string,
    config: unknown,
  ): void;
  animValueAnimate(
    value: number,
    toValue: number,
    config: HostProps,
    onEnd: (finished: unknown) => void,
  ): number;
  animStop(animation: number): void;
  animUnbind(node: number, prop: string): void;
  configureNextLayoutAnimation(config: HostProps): void;
  hasPendingLayoutAnimation(): boolean;

  keyboardFrame(): KeyboardFrame | undefined;
  setKeyboardListener(listener: (shown: boolean) => void): void;
  setKeyboardConfig(config: KeyboardConfig | null): void;
  dismissKeyboard(): void;

  // Runs after the host changes the `screen` size. `followed` says the app lays out again when it does.
  setScreenListener?(listener: () => void, followed?: boolean): void;

  maxVectorOps: number;
  maxVectorPaints: number;
  maxVectorGrads: number;

  setBatcher?(batcher: Batcher): void;
  // Only on an instrumented build.
  perfRenderBegin?(): void;
  perfRenderEnd?(): void;
  perfCallbackBegin?(): void;
  perfCallbackEnd?(): void;
  perfMarshalBegin?(): void;
  perfMarshalEnd?(): void;
}

declare global {
  var NativeUI: NativeUIBridge;
}

export const NativeUI = globalThis.NativeUI;

if (!NativeUI) {
  throw new Error(
    'NativeUI global is missing — the QuickJS bridge must be installed before the bundle runs.',
  );
}
