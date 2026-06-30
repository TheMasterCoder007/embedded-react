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

// Phase-2 integration spike: Fast Refresh through the REAL AppRegistry + renderer (not a bare reconciler).
// It models what the dev build produces — components instrumented with react-refresh ($RefreshReg$/
// $RefreshSig$) and the runtime's global hook installed by the vendor — then "reloads" by re-registering a
// new build of the app (a fresh function, same refresh family) and calling performReactRefresh. The win:
// the changed component is swapped IN PLACE with useState preserved, instead of the cold remount Phase 1
// does. renderer.js registers the reconciler with the refresh hook (gated on $RefreshReg$), so this also
// exercises that wiring.

import {describe, it, expect, vi, beforeEach, afterEach} from 'vitest';

function installMockNativeUI() {
  let idc = 0;
  globalThis.IS_REACT_ACT_ENVIRONMENT = false;
  globalThis.screen = {width: 480, height: 320};
  globalThis.NativeUI = {
    createNode: () => ++idc,
    setProps: () => {},
    setRoot: () => {},
    appendChild: () => {},
    insertBefore: () => {},
    removeChild: () => {},
    destroyNode: () => {},
    setEvent: () => {},
    setTextSpans: () => {},
    setVectorOps: () => {},
    commit: () => {},
    now: () => 0,
    maxVectorOps: 4096,
    maxVectorPaints: 1024,
    maxVectorGrads: 256,
  };
}

describe('Fast Refresh integration (AppRegistry + renderer + react-refresh)', () => {
  let RefreshRuntime;
  let probe; // {version, n} written on each render
  let setN; // live setter captured from the mounted component

  beforeEach(async () => {
    installMockNativeUI();
    vi.resetModules();
    probe = null;
    setN = null;

    // Vendor-side setup (what the dev vendor chunk does): the refresh transform's globals + the runtime's
    // global hook, installed BEFORE the reconciler registers with it.
    RefreshRuntime = (await import('react-refresh/runtime')).default;
    globalThis.$RefreshReg$ = (type, id) => RefreshRuntime.register(type, id);
    globalThis.$RefreshSig$ = () =>
      RefreshRuntime.createSignatureFunctionForTransform();
    RefreshRuntime.injectIntoGlobalHook(globalThis);
  });

  afterEach(() => {
    delete globalThis.NativeUI;
    delete globalThis.screen;
    delete globalThis.$RefreshReg$;
    delete globalThis.$RefreshSig$;
  });

  it('swaps the app component in place on reload with useState preserved', async () => {
    const {createElement: el, useState} = (await import('react')).default;
    const {AppRegistry} = await import('../embedded-react/AppRegistry.js');

    // Emulates one app build: a fresh Counter function (new identity each "save"), registered to a STABLE
    // refresh family + signature — exactly what react-refresh/babel emits.
    const buildApp = version => {
      const _s = globalThis.$RefreshSig$();
      function Counter() {
        _s();
        const [n, set] = useState(0);
        setN = set;
        probe = {version, n};
        return el('View', {});
      }
      _s(Counter, 'useState{n}'); // same signature both builds ⇒ state-preserving
      globalThis.$RefreshReg$(Counter, 'app/App.jsx Counter'); // same family ⇒ a swap, not a remount
      return Counter;
    };

    // Boot: mount v1 via the real AppRegistry, then advance its state.
    AppRegistry.registerComponent('demo', () => buildApp('v1'));
    expect(probe).toEqual({version: 'v1', n: 0});
    setN(7);
    expect(probe).toEqual({version: 'v1', n: 7});

    // Reload: re-register the next build (new Counter, same family) and apply the refresh — the device does
    // this after evaluating an app frame.
    AppRegistry.registerComponent('demo', () => buildApp('v2'));
    RefreshRuntime.performReactRefresh();

    // New code ran AND useState survived — an in-place swap, not a cold remount.
    expect(probe).toEqual({version: 'v2', n: 7});
  });

  it('remounts (state resets) when the component hook signature changes', async () => {
    const {createElement: el, useState} = (await import('react')).default;
    const {AppRegistry} = await import('../embedded-react/AppRegistry.js');

    const buildApp = (version, sig, extraHook) => {
      const _s = globalThis.$RefreshSig$();
      function Counter() {
        _s();
        const [n, set] = useState(0);
        if (extraHook) useState('x'); // an added hook in v2
        setN = set;
        probe = {version, n};
        return el('View', {});
      }
      _s(Counter, sig);
      globalThis.$RefreshReg$(Counter, 'app/App.jsx Counter');
      return Counter;
    };

    AppRegistry.registerComponent('demo', () =>
      buildApp('v1', 'useState{n}', false),
    );
    setN(9);
    expect(probe).toEqual({version: 'v1', n: 9});

    AppRegistry.registerComponent('demo', () =>
      buildApp('v2', 'useState{n}useState{}', true),
    );
    RefreshRuntime.performReactRefresh();

    // Hooks changed ⇒ React must remount, so state resets to its initial value.
    expect(probe).toEqual({version: 'v2', n: 0});
  });
});
