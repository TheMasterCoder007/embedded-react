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

// When React deletes a subtree it calls the host removeChild only for the subtree's TOP node, never its
// descendants — so the renderer's destroyNode must free the WHOLE subtree, not just that node. If it frees
// only the top, every descendant leaks in the engine's fixed-size node pool. That leak is invisible in
// PSRAM (the pool is a static array, not heap) and only bites once the pool exhausts — e.g. after toggling
// a panel's visibility, or a few incremental hot reloads that tear down and rebuild the tree. The real
// NativeUI.destroyNode recurses (in C: js_destroy_node → destroy_node_and_subtree); this test keeps the
// teardown balanced.

import {describe, it, expect, vi, beforeEach, afterEach} from 'vitest';

// A mock that mirrors the real bridge's tree + teardown: it tracks parent→children links, and destroyNode
// recurses the subtree — exactly as NativeUI.destroyNode now does in C (js_destroy_node →
// destroy_node_and_subtree). React calls the host removeChild only for a deleted subtree's TOP, so this
// is what frees the descendants. If the host config stopped destroying removed subtree tops, the recursive
// destroy would never run and the live set would not return to baseline — which these tests assert.
function installMockNativeUI() {
  let idc = 0;
  const live = new Set();
  const kids = new Map(); // handle → [child handles]
  globalThis.IS_REACT_ACT_ENVIRONMENT = false;
  const destroy = h => {
    if (!live.has(h))
      throw new Error(
        `destroyNode on a non-live node ${h} (double-free or unknown handle)`,
      );
    for (const c of kids.get(h) || []) destroy(c); // recurse the subtree, like the real bridge
    live.delete(h);
    kids.delete(h);
  };
  globalThis.NativeUI = {
    createNode: () => {
      const h = ++idc;
      live.add(h);
      kids.set(h, []);
      return h;
    },
    setProps: () => {},
    setRoot: () => {},
    appendChild: (p, c) => kids.get(p)?.push(c),
    insertBefore: (p, c, before) => {
      const arr = kids.get(p) || [];
      const i = arr.indexOf(before);
      arr.splice(i < 0 ? arr.length : i, 0, c);
    },
    removeChild: (p, c) => {
      const arr = kids.get(p);
      if (arr) arr.splice(arr.indexOf(c), 1);
    },
    destroyNode: destroy,
    setEvent: () => {},
    setTextSpans: () => {},
    setVectorOps: () => {},
    commit: () => {},
    now: () => 0,
    maxVectorOps: 4096,
    maxVectorPaints: 1024,
    maxVectorGrads: 256,
    __live: live,
  };
}

describe('node teardown: deleting a subtree destroys every node in it', () => {
  beforeEach(installMockNativeUI);
  afterEach(() => delete globalThis.NativeUI);

  it('does not leak descendant nodes when a nested subtree is removed', async () => {
    const Reconciler = (await import('react-reconciler')).default;
    const {hostConfig} = await import('../host-config.js');
    const {createElement: el} = (await import('react')).default;
    const reconciler = Reconciler(hostConfig);

    const container = globalThis.NativeUI.createNode('View'); // created outside the reconciler
    const root = reconciler.createContainer(
      container,
      0,
      null,
      false,
      null,
      '',
      () => {},
      null,
    );

    // Mount: A > B > [C, D]. The reconciler creates A, B, C, D (4 nodes; container was created above).
    const tree = el('View', null, el('View', null, el('View'), el('View')));
    reconciler.updateContainer(tree, root, null, null);
    expect(globalThis.NativeUI.__live.size).toBe(5); // container + A + B + C + D

    // Re-render with B's whole subtree gone — React deletes B, C, and D.
    reconciler.updateContainer(el('View'), root, null, null);

    // Only container + A may remain. If destroyNode fired only for B (the subtree top), C and D leak.
    expect(globalThis.NativeUI.__live.size).toBe(2);
  });

  it('cold-swapping the root component destroys the entire previous tree (the hot-reload soft swap)', async () => {
    const Reconciler = (await import('react-reconciler')).default;
    const {hostConfig} = await import('../host-config.js');
    const {createElement: el} = (await import('react')).default;
    const reconciler = Reconciler(hostConfig);

    const container = globalThis.NativeUI.createNode('View');
    const root = reconciler.createContainer(
      container,
      0,
      null,
      false,
      null,
      '',
      () => {},
      null,
    );

    // Two distinct root components (a new function each "reload"), each a small nested tree.
    const AppV1 = () =>
      el('View', null, el('View', null, el('View')), el('View'));
    const AppV2 = () =>
      el('View', null, el('View', null, el('View')), el('View'));

    reconciler.updateContainer(el(AppV1), root, null, null);
    const afterV1 = globalThis.NativeUI.__live.size;

    // Reload: swap the root to a different component type → React unmounts ALL of V1, mounts V2.
    reconciler.updateContainer(el(AppV2), root, null, null);

    // The live count must not grow across the swap — every V1 node is destroyed as V2 mounts.
    expect(globalThis.NativeUI.__live.size).toBe(afterV1);
  });
});
