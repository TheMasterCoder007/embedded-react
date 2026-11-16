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

// Keyboard — RN's Keyboard over the engine's on-screen keyboard, which shows while a <TextInput> has the
// focus. A build without it (ERUI_ONSCREEN_KEYBOARD=0) has no keyboard to report: dismiss() still takes
// the focus away, but no event fires.
import {NativeUI} from '../native-ui.mts';
import type {KeyboardFrame} from '../native-ui.mts';
import type {
  Keyboard as PublicKeyboard,
  KeyboardEvent,
  KeyboardEventName,
  KeyboardMetrics,
} from './index.d.ts';

type Listeners = Set<(event: KeyboardEvent) => void>;

const listeners: Record<KeyboardEventName, Listeners> = {
  keyboardWillShow: new Set(),
  keyboardDidShow: new Set(),
  keyboardWillHide: new Set(),
  keyboardDidHide: new Set(),
};

let subscribed = false;
// Where the keyboard last showed, for the hide events.
let lastFrame = {x: 0, y: 0, width: 0, height: 0};

const coordinates = (frame: KeyboardFrame): KeyboardMetrics => ({
  screenX: frame.x,
  screenY: frame.y,
  width: frame.width,
  height: frame.height,
});

function emit(eventName: KeyboardEventName, event: KeyboardEvent): void {
  // A copy, so a listener can remove itself.
  for (const listener of [...listeners[eventName]]) listener(event);
}

function onKeyboardChange(shown: boolean): void {
  if (shown) {
    lastFrame = NativeUI.keyboardFrame() ?? lastFrame;
    const event = {endCoordinates: coordinates(lastFrame)};
    emit('keyboardWillShow', event);
    emit('keyboardDidShow', event);
  } else {
    // It ends just below the screen.
    const hidden = {...lastFrame, y: lastFrame.y + lastFrame.height};
    const event = {endCoordinates: coordinates(hidden)};
    emit('keyboardWillHide', event);
    emit('keyboardDidHide', event);
  }
}

export const Keyboard: typeof PublicKeyboard = {
  // Nothing animates, so each "will" event fires just before its "did".
  addListener(eventName, listener) {
    const set = listeners[eventName];
    if (!set) {
      throw new Error(`Keyboard.addListener: no event named "${eventName}"`);
    }
    if (!subscribed) {
      NativeUI.setKeyboardListener(onKeyboardChange);
      subscribed = true;
    }
    set.add(listener);
    return {remove: () => set.delete(listener)};
  },

  removeAllListeners(eventName) {
    listeners[eventName]?.clear();
  },

  dismiss() {
    NativeUI.dismissKeyboard();
  },

  isVisible() {
    return NativeUI.keyboardFrame() !== undefined;
  },

  metrics() {
    const frame = NativeUI.keyboardFrame();
    return frame ? coordinates(frame) : undefined;
  },
};
