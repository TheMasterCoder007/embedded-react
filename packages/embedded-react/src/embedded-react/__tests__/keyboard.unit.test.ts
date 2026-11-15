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

// Keyboard over a mocked NativeUI: what the bridge's one listener fans out to.
import {beforeEach, describe, expect, it, vi} from 'vitest';
import type {Keyboard as KeyboardModule} from '../index';

interface Frame {
  x: number;
  y: number;
  width: number;
  height: number;
}

const FRAME: Frame = {x: 0, y: 200, width: 240, height: 120};

let frame: Frame | undefined;
let bridgeListener: ((shown: boolean) => void) | undefined;
let dismissals: number;
let Keyboard: typeof KeyboardModule;

/** Shows or hides the mocked keyboard, the way the engine tells the bridge. */
function setShown(shown: boolean) {
  frame = shown ? FRAME : undefined;
  bridgeListener?.(shown);
}

beforeEach(async () => {
  frame = undefined;
  bridgeListener = undefined;
  dismissals = 0;
  (globalThis as Record<string, unknown>).NativeUI = {
    setKeyboardListener: (listener: (shown: boolean) => void) => {
      bridgeListener = listener;
    },
    dismissKeyboard: () => {
      dismissals++;
    },
    keyboardFrame: () => frame,
  };
  vi.resetModules();
  ({Keyboard} = await import('../Keyboard.js'));
});

describe('Keyboard', () => {
  it('fires will then did, with the strip the keyboard covers', () => {
    const calls: string[] = [];
    for (const name of [
      'keyboardDidShow',
      'keyboardWillShow',
      'keyboardDidHide',
      'keyboardWillHide',
    ] as const) {
      Keyboard.addListener(name, event =>
        calls.push(`${name} ${event.endCoordinates.screenY}`),
      );
    }

    setShown(true);
    setShown(false);

    expect(calls).toEqual([
      'keyboardWillShow 200',
      'keyboardDidShow 200',
      'keyboardWillHide 320',
      'keyboardDidHide 320',
    ]);
  });

  it('stops calling a removed listener', () => {
    const kept = vi.fn();
    const removed = vi.fn();
    Keyboard.addListener('keyboardDidShow', kept);
    Keyboard.addListener('keyboardDidShow', removed).remove();
    setShown(true);
    expect(kept).toHaveBeenCalledTimes(1);
    expect(removed).not.toHaveBeenCalled();

    Keyboard.removeAllListeners('keyboardDidShow');
    setShown(false);
    setShown(true);
    expect(kept).toHaveBeenCalledTimes(1);
  });

  it('lets a listener remove itself while the event fires', () => {
    const second = vi.fn();
    const first = Keyboard.addListener('keyboardDidShow', () => first.remove());
    Keyboard.addListener('keyboardDidShow', second);
    setShown(true);
    setShown(false);
    setShown(true);
    expect(second).toHaveBeenCalledTimes(2);
  });

  it('subscribes to the bridge only once a listener is added', () => {
    expect(bridgeListener).toBeUndefined();
    Keyboard.addListener('keyboardDidHide', () => {});
    expect(bridgeListener).toBeDefined();
  });

  it('refuses an event it does not have', () => {
    expect(() =>
      // @ts-expect-error -- not a keyboard event
      Keyboard.addListener('keyboardDidChangeFrame', () => {}),
    ).toThrow(/keyboardDidChangeFrame/);
  });

  it('dismisses through the bridge', () => {
    Keyboard.dismiss();
    expect(dismissals).toBe(1);
  });

  it('reads visibility and metrics from the bridge', () => {
    expect(Keyboard.isVisible()).toBe(false);
    expect(Keyboard.metrics()).toBeUndefined();
    setShown(true);
    expect(Keyboard.isVisible()).toBe(true);
    expect(Keyboard.metrics()).toEqual({
      screenX: 0,
      screenY: 200,
      width: 240,
      height: 120,
    });
  });
});
