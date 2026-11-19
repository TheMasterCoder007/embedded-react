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

// Dimensions — RN analog over the host-injected `screen` global. A panel has no pixel-density or font
// setting, so `scale` and `fontScale` are 1, and 'window' and 'screen' are the same size. The host can change
// the size while the app runs (a live rotation, a simulator resize), which fires 'change'.
import {useSyncExternalStore} from 'react';
import {NativeUI} from '../native-ui.mts';
import type {
  Dimensions as PublicDimensions,
  DimensionsChange,
  ScaledSize,
} from './index.d.ts';

const listeners = new Set<(sizes: DimensionsChange) => void>();
// How many of them the app added. With one or more, the app follows a size change.
let appListenerCount = 0;
// The size last read, kept so the hook gets the same object until the size changes.
let current: ScaledSize | null = null;

function currentSize(): ScaledSize {
  if (
    !current ||
    current.width !== screen.width ||
    current.height !== screen.height
  ) {
    current = {
      width: screen.width,
      height: screen.height,
      scale: 1,
      fontScale: 1,
    };
  }

  return current;
}

function onScreenChange(): void {
  const size = currentSize();
  // A copy, so a listener can remove itself.
  for (const listener of [...listeners]) {
    listener({window: size, screen: size});
  }
}

// Hands the bridge the listener, and whether the app follows a size change.
function tellBridge(): void {
  NativeUI.setScreenListener?.(onScreenChange, appListenerCount > 0);
}

// The library's own listeners: they don't say the app follows a size change.
export function onScreenSize(
  listener: (sizes: DimensionsChange) => void,
): () => void {
  if (listeners.size === 0) tellBridge();
  listeners.add(listener);
  return () => listeners.delete(listener);
}

function subscribe(listener: (sizes: DimensionsChange) => void): () => void {
  const remove = onScreenSize(listener);
  let removed = false;
  appListenerCount++;
  if (appListenerCount === 1) tellBridge();
  return () => {
    if (removed) return;
    removed = true;
    remove();
    appListenerCount--;
    if (appListenerCount === 0) tellBridge();
  };
}

export const Dimensions: typeof PublicDimensions = {
  get(dimension) {
    if (dimension !== 'window' && dimension !== 'screen') {
      throw new Error(`Dimensions.get: no dimension named "${dimension}"`);
    }

    return {...currentSize()};
  },

  addEventListener(type, handler) {
    if (type !== 'change') {
      throw new Error(`Dimensions.addEventListener: no event named "${type}"`);
    }

    return {remove: subscribe(handler)};
  },
};

// The panel's size. A component that reads it renders again when the size changes.
export function useWindowDimensions(): ScaledSize {
  return useSyncExternalStore(subscribe, currentSize);
}

// The same size for the library's own components.
export function useScreenSize(): ScaledSize {
  return useSyncExternalStore(onScreenSize, currentSize);
}
