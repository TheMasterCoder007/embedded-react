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

// The lib half of the incremental "soft" reload: when only the app chunk is re-evaluated into a resident
// context, re-registering a (new) root component must reuse the existing root — React reconciles old→new
// in place rather than leaking a fresh root each reload — and usePersistentState must survive, because the
// __erPersist store lives in the context, not the app chunk. Mirrors what the device does when it swaps
// just the app section and keeps the vendor resident.

import {describe, it, expect, vi, beforeEach, afterEach} from 'vitest';

let setRootCount;
let persistStore;

function installMocks() {
  let idc = 0;
  setRootCount = 0;
  persistStore = new Map();
  globalThis.IS_REACT_ACT_ENVIRONMENT = false;
  globalThis.screen = {width: 480, height: 320};
  globalThis.__erPersist = {
    get: k => persistStore.get(k),
    set: (k, v) => persistStore.set(k, v),
  };
  globalThis.NativeUI = {
    createNode: type => ({id: ++idc, type, props: {}, children: []}),
    setProps: (h, p) => {
      h.props = p;
    },
    setRoot: () => {
      setRootCount++;
    },
    appendChild: (p, c) => p.children.push(c),
    insertBefore: (p, c, before) => {
      const i = p.children.indexOf(before);
      p.children.splice(i < 0 ? p.children.length : i, 0, c);
    },
    removeChild: (p, c) => {
      const i = p.children.indexOf(c);
      if (i >= 0) p.children.splice(i, 1);
    },
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

describe('AppRegistry soft reload (resident root + persisted state)', () => {
  beforeEach(() => {
    vi.resetModules(); // fresh AppRegistry/renderer/reconciler module state per test
    installMocks();
  });
  afterEach(() => {
    delete globalThis.NativeUI;
    delete globalThis.screen;
    delete globalThis.__erPersist;
  });

  it('reuses the root and preserves usePersistentState across an app-chunk reload', async () => {
    const {createElement: el} = (await import('react')).default;
    const {AppRegistry} = await import('../AppRegistry.js');
    const {usePersistentState} = await import('../usePersistentState.js');

    let seen = null;
    let setN = null;

    // "App v1" — the running app.
    function AppV1() {
      const [n, set] = usePersistentState('counter', 0);
      setN = set;
      seen = {version: 'v1', n};
      return el('View', {});
    }
    AppRegistry.registerComponent('demo', () => AppV1);
    expect(seen).toEqual({version: 'v1', n: 0});
    expect(setRootCount).toBe(1); // root created once

    // User interacts: bump the persisted value (writes through to the __erPersist store).
    setN(1);
    expect(JSON.parse(persistStore.get('counter'))).toBe(1);

    // "Edit + save" — only the app chunk re-evaluates; AppRegistry is resident, so re-register a NEW
    // component. This is the soft reload.
    function AppV2() {
      const [n] = usePersistentState('counter', 0);
      seen = {version: 'v2', n};
      return el('View', {});
    }
    AppRegistry.registerComponent('demo', () => AppV2);

    // New code ran (v2), persisted state survived (n === 1), and the root was NOT recreated.
    expect(seen).toEqual({version: 'v2', n: 1});
    expect(setRootCount).toBe(1);
  });
});
