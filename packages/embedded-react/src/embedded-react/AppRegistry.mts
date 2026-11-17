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
// sized from the host-injected `screen` global, which it follows when the host changes it. (A host-driven
// runApplication can be split out later when the C host owns app lifecycle.)
import {createElement, Fragment} from 'react';
import type {ComponentType} from 'react';
import {createRoot} from '../renderer.mts';
import {AlertHost} from './Alert.mts';
import {Dimensions} from './Dimensions.mts';

let registered: {appKey: string; Component: ComponentType} | null = null;
// The mounted root is created ONCE and reused on every subsequent registration. On a full reload, the
// whole context is torn down and recreated, so this is recreated too (a fresh boot). But on an
// incremental "soft" reload, only the app chunk is re-evaluated into the SAME context — this lib (and
// therefore `root`) stays resident, so re-registering renders the new app into the existing root and
// React reconciles old→new in place. Creating a new root each time would instead leak a fiber root per
// reload and orphan the previous tree's effects.
let root: ReturnType<typeof createRoot> | null = null;

export const AppRegistry = {
  /**
   * Registers a root component and mounts it. `componentProvider` is a zero-arg function returning
   * the component (RN signature), so the component module isn't evaluated until registration.
   */
  registerComponent(
    appKey: string,
    componentProvider: () => ComponentType,
  ): string {
    registered = {appKey, Component: componentProvider()};
    AppRegistry.runApplication(appKey);
    return appKey;
  },

  /**
   * Mounts the registered root component into the screen-sized container, creating it on first use and
   * reusing it thereafter (see `root` above).
   */
  runApplication(appKey?: string): void {
    if (!registered) return;
    if (appKey && appKey !== registered.appKey) return;
    if (!root) {
      const mounted = createRoot({width: screen.width, height: screen.height});
      root = mounted;
      // The root follows the screen. This all runs in one batch, so the app never draws at the old size.
      Dimensions.addEventListener('change', ({window}) =>
        mounted.setContainerProps({width: window.width, height: window.height}),
      );
    }
    // The alert host comes after the app, so an alert draws over it.
    root.render(
      createElement(
        Fragment,
        null,
        createElement(registered.Component),
        createElement(AlertHost),
      ),
    );
  },
};
