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
import { createElement } from 'react';
import { createRoot } from '../renderer.js';

let registered = null;

export const AppRegistry = {
  /**
   * Registers a root component and mounts it. `componentProvider` is a zero-arg function returning
   * the component (RN signature), so the component module isn't evaluated until registration.
   */
  registerComponent(appKey, componentProvider) {
    registered = { appKey, Component: componentProvider() };
    AppRegistry.runApplication(appKey);
    return appKey;
  },

  /**
   * Mounts the registered root component into a fresh screen-sized container.
   */
  runApplication(appKey) {
    if (!registered) return;
    if (appKey && appKey !== registered.appKey) return;
    const root = createRoot({ width: screen.width, height: screen.height });
    root.render(createElement(registered.Component));
  },
};
