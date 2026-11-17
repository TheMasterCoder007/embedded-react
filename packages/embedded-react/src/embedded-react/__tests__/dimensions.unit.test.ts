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

// Dimensions over a mocked NativeUI: what the bridge's one listener fans out to.
import {beforeEach, describe, expect, it, vi} from 'vitest';
import type {Dimensions as DimensionsModule} from '../index';

let bridgeListener: (() => void) | undefined;
let Dimensions: typeof DimensionsModule;

/** Changes the mocked panel's size, the way the host tells the bridge. */
function setScreen(width: number, height: number) {
  vi.stubGlobal('screen', {width, height});
  bridgeListener?.();
}

beforeEach(async () => {
  bridgeListener = undefined;
  vi.unstubAllGlobals();
  vi.stubGlobal('screen', {width: 240, height: 320});
  vi.stubGlobal('NativeUI', {
    setScreenListener: (listener: () => void) => {
      bridgeListener = listener;
    },
  });
  vi.resetModules();
  ({Dimensions} = await import('../Dimensions.mts'));
});

describe('Dimensions', () => {
  it('reports the panel size at a scale of 1', () => {
    const size = {width: 240, height: 320, scale: 1, fontScale: 1};
    expect(Dimensions.get('window')).toEqual(size);
    expect(Dimensions.get('screen')).toEqual(size);
  });

  it('reads the panel each time', () => {
    Dimensions.get('window');
    vi.stubGlobal('screen', {width: 480, height: 272});
    expect(Dimensions.get('window').width).toBe(480);
  });

  it('refuses a size it does not have', () => {
    // @ts-expect-error -- only 'window' and 'screen' name a size
    expect(() => Dimensions.get('display')).toThrow(/display/);
  });

  it('fires change with the new size', () => {
    const sizes: string[] = [];
    Dimensions.addEventListener('change', ({window, screen}) =>
      sizes.push(`${window.width}x${window.height} ${screen.width}`),
    );

    setScreen(320, 240);

    expect(sizes).toEqual(['320x240 320']);
  });

  it('stops calling a removed listener, and lets one remove itself', () => {
    let calls = 0;
    const first = Dimensions.addEventListener('change', () => {
      calls++;
      first.remove();
    });
    const second = Dimensions.addEventListener('change', () => calls++);

    setScreen(320, 240);
    second.remove();
    setScreen(240, 320);

    expect(calls).toBe(2);
  });

  it('refuses an event it does not have', () => {
    expect(() =>
      // @ts-expect-error -- 'change' is the only event
      Dimensions.addEventListener('resize', () => {}),
    ).toThrow(/resize/);
  });

  it('runs on a host with no screen listener', () => {
    vi.stubGlobal('NativeUI', {});
    expect(() => Dimensions.addEventListener('change', () => {})).not.toThrow();
  });
});
