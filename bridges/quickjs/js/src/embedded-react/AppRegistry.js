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

// AppRegistry — the RN entry point. An app registers a root component; the runtime mounts it into
// a screen-sized container.
//
// In React Native the native side calls runApplication when the activity starts. Our host model is
// "running the bundle IS starting the app", so registerComponent mounts immediately into a root
// sized from the host-injected `screen` global. (A host-driven runApplication can be split out
// later when the C host owns app lifecycle.)
import {createElement} from 'react';
import {createRoot} from '../renderer.js';

let registered = null;
// The mounted root is created ONCE and reused on every subsequent registration. On a full reload, the
// whole context is torn down and recreated, so this is recreated too (a fresh boot). But on an
// incremental "soft" reload, only the app chunk is re-evaluated into the SAME context — this lib (and
// therefore `root`) stays resident, so re-registering renders the new app into the existing root and
// React reconciles old→new in place. Creating a new root each time would instead leak a fiber root per
// reload and orphan the previous tree's effects.
let root = null;

export const AppRegistry = {
  /**
   * Registers a root component and mounts it. `componentProvider` is a zero-arg function returning
   * the component (RN signature), so the component module isn't evaluated until registration.
   */
  registerComponent(appKey, componentProvider) {
    registered = {appKey, Component: componentProvider()};
    AppRegistry.runApplication(appKey);
    return appKey;
  },

  /**
   * Mounts the registered root component into the screen-sized container, creating it on first use and
   * reusing it thereafter (see `root` above).
   */
  runApplication(appKey) {
    if (!registered) return;
    if (appKey && appKey !== registered.appKey) return;
    if (!root) {
      // First mount.
      root = createRoot({width: screen.width, height: screen.height});
      root.render(createElement(registered.Component));
      return;
    }
    // Already mounted — this is a reload (the app chunk re-evaluated). With Fast Refresh, the changed
    // component families have just re-registered; performReactRefresh (applied by the dev runtime once the
    // app frame finishes evaluating) swaps them IN PLACE with state preserved. Re-rendering the root here
    // would instead cold-remount the new function and throw that state away, so we don't. Without Fast
    // Refresh, re-render into the reused root — the Phase 1 cold reload (state survives via usePersistentState).
    if (typeof globalThis.$RefreshReg$ !== 'function') {
      root.render(createElement(registered.Component));
    }
  },
};
