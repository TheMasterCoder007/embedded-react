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

// A type-level fixture, compiled by `npm run typecheck`: Keyboard used the way an app uses it.
import {Keyboard} from '../../src/embedded-react/index';
import type {KeyboardEvent} from '../../src/embedded-react/index';

const heights: number[] = [];

export function watchKeyboard() {
  const subscription = Keyboard.addListener(
    'keyboardDidShow',
    (event: KeyboardEvent) => heights.push(event.endCoordinates.height),
  );
  Keyboard.addListener('keyboardDidHide', () => heights.push(0));
  subscription.remove();
  Keyboard.removeAllListeners('keyboardDidHide');

  Keyboard.dismiss();
  const visible: boolean = Keyboard.isVisible();
  const top: number | undefined = Keyboard.metrics()?.screenY;

  // @ts-expect-error not a keyboard event
  Keyboard.addListener('keyboardDidChangeFrame', () => {});
  // @ts-expect-error dismiss takes nothing
  Keyboard.dismiss(true);

  return visible ? top : undefined;
}
