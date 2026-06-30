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

// Public renderer API: createRoot(...).render(<App/>).
//
// The container is a real engine View node set as the scene root; React children mount into it.
// We use LegacyRoot (synchronous) mode so the initial render flushes without depending on the
// async scheduler/timers — the host's frame loop then paints whatever the tree became.
import Reconciler from 'react-reconciler';
import {hostConfig} from './host-config.js';
import {NativeUI} from './native-ui.js';

const reconciler = Reconciler(hostConfig);

// Fast Refresh (dev only): when the app was built with the react-refresh transform, the dev vendor sets up
// $RefreshReg$ + react-refresh/runtime's global hook before this module loads. Registering the reconciler
// with that hook lets performReactRefresh find our roots and swap changed components in place. Gated on
// $RefreshReg$ so production (no refresh transform) is untouched — injectIntoDevTools is never called there.
if (typeof globalThis.$RefreshReg$ === 'function') {
  reconciler.injectIntoDevTools({
    bundleType: 1,
    version: '18.3.1',
    rendererPackageName: 'embedded-react',
    findFiberByHostInstance: () => null,
  });
}

const LegacyRoot = 0;

/**
 * Creates a root bound to a screen-sized container node.
 *
 * @param {object} containerProps  Props for the container View (e.g. width/height/backgroundColor).
 * @returns {{ render: (element: any) => void }}
 */
export function createRoot(containerProps) {
  const container = NativeUI.createNode('View');
  NativeUI.setProps(container, containerProps || {});
  NativeUI.setRoot(container);

  const fiberRoot = reconciler.createContainer(
    container, // containerInfo — our root node handle
    LegacyRoot,
    null, // hydration callbacks
    false, // isStrictMode
    null, // concurrentUpdatesByDefaultOverride
    '', // identifierPrefix
    error => console.error('react recoverable error:', error),
    null, // transitionCallbacks
  );

  return {
    render(element) {
      reconciler.updateContainer(element, fiberRoot, null, null);
    },
  };
}
